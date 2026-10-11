/*
 * rx_main.c — Rx 스테이지 + 디코드/북/BBO 소비자 (진행순서 2단계)
 *
 *   [udp_replay_send] ──UDP──▶ Rx(코어 0) ──rx_queue──▶ decode+book+BBO(코어 1)
 *
 * 이전까지 소비자는 개수만 세는 자리채움이었다. 여기서는 book_offline.c가
 * 정답지 대조로 확정한 디코드/북/BBO 로직을 그대로 가져와 라이브 경로에 태운다
 * (deep_decode / book_apply / book_bbo — 공유 코드, 복붙 아님).
 *
 * BBO 검증: stdout에는 BBO TSV만 쓴다 (book_offline --bbo와 같은 포맷) — 그래야
 * 같은 diff 방법으로 오프라인 결과와 대조할 수 있다. 통계/로그는 stderr.
 * drop-newest 정책 때문에 완전 일치는 보장되지 않는다 — dropped_full만큼만
 * 어긋나는지가 확인 대상 (CLAUDE.md 다음 작업 순서 1번).
 *
 * Run: rx_stage [port] [--bbo] [--symbol SYM]   — 다른 셸에서 udp_replay_send 실행
 */
#define _GNU_SOURCE
#include <ll/book.h>
#include <ll/deep_decode.h>
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

/* book_offline.c와 같은 전역 사전 할당 — book_set_t가 19MB라 BSS에 둔다 */
static ll_symtab_t g_symtab;
static book_set_t g_books;
static deep_stats_t g_dec;

static int g_emit_bbo = 0;
static uint64_t g_filter_key = 0; /* 0 = 전 심볼 */

typedef struct {
    uint64_t popped;
    uint64_t buy, sell, other;
    uint64_t deletes;   /* size == 0 = 레벨 제거 */
    uint64_t epc;       /* Event Processing Complete = BBO 발행 시점 */
    uint64_t bbo_lines;
    uint64_t after_gap;
    uint64_t *lat;      /* push→pop 사이클 (핫패스 밖에서 미리 할당) */
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

/* 고정소수점 → 사람이 읽는 십진. 출력 지점에서만 한다 (북은 정수로만 다룬다) */
static void fmt_px(int64_t px, char *out, size_t n) {
    int neg = px < 0;
    uint64_t a = neg ? (uint64_t)(-px) : (uint64_t)px;
    snprintf(out, n, "%s%" PRIu64 ".%04" PRIu64, neg ? "-" : "", a / 10000, a % 10000);
}

/* book_offline.c의 on_update와 동일한 발행 조건 — epc가 서야 책이 일관 상태다 */
static void emit_bbo(const deep_update_t *u, drain_stats_t *st) {
    st->epc++;
    if (!g_emit_bbo) {
        return;
    }
    if (g_filter_key && g_symtab.name[u->sym] != g_filter_key) {
        return;
    }

    bbo_t bbo;
    book_bbo(&g_books, u->sym, &bbo);

    char sym[9], bp[32], ap[32];
    ll_symtab_name(&g_symtab, u->sym, sym);
    fmt_px(bbo.bid_px, bp, sizeof bp);
    fmt_px(bbo.ask_px, ap, sizeof ap);

    printf("%" PRId64 "\t%s\t%s\t%u\t%s\t%u\n", u->seq, sym, bbo.has_bid ? bp : "-",
           bbo.bid_sz, bbo.has_ask ? ap : "-", bbo.ask_sz);
    st->bbo_lines++;
}

static void *drain_thread(void *arg) {
    drain_stats_t *st = (drain_stats_t *)arg;
    pin_to_cpu(DRAIN_CPU);

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
        else st->other++; /* 0이 아니면 Rx 필터가 샌 것 */
        if (m->plu.size == 0) st->deletes++;
        if (m->rx_flags & RX_FLAG_AFTER_GAP) st->after_gap++;

        /* decode는 슬롯이 아직 살아있는 동안 끝낸다 — book_apply는 pop_end 뒤로
           미뤄도 된다 (u가 이미 로컬 복사본이라 m을 더 안 본다) */
        deep_update_t u;
        int ok = deep_decode(m, &g_symtab, &u, &g_dec);
        rx_queue_pop_end(&g_queue);
        /* pop_end 이후 m을 만지지 않는다 — 생산자가 이 슬롯을 재사용할 수 있다 */

        if (!ok) {
            continue; /* 심볼 테이블이 꽉 찼을 때뿐 — book_offline과 동일 처리 */
        }
        book_apply(&g_books, &u);
        if (u.epc) {
            emit_bbo(&u, st);
        }
    }
    return NULL;
}

static void print_lat(drain_stats_t *st) {
    if (st->lat_n == 0) {
        return;
    }
    qsort(st->lat, st->lat_n, sizeof(uint64_t), cmp_u64);
    fprintf(stderr, "\n== push→pop 구간 (TSC 사이클, n=%" PRIu64 ") ==\n", st->lat_n);
    fprintf(stderr, "  p50   : %" PRIu64 "\n", st->lat[(size_t)(0.50 * (st->lat_n - 1))]);
    fprintf(stderr, "  p99   : %" PRIu64 "\n", st->lat[(size_t)(0.99 * (st->lat_n - 1))]);
    fprintf(stderr, "  p99.9 : %" PRIu64 "\n", st->lat[(size_t)(0.999 * (st->lat_n - 1))]);
    fprintf(stderr, "  (Rx가 데이터그램 단위로 찍은 rx_tsc 기준이라 같은 세그먼트의\n"
                    "   뒤쪽 메시지일수록 커진다 — 큐 핸드오프만의 값이 아님)\n");
}

