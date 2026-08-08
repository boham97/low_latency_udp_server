/*
 * slot_vs_index.c — 큐에 "데이터를 직접" vs "인덱스를" 싣는 비용 비교
 *
 * CLAUDE.md가 "큐에 포인터나 인덱스를 싣지 않는다"로 확정한 근거를 실측한다.
 * 네 변형 모두 같은 SPSC 큐(cached), 같은 용량, 같은 페이로드, 같은 측정 코드.
 *
 *   copy     : 큐가 데이터를 담고, 값 복사 push/pop
 *              producer가 스택에 임시 객체를 만들고 push가 buf[tail] = *item
 *   borrow   : 큐가 데이터를 담고, 슬롯 대여 (push_begin/commit)
 *              producer가 링 슬롯에 직접 쓴다 — 임시 객체도 복사도 없음
 *   index    : 큐는 uint32_t 인덱스만, 데이터는 별도 풀 배열
 *              풀 크기 = 큐 용량, 순차 접근 → "간접 참조 비용"만 분리
 *   scatter  : index와 같되 풀이 8배 크고 stride로 흩어 쓴다
 *              반환이 FIFO가 아닌 실제 mempool의 접근 패턴
 *
 * 페이로드 16 / 64 / 2048 바이트로 "언제부터 복사가 아픈가"를 본다.
 *
 * 공정성을 위해 네 변형 모두 **같은 양의 유효 데이터만 쓴다** (ts, seq, pad의
 * 첫/끝 바이트). 그래서 차이로 남는 것은 큐 자체가 강제하는 복사와 메모리
 * 접근 패턴뿐이다 — copy는 유효 데이터가 16B여도 sizeof(TYPE) 전체를 옮긴다.
 *
 * 한 번에 조합 하나만 실행한다. 한 프로세스에서 연달아 돌리면 앞 실행이
 * 캐시/TLB/페이지를 예열해 뒤 실행이 부당하게 유리해진다 (bench/results.md와
 * 같은 이유). 3회 실행 중앙값으로 읽을 것.
 *
 * Build: gcc -O2 -Wall -Wextra -I../../include -pthread -o slot_vs_index slot_vs_index.c
 * Run:   ./slot_vs_index <copy|borrow|index|scatter> <16|64|2048>
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

#define CAP 1024
#define POOL (CAP * 8) /* scatter용 — 반환이 FIFO가 아니면 풀은 큐보다 커야 한다 */
#define NUM_ITEMS 2000000
#define PRODUCER_CPU 0
#define CONSUMER_CPU 1

/* stride 5로 풀을 돌면 POOL이 2의 거듭제곱이고 gcd(5,POOL)=1이라 POOL번 만에
   한 바퀴 — in-flight는 최대 CAP개(<POOL)이므로 아직 안 읽은 슬롯을 덮지 않는다 */
#define SCATTER_STRIDE 5

typedef enum { V_COPY, V_BORROW, V_INDEX, V_SCATTER } variant_t;

static uint64_t *g_lat;
static uint64_t g_sink; /* 소비자 읽기가 최적화로 사라지지 않게 */

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

/* 16B 변형은 pad가 0바이트다 — 길이 0이면 아무것도 안 만진다 */
static inline void touch_pad(uint8_t *p, size_t n, uint8_t v) {
    if (n) {
        p[0] = v;
        p[n - 1] = v;
    }
}

/*
 * 페이로드 크기마다 타입이 달라지므로 큐 인스턴스와 스레드 함수를 매크로로 생성.
 * 알고리즘은 셋 다 동일하고 크기만 다르다.
 */
