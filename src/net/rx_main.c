/*
 * rx_main.c — Rx 스테이지 + 드레인 소비자 (2스레드 SPSC 연결 확인용)
 *
 *   [udp_replay_send] ──UDP──▶ Rx(코어 0) ──rx_queue──▶ drain(코어 1)
 *
 * 소비자는 아직 디코드/북이 아니라 개수만 세는 자리채움이다. 여기서 보고 싶은 것:
 *   1) 큐를 건너간 메시지가 전부 PLU인가 (필터가 Rx에서 제대로 걸렸는가)
 *   2) push 수 == pop 수 (슬롯 대여 API가 SPSC로 성립하는가)
 *   3) push→pop 구간이 TSC로 몇 사이클인가 (스테이지 경계 비용의 첫 기준선)
 *
 * Run: rx_stage [port]   — 다른 셸에서 test/udp/udp_replay_send 실행
 */
#define _GNU_SOURCE
#include <ll/rx_stage.h>
#include <ll/tsc.h>

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RX_CPU 0
#define DRAIN_CPU 1
#define LAT_CAP (8u * 1024 * 1024)

static rx_queue_t g_queue;
static atomic_int g_rx_done;

typedef struct {
    uint64_t popped;
    uint64_t buy, sell, other;
    uint64_t deletes;      /* size == 0 = 레벨 제거 */
    uint64_t complete;     /* Event Processing Complete = BBO 발행 시점 */
    uint64_t after_gap;
    uint64_t *lat;         /* push→pop 사이클 (핫패스 밖에서 미리 할당) */
    uint64_t lat_n;
} drain_stats_t;

static void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void fmt_sym(const char *p, char *out) {
    int len = 8;
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == 0)) len--;
    memcpy(out, p, len);
    out[len] = 0;
}

static void *drain_thread(void *arg) {
    drain_stats_t *st = (drain_stats_t *)arg;
    pin_to_cpu(DRAIN_CPU);

    int shown = 0;
    for (;;) {
        rx_msg_t *m = rx_queue_pop_begin(&g_queue);
        if (!m) {
            /* 생산자 종료 확인 후 한 번 더 훑어야 마지막 커밋을 놓치지 않는다 */
            if (atomic_load_explicit(&g_rx_done, memory_order_acquire)) {
                if (!rx_queue_pop_begin(&g_queue)) break;
                continue;
            }
            __builtin_ia32_pause();
            continue;
        }

        if (st->lat_n < LAT_CAP) {
            st->lat[st->lat_n++] = ll_rdtsc() - m->rx_tsc;
        }
        st->popped++;

        uint8_t type = m->plu.type;
        if (type == DEEP_PLU_BUY) st->buy++;
        else if (type == DEEP_PLU_SELL) st->sell++;
        else st->other++;  /* 0이 아니면 Rx 필터가 샌 것 */

        if (m->plu.size == 0) st->deletes++;
        if (DEEP_EVENT_COMPLETE(m->plu.flags)) st->complete++;
        if (m->rx_flags & RX_FLAG_AFTER_GAP) st->after_gap++;

        if (shown < 10) {
            char sym[16];
            fmt_sym(m->plu.symbol, sym);
            int64_t px = m->plu.price;
            printf("  [%" PRId64 "] %-4s %-8s size=%-8u price=%" PRId64 ".%04" PRId64 "%s\n",
                   m->seq, type == DEEP_PLU_BUY ? "BUY" : "SELL", sym, m->plu.size,
                   px / 10000, (px < 0 ? -px : px) % 10000,
                   DEEP_EVENT_COMPLETE(m->plu.flags) ? "  [complete]" : "");
            shown++;
        }

        rx_queue_pop_end(&g_queue);
        /* pop_end 이후 m을 만지지 않는다 — 생산자가 이 슬롯을 재사용할 수 있다 */
    }
    return NULL;
}