int main(int argc, char **argv) {
    int port = 9004;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bbo") == 0) {
            g_emit_bbo = 1;
        } else if (strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
            /* 심볼은 와이어에서 공백패딩 8바이트 — 같은 모양으로 만들어야 키가 맞는다 */
            char padded[8];
            memset(padded, ' ', sizeof padded);
            size_t len = strlen(argv[++i]);
            if (len > 8) len = 8;
            memcpy(padded, argv[i], len);
            g_filter_key = ll_sym_key(padded);
            g_emit_bbo = 1;
        } else {
            port = atoi(argv[i]);
        }
    }

    rx_queue_init(&g_queue);
    ll_symtab_init(&g_symtab);
    book_set_init(&g_books);
    fprintf(stderr, "rx_msg_t=%zuB  슬롯 %d개  링 %zuKB\n", sizeof(rx_msg_t), RX_QUEUE_CAPACITY,
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

    fprintf(stderr, "UDP DEEP Rx listen :%d  (2초 idle 시 종료)\n\n", port);

    pin_to_cpu(RX_CPU);
    rx_stats_t rs = {0};
    rx_stage_run(fd, &g_queue, &rs);
    atomic_store_explicit(&g_rx_done, 1, memory_order_release);

    pthread_join(th, NULL);
    close(fd);

    FILE *o = stderr; /* stdout은 BBO 전용 — book_offline --bbo와 같은 diff에 물릴 수 있게 */
    fprintf(o, "\n== Rx 스테이지 ==\n");
    fprintf(o, "datagrams   : %" PRIu64 "  (비DEEP %" PRIu64 ")\n", rs.datagrams, rs.non_deep);
    fprintf(o, "heartbeats  : %" PRIu64 "\n", rs.heartbeats);
    fprintf(o, "messages    : %" PRIu64 "\n", rs.messages);
    fprintf(o, "  큐로 push : %" PRIu64 "\n", rs.pushed);
    fprintf(o, "  필터 제외 : %" PRIu64 "  (%.1f%% — 큐를 안 건넌 분량)\n", rs.filtered,
            rs.messages ? 100.0 * (double)rs.filtered / (double)rs.messages : 0.0);
    fprintf(o, "  큐 가득 드롭: %" PRIu64 "\n", rs.dropped_full);
    fprintf(o, "잘린 세그먼트: %" PRIu64 "\n", rs.truncated);
    fprintf(o, "seq GAP     : %" PRIu64 "  (누락 추정 %" PRIu64 " msg)\n", rs.gaps, rs.gap_msgs);
    fprintf(o, "reorder     : %" PRIu64 "\n", rs.reorders);

    fprintf(o, "\n== 드레인 스테이지 ==\n");
    fprintf(o, "popped      : %" PRIu64 "\n", ds.popped);
    fprintf(o, "  BUY/SELL  : %" PRIu64 " / %" PRIu64 "\n", ds.buy, ds.sell);
    fprintf(o, "  PLU 아님  : %" PRIu64 "  (0이어야 정상)\n", ds.other);
    fprintf(o, "  size==0   : %" PRIu64 "  (레벨 제거)\n", ds.deletes);
    fprintf(o, "  갭 직후   : %" PRIu64 "\n", ds.after_gap);

    fprintf(o, "\n== 디코드 ==\n");
    fprintf(o, "decoded     : %" PRIu64 "\n", g_dec.decoded);
    fprintf(o, "심볼 테이블 초과: %" PRIu64 "\n", g_dec.sym_dropped);
    fprintf(o, "distinct symbol : %u\n", g_symtab.count);

    fprintf(o, "\n== 오더북 ==\n");
    fprintf(o, "applied     : %" PRIu64 "\n", g_books.st.applied);
    fprintf(o, "  insert    : %" PRIu64 "\n", g_books.st.inserted);
    fprintf(o, "  update    : %" PRIu64 "\n", g_books.st.updated);
    fprintf(o, "  delete    : %" PRIu64 "\n", g_books.st.deleted);
    fprintf(o, "  레벨 초과 : %" PRIu64 "  (BOOK_MAX_LEVELS=%d)\n", g_books.st.overflow,
            BOOK_MAX_LEVELS);
    fprintf(o, "관측 최대 깊이: %u\n", g_books.st.max_depth);

    fprintf(o, "\n== BBO ==\n");
    fprintf(o, "EPC 발행 시점 : %" PRIu64 "\n", ds.epc);
    fprintf(o, "출력 라인     : %" PRIu64 "\n", ds.bbo_lines);

    print_lat(&ds);

    fprintf(o, "\n%s\n", (ds.popped == rs.pushed && ds.other == 0)
                             ? "=> push==pop, 큐에는 PLU만. 스테이지 연결 확인."
                             : "=> 불일치! 큐 핸드오프 또는 필터 점검 필요.");
    free(ds.lat);
    return 0;
}