#define GEN_SIZE(NAME, BYTES)                                                  \
    typedef struct {                                                           \
        uint64_t ts;                                                           \
        uint64_t seq;                                                          \
        uint8_t pad[(BYTES) - 16]; /* BYTES=16이면 길이 0 (GCC 확장) */        \
    } NAME##_msg_t;                                                            \
    _Static_assert(sizeof(NAME##_msg_t) == (BYTES), #NAME " payload size");    \
                                                                               \
    LL_SPSC_CACHED_DEFINE(NAME##_dq, NAME##_msg_t, CAP)  /* 데이터 큐 */       \
    LL_SPSC_CACHED_DEFINE(NAME##_iq, uint32_t, CAP)      /* 인덱스 큐 */       \
                                                                               \
    static NAME##_dq_t NAME##_dqi;                                             \
    static NAME##_iq_t NAME##_iqi;                                             \
    static NAME##_msg_t NAME##_pool[POOL];                                     \
                                                                               \
    static void *NAME##_producer(void *arg) {                                  \
        variant_t v = *(const variant_t *)arg;                                 \
        pin_to_cpu(PRODUCER_CPU);                                              \
        for (uint64_t i = 0; i < NUM_ITEMS; i++) {                             \
            if (v == V_COPY) {                                                 \
                /* 임시 객체 → push가 sizeof(TYPE) 전체를 링으로 복사 */       \
                NAME##_msg_t m;                                                \
                m.ts = ll_rdtsc();                                             \
                m.seq = i;                                                     \
                touch_pad(m.pad, sizeof m.pad, (uint8_t)i);                    \
                while (!NAME##_dq_push(&NAME##_dqi, &m)) {                     \
                }                                                              \
            } else if (v == V_BORROW) {                                        \
                /* 링 슬롯에 직접 — 임시 객체 없음, 복사 없음 */               \
                NAME##_msg_t *s;                                               \
                while (!(s = NAME##_dq_push_begin(&NAME##_dqi))) {             \
                }                                                              \
                s->ts = ll_rdtsc();                                            \
                s->seq = i;                                                    \
                touch_pad(s->pad, sizeof s->pad, (uint8_t)i);                  \
                NAME##_dq_push_commit(&NAME##_dqi);                            \
            } else {                                                           \
                uint32_t idx = (v == V_INDEX)                                  \
                                   ? (uint32_t)(i & (CAP - 1))                 \
                                   : (uint32_t)((i * SCATTER_STRIDE) &         \
                                                (POOL - 1));                   \
                uint32_t *s;                                                   \
                while (!(s = NAME##_iq_push_begin(&NAME##_iqi))) {             \
                }                                                              \
                /* 데이터는 풀에, 큐에는 인덱스만. commit의 release-store가     \
                   풀 쓰기까지 함께 공개한다 */                                \
                NAME##_msg_t *m = &NAME##_pool[idx];                           \
                m->ts = ll_rdtsc();                                            \
                m->seq = i;                                                    \
                touch_pad(m->pad, sizeof m->pad, (uint8_t)i);                  \
                *s = idx;                                                      \
                NAME##_iq_push_commit(&NAME##_iqi);                            \
            }                                                                  \
        }                                                                      \
        return NULL;                                                           \
    }                                                                          \
                                                                               \
    static void *NAME##_consumer(void *arg) {                                  \
        variant_t v = *(const variant_t *)arg;                                 \
        pin_to_cpu(CONSUMER_CPU);                                              \
        uint64_t sink = 0;                                                     \
        for (uint64_t i = 0; i < NUM_ITEMS; i++) {                             \
            const NAME##_msg_t *m;                                             \
            NAME##_msg_t tmp;                                                  \
            if (v == V_COPY) {                                                 \
                while (!NAME##_dq_pop(&NAME##_dqi, &tmp)) {                    \
                }                                                              \
                m = &tmp;                                                      \
            } else if (v == V_BORROW) {                                        \
                NAME##_msg_t *s;                                               \
                while (!(s = NAME##_dq_pop_begin(&NAME##_dqi))) {              \
                }                                                              \
                m = s;                                                         \
            } else {                                                           \
                uint32_t *s;                                                   \
                while (!(s = NAME##_iq_pop_begin(&NAME##_iqi))) {              \
                }                                                              \
                /* 인덱스 큐 라인 하나 + 풀 라인 하나 = 스트림 2개 */          \
                m = &NAME##_pool[*s];                                          \
            }                                                                  \
            g_lat[i] = ll_rdtsc() - m->ts;                                     \
            sink += m->seq + (sizeof m->pad ? m->pad[0] : 0);                  \
            if (v == V_BORROW) {                                               \
                NAME##_dq_pop_end(&NAME##_dqi);                                \
            } else if (v == V_INDEX || v == V_SCATTER) {                       \
                NAME##_iq_pop_end(&NAME##_iqi);                                \
            }                                                                  \
        }                                                                      \
        g_sink = sink;                                                         \
        return NULL;                                                           \
    }                                                                          \
                                                                               \
    static uint64_t NAME##_run(variant_t v) {                                  \
        NAME##_dq_init(&NAME##_dqi);                                           \
        NAME##_iq_init(&NAME##_iqi);                                           \
        pthread_t prod, cons;                                                  \
        uint64_t t0 = now_ns();                                                \
        pthread_create(&prod, NULL, NAME##_producer, &v);                      \
        pthread_create(&cons, NULL, NAME##_consumer, &v);                      \
        pthread_join(prod, NULL);                                              \
        pthread_join(cons, NULL);                                              \
        return now_ns() - t0;                                                  \
    }

GEN_SIZE(p16, 16)
GEN_SIZE(p64, 64)
GEN_SIZE(p2k, 2048)

static void report(const char *label, uint64_t elapsed_ns) {
    qsort(g_lat, NUM_ITEMS, sizeof(uint64_t), cmp_u64);
    printf("%s\n", label);
    printf("  throughput : %.2f M msg/s\n",
           (double)NUM_ITEMS / ((double)elapsed_ns / 1e9) / 1e6);
    printf("  push->pop  : p50 %" PRIu64 "  p99 %" PRIu64 "  p99.9 %" PRIu64
           " cycles\n",
           g_lat[(size_t)(0.50 * (NUM_ITEMS - 1))],
           g_lat[(size_t)(0.99 * (NUM_ITEMS - 1))],
           g_lat[(size_t)(0.999 * (NUM_ITEMS - 1))]);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <copy|borrow|index|scatter> <16|64|2048>\n",
                argv[0]);
        return 2;
    }
    variant_t v;
    if (strcmp(argv[1], "copy") == 0) v = V_COPY;
    else if (strcmp(argv[1], "borrow") == 0) v = V_BORROW;
    else if (strcmp(argv[1], "index") == 0) v = V_INDEX;
    else if (strcmp(argv[1], "scatter") == 0) v = V_SCATTER;
    else { fprintf(stderr, "unknown variant: %s\n", argv[1]); return 2; }

    int bytes = atoi(argv[2]);
    if (bytes != 16 && bytes != 64 && bytes != 2048) {
        fprintf(stderr, "payload must be 16, 64 or 2048 (got %d)\n", bytes);
        return 2;
    }

    g_lat = malloc(sizeof(uint64_t) * NUM_ITEMS);
    if (!g_lat) {
        return 1;
    }

    uint64_t elapsed = bytes == 16    ? p16_run(v)
                       : bytes == 64  ? p64_run(v)
                                      : p2k_run(v);

    char label[96];
    snprintf(label, sizeof label, "%s, payload=%dB, cap=%d, items=%d", argv[1],
             bytes, CAP, NUM_ITEMS);
    report(label, elapsed);
    free(g_lat);
    return 0;
}