static void print_lat(drain_stats_t *st) {
    if (st->lat_n == 0) {
        return;
    }
    qsort(st->lat, st->lat_n, sizeof(uint64_t), cmp_u64);
    printf("\n== push→pop 구간 (TSC 사이클, n=%" PRIu64 ") ==\n", st->lat_n);
    printf("  p50   : %" PRIu64 "\n", st->lat[(size_t)(0.50 * (st->lat_n - 1))]);
    printf("  p99   : %" PRIu64 "\n", st->lat[(size_t)(0.99 * (st->lat_n - 1))]);
    printf("  p99.9 : %" PRIu64 "\n", st->lat[(size_t)(0.999 * (st->lat_n - 1))]);
    printf("  (Rx가 데이터그램 단위로 찍은 rx_tsc 기준이라 같은 세그먼트의\n"
           "   뒤쪽 메시지일수록 커진다 — 큐 핸드오프만의 값이 아님)\n");
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9004;

    rx_queue_init(&g_queue);
    printf("rx_msg_t=%zuB  슬롯 %d개  링 %zuKB\n", sizeof(rx_msg_t), RX_QUEUE_CAPACITY,
           sizeof(g_queue) / 1024);

    int fd = rx_open_socket(port, 2);
    if (fd < 0) {
        perror("rx_open_socket");
        return 1;
    }

    drain_stats_t ds = {0};
    ds.lat = malloc(LAT_CAP * sizeof(uint64_t));
    if (!ds.lat) {
        perror("malloc");
        return 1;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, drain_thread, &ds) != 0) {
        perror("pthread_create");
        return 1;
    }

    printf("UDP DEEP Rx listen :%d  (2초 idle 시 종료)\n\n", port);
    fflush(stdout);

    pin_to_cpu(RX_CPU);
    rx_stats_t rs = {0};
    rx_stage_run(fd, &g_queue, &rs);
    atomic_store_explicit(&g_rx_done, 1, memory_order_release);

    pthread_join(th, NULL);
    close(fd);

    printf("\n== Rx 스테이지 ==\n");
    printf("datagrams   : %" PRIu64 "  (비DEEP %" PRIu64 ")\n", rs.datagrams, rs.non_deep);
    printf("heartbeats  : %" PRIu64 "\n", rs.heartbeats);
    printf("messages    : %" PRIu64 "\n", rs.messages);
    printf("  큐로 push : %" PRIu64 "\n", rs.pushed);
    printf("  필터 제외 : %" PRIu64 "  (%.1f%% — 큐를 안 건넌 분량)\n", rs.filtered,
           rs.messages ? 100.0 * (double)rs.filtered / (double)rs.messages : 0.0);
    printf("  큐 가득 드롭: %" PRIu64 "\n", rs.dropped_full);
    printf("잘린 세그먼트: %" PRIu64 "\n", rs.truncated);
    printf("seq GAP     : %" PRIu64 "  (누락 추정 %" PRIu64 " msg)\n", rs.gaps, rs.gap_msgs);
    printf("reorder     : %" PRIu64 "\n", rs.reorders);

    printf("\n== 드레인 스테이지 ==\n");
    printf("popped      : %" PRIu64 "\n", ds.popped);
    printf("  BUY/SELL  : %" PRIu64 " / %" PRIu64 "\n", ds.buy, ds.sell);
    printf("  PLU 아님  : %" PRIu64 "  (0이어야 정상)\n", ds.other);
    printf("  size==0   : %" PRIu64 "  (레벨 제거)\n", ds.deletes);
    printf("  complete  : %" PRIu64 "  (BBO 발행 시점)\n", ds.complete);
    printf("  갭 직후   : %" PRIu64 "\n", ds.after_gap);

    print_lat(&ds);

    printf("\n%s\n", (ds.popped == rs.pushed && ds.other == 0)
                         ? "=> push==pop, 큐에는 PLU만. 스테이지 연결 확인."
                         : "=> 불일치! 큐 핸드오프 또는 필터 점검 필요.");
    free(ds.lat);
    return 0;
}
