/*
 * slot_vs_index.c — 큐에 "데이터를 직접" vs "인덱스를" 싣는 비용 비교
 *
 * CLAUDE.md가 "큐에 포인터나 인덱스를 싣지 않는다"로 확정한 근거를 실측한다.
 * 네 변형 모두 같은 SPSC 큐(cached), 같은 용량, 같은 페이로드, 같은 측정 코드.
 *
 *   copy     : 큐가 데이터를 담고, 값 복사 push/pop
 *              생산자가 스택에 메시지를 만들고 push가 buf[tail] = *item 으로 복사
 *   borrow   : 큐가 데이터를 담고, 슬롯 대여 (push_begin/commit)
 *              생산자가 링 슬롯에 직접 채운다 — 스택 임시 객체도 복사도 없음
 *   index    : 큐는 uint32_t 인덱스만, 데이터는 별도 풀 배열
 *              풀 크기 = 큐 용량, 순차 접근 → "간접 참조 비용"만 분리
 *   scatter  : index와 같되 풀이 8배 크고 stride로 흩어 쓴다
 *              반환이 FIFO가 아닌 실제 mempool의 접근 패턴
 *
 * 페이로드 크기는 컴파일 타임 상수다 (-DPAYLOAD_BYTES). 크기마다 별도 바이너리를
 * 빌드한다 — 한 바이너리에 여러 크기를 넣으면 타입이 여러 벌이라 매크로로 코드를
 * 찍어내야 하고, 어차피 조합마다 별도 프로세스로 돌려야 한다(아래).
 *
 * **생산자는 페이로드 전체를 실제로 채운다.** 크기만 키우고 내용을 안 쓰면
 * 페이로드가 "슬롯 간격"으로만 작용해 복사 비용 실험이 성립하지 않는다.
 * 실제 Rx도 커널이 데이터그램 전체를 써준다. 소비자도 캐시라인마다 한 바이트씩
 * 읽어 페이로드 전체를 훑는다.
 *
 * 한 번에 변형 하나만 실행한다. 한 프로세스에서 연달아 돌리면 앞 실행이
 * 캐시/TLB/페이지를 예열해 뒤 실행이 부당하게 유리해진다 (bench/results.md와
 * 같은 이유).
 *
 * Build: gcc -O2 -Wall -Wextra -I../../include -pthread \
 *            -DPAYLOAD_BYTES=2048 -o slot_vs_index_2048 slot_vs_index.c
 * Run:   ./slot_vs_index_2048 <copy|borrow|index|scatter>
 */
#define _GNU_SOURCE
#include <ll/spsc_queue_cached.h>
#include <ll/tsc.h>

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PAYLOAD_BYTES
#define PAYLOAD_BYTES 64
#endif

#ifndef CAP
#define CAP 1024
#endif
#define POOL (CAP * 8) /* scatter용 — 반환이 FIFO가 아니면 풀은 큐보다 커야 한다 */
#define NUM_ITEMS 2000000
#define PRODUCER_CPU 0
#define CONSUMER_CPU 1

/* stride 5로 풀을 돌면 POOL이 2의 거듭제곱이고 gcd(5,POOL)=1이라 POOL번 만에
   한 바퀴 — in-flight는 최대 CAP개(<POOL)이므로 아직 안 읽은 슬롯을 덮지 않는다 */
#define SCATTER_STRIDE 5

typedef enum { V_COPY, V_BORROW, V_INDEX, V_SCATTER } variant_t;

typedef struct {
    uint64_t ts;  /* 생산 시점 TSC — push→pop 구간 측정용 */
    uint64_t seq;
    uint8_t data[PAYLOAD_BYTES - 16]; /* 실어 나르는 내용 */
} msg_t;

_Static_assert(sizeof(msg_t) == PAYLOAD_BYTES, "msg_t must be PAYLOAD_BYTES");

LL_SPSC_CACHED_DEFINE(dataq, msg_t, CAP)    /* copy / borrow 용 — 원소가 메시지 */
LL_SPSC_CACHED_DEFINE(idxq, uint32_t, CAP)  /* index / scatter 용 — 원소가 4B 번호 */

static dataq_t g_dataq;
static idxq_t g_idxq;
static msg_t g_pool[POOL]; /* index / scatter가 데이터를 두는 곳 */

/* -DPERF_MODE: 지연 기록과 qsort를 뺀다. perf stat으로 파이프라인만 재려면
   200만 개 정렬(그 자체로 미스 폭탄)이 카운터에 섞이면 안 된다 */
#ifdef PERF_MODE
#define RECORD_LAT 0
#else
#define RECORD_LAT 1
#endif

static uint64_t *g_lat;
/* 소비자가 읽은 값의 합. main에서 반드시 출력한다 — 출력하지 않으면 아무도 읽지
   않는 static 변수가 되어, GCC가 죽은 저장으로 보고 consume() 전체를 지운다
   (그러면 소비자가 페이로드를 안 읽는 벤치가 된다) */
static uint64_t g_sink;

/* 생산자가 메시지 하나를 채운다 — 페이로드 전체를 쓴다 */
static void fill(msg_t *m, uint64_t i) {
    m->ts = ll_rdtsc();
    m->seq = i;
    memset(m->data, (int)i, sizeof m->data);
}

/* 소비자가 메시지 하나를 읽는다 — 캐시라인마다 한 바이트씩, 페이로드 전체를 훑는다 */
static uint64_t consume(const msg_t *m) {
    uint64_t s = m->seq;
    size_t n = sizeof m->data; /* PAYLOAD_BYTES=16 빌드에선 0 */
    for (size_t k = 0; k < n; k += 64) {
        s += m->data[k];
    }
    return s;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

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

/* index는 큐 용량만큼만 순차로, scatter는 8배 풀을 stride로 흩어 쓴다 */
static uint32_t pool_index(variant_t v, uint64_t i) {
    return v == V_INDEX ? (uint32_t)(i & (CAP - 1))
                        : (uint32_t)((i * SCATTER_STRIDE) & (POOL - 1));
}

static void *producer(void *arg) {
    variant_t v = *(const variant_t *)arg;
    pin_to_cpu(PRODUCER_CPU);

    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
        if (v == V_COPY) {
            /* 스택에 메시지를 만들고 → push가 sizeof(msg_t) 전체를 링으로 복사 */
            msg_t m;
            fill(&m, i);
            while (!dataq_push(&g_dataq, &m)) {
            }
        } else if (v == V_BORROW) {
            /* 링 슬롯 주소를 받아 그 자리에 채운다 — 임시 객체 없음, 복사 없음 */
            msg_t *slot;
            while (!(slot = dataq_push_begin(&g_dataq))) {
            }
            fill(slot, i);
            dataq_push_commit(&g_dataq);
        } else {
            /* 데이터는 풀에 채우고, 큐에는 번호만 싣는다.
               commit의 release-store가 풀 쓰기까지 함께 공개한다 */
            uint32_t idx = pool_index(v, i);
            uint32_t *slot;
            while (!(slot = idxq_push_begin(&g_idxq))) {
            }
            fill(&g_pool[idx], i);
            *slot = idx;
            idxq_push_commit(&g_idxq);
        }
    }
    return NULL;
}

static void *consumer(void *arg) {
    variant_t v = *(const variant_t *)arg;
    pin_to_cpu(CONSUMER_CPU);

    uint64_t sink = 0;
    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
        if (v == V_COPY) {
            /* pop이 sizeof(msg_t) 전체를 스택으로 복사 */
            msg_t m;
            while (!dataq_pop(&g_dataq, &m)) {
            }
            if (RECORD_LAT) g_lat[i] = ll_rdtsc() - m.ts;
            sink += consume(&m);
        } else if (v == V_BORROW) {
            msg_t *slot;
            while (!(slot = dataq_pop_begin(&g_dataq))) {
            }
            if (RECORD_LAT) g_lat[i] = ll_rdtsc() - slot->ts;
            sink += consume(slot);
            dataq_pop_end(&g_dataq);
        } else {
            /* 인덱스 큐 라인 하나 + 풀 라인 하나 = 캐시라인 스트림 2개.
               게다가 *slot을 읽어야 풀 주소를 알 수 있다 (로드 의존성) */
            uint32_t *slot;
            while (!(slot = idxq_pop_begin(&g_idxq))) {
            }
            const msg_t *m = &g_pool[*slot];
            if (RECORD_LAT) g_lat[i] = ll_rdtsc() - m->ts;
            sink += consume(m);
            idxq_pop_end(&g_idxq);
        }
    }
    g_sink = sink;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <copy|borrow|index|scatter>   (payload=%dB)\n",
                argv[0], PAYLOAD_BYTES);
        return 2;
    }
    variant_t v;
    if (strcmp(argv[1], "copy") == 0) v = V_COPY;
    else if (strcmp(argv[1], "borrow") == 0) v = V_BORROW;
    else if (strcmp(argv[1], "index") == 0) v = V_INDEX;
    else if (strcmp(argv[1], "scatter") == 0) v = V_SCATTER;
    else { fprintf(stderr, "unknown variant: %s\n", argv[1]); return 2; }

    g_lat = malloc(sizeof(uint64_t) * NUM_ITEMS);
    if (!g_lat) {
        return 1;
    }

    dataq_init(&g_dataq);
    idxq_init(&g_idxq);

    pthread_t prod, cons;
    uint64_t t0 = now_ns();
    pthread_create(&prod, NULL, producer, &v);
    pthread_create(&cons, NULL, consumer, &v);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    uint64_t elapsed_ns = now_ns() - t0;

    if (RECORD_LAT) qsort(g_lat, NUM_ITEMS, sizeof(uint64_t), cmp_u64);
    printf("%s, payload=%dB, cap=%d, items=%d\n", argv[1], PAYLOAD_BYTES, CAP,
           NUM_ITEMS);
    printf("  throughput : %.2f M msg/s\n",
           (double)NUM_ITEMS / ((double)elapsed_ns / 1e9) / 1e6);
    if (RECORD_LAT) printf("  push->pop  : p50 %" PRIu64 "  p99 %" PRIu64 "  p99.9 %" PRIu64
           " cycles\n",
           g_lat[(size_t)(0.50 * (NUM_ITEMS - 1))],
           g_lat[(size_t)(0.99 * (NUM_ITEMS - 1))],
           g_lat[(size_t)(0.999 * (NUM_ITEMS - 1))]);
    printf("  checksum   : %" PRIu64 "\n", g_sink);

    free(g_lat);
    return 0;
}
