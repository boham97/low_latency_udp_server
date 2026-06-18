/*
 * SPSC 큐 레이아웃 3종 비교 벤치마크.
 *
 *   nopad  : 캐시라인 정렬 없음 (false sharing)        - LL_SPSC_NOPAD_DEFINE
 *   padded : alignas(64) 분리만                         - LL_SPSC_PADDED_DEFINE
 *   cached : 분리 + cached_head/cached_tail             - LL_SPSC_CACHED_DEFINE
 *
 * producer(코어 0) / consumer(코어 1)를 별도 스레드로 고정 실행하고
 * push-to-pop 지연을 clock_gettime(CLOCK_MONOTONIC)으로 측정해 p50/p99 비교.
 *
 * 한 번에 변형 하나만 실행한다. 여러 변형을 한 프로세스에서 연달아 돌리면
 * 앞 실행이 캐시/TLB/페이지를 예열해 뒤 실행이 부당하게 유리해지므로,
 * 변형마다 별도 프로세스로 띄워 조건을 맞춘다.
 *
 *   사용법: spsc_variants_bench <nopad|padded|cached> [capacity(8|1024)]
 *
 * 주의: WSL2/하이퍼바이저 환경 — 절대 수치 무의미, 변형 간 상대 비교만 참고.
 */
#define _GNU_SOURCE
#include <ll/spsc_queue_cached.h>
#include <ll/spsc_queue_nopad.h>
#include <ll/spsc_queue_padded.h>

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_ITEMS 2000000
#define PRODUCER_CPU 0
#define CONSUMER_CPU 1

typedef struct {
    uint64_t ts_ns;
    uint64_t seq;
} msg_t;

/* (변형 매크로, capacity) 조합마다 타입 1개를 컴파일 타임에 인스턴스화 */
LL_SPSC_NOPAD_DEFINE(nopad8, msg_t, 8)
LL_SPSC_NOPAD_DEFINE(nopad1k, msg_t, 1024)
LL_SPSC_PADDED_DEFINE(padded8, msg_t, 8)
LL_SPSC_PADDED_DEFINE(padded1k, msg_t, 1024)
LL_SPSC_CACHED_DEFINE(cached8, msg_t, 8)
LL_SPSC_CACHED_DEFINE(cached1k, msg_t, 1024)

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
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t percentile(const uint64_t *sorted, size_t n, double p) {
    size_t idx = (size_t)(p * (double)(n - 1));
    return sorted[idx];
}

static void print_result(const char *label, uint64_t *lat, uint64_t elapsed_ns) {
    qsort(lat, NUM_ITEMS, sizeof(uint64_t), cmp_u64);
    uint64_t sum = 0;
    for (size_t i = 0; i < NUM_ITEMS; i++) {
        sum += lat[i];
    }
    double avg = (double)sum / NUM_ITEMS;
    double throughput = (double)NUM_ITEMS / ((double)elapsed_ns / 1e9);

    printf("[%s]\n", label);
    printf("  p50  = %6lu ns\n", percentile(lat, NUM_ITEMS, 0.50));
    printf("  p90  = %6lu ns\n", percentile(lat, NUM_ITEMS, 0.90));
    printf("  p99  = %6lu ns\n", percentile(lat, NUM_ITEMS, 0.99));
    printf("  p99.9= %6lu ns\n", percentile(lat, NUM_ITEMS, 0.999));
    printf("  avg  = %8.1f ns\n", avg);
    printf("  throughput = %.2f M msg/s\n", throughput / 1e6);
}

/*
 * 변형/용량별 타입이 다르므로 producer/consumer/run을 매크로로 생성.
 * 알고리즘은 동일하고 PREFIX 만 다르다.
 */
#define GEN_BENCH(PREFIX)                                                     \
    static void *PREFIX##_producer(void *arg) {                              \
        PREFIX##_t *q = (PREFIX##_t *)arg;                                   \
        pin_to_cpu(PRODUCER_CPU);                                            \
        for (uint64_t i = 0; i < NUM_ITEMS; i++) {                           \
            msg_t m = {.ts_ns = now_ns(), .seq = i};                         \
            while (!PREFIX##_push(q, &m)) {                                  \
            }                                                                 \
        }                                                                     \
        return NULL;                                                         \
    }                                                                         \
    static void *PREFIX##_consumer(void *arg) {                              \
        void **a = (void **)arg;                                             \
        PREFIX##_t *q = (PREFIX##_t *)a[0];                                  \
        uint64_t *lat = (uint64_t *)a[1];                                    \
        pin_to_cpu(CONSUMER_CPU);                                            \
        for (uint64_t i = 0; i < NUM_ITEMS; i++) {                           \
            msg_t m;                                                         \
            while (!PREFIX##_pop(q, &m)) {                                   \
            }                                                                 \
            lat[i] = now_ns() - m.ts_ns;                                     \
        }                                                                     \
        return NULL;                                                         \
    }                                                                         \
    static void PREFIX##_run(const char *label, uint64_t *lat) {             \
        PREFIX##_t *q = aligned_alloc(64, sizeof(PREFIX##_t));               \
        PREFIX##_init(q);                                                    \
        void *carg[2] = {q, lat};                                           \
        pthread_t prod, cons;                                                \
        uint64_t t0 = now_ns();                                             \
        pthread_create(&prod, NULL, PREFIX##_producer, q);                   \
        pthread_create(&cons, NULL, PREFIX##_consumer, carg);                \
        pthread_join(prod, NULL);                                            \
        pthread_join(cons, NULL);                                            \
        print_result(label, lat, now_ns() - t0);                            \
        free(q);                                                            \
    }

GEN_BENCH(nopad8)
GEN_BENCH(nopad1k)
GEN_BENCH(padded8)
GEN_BENCH(padded1k)
GEN_BENCH(cached8)
GEN_BENCH(cached1k)

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <nopad|padded|cached> [capacity(8|1024)]\n",
                argv[0]);
        return 2;
    }
    const char *variant = argv[1];
    int cap = (argc >= 3) ? atoi(argv[2]) : 1024;
    if (cap != 8 && cap != 1024) {
        fprintf(stderr, "capacity must be 8 or 1024 (got %d)\n", cap);
        return 2;
    }

    uint64_t *lat = malloc(sizeof(uint64_t) * NUM_ITEMS);
    if (!lat) {
        return 1;
    }

    char label[64];
    snprintf(label, sizeof(label), "%s, cap=%d, items=%d", variant, cap, NUM_ITEMS);
    printf("PRODUCER_CPU=%d CONSUMER_CPU=%d\n\n", PRODUCER_CPU, CONSUMER_CPU);

    int matched = 1;
    if (strcmp(variant, "nopad") == 0) {
        cap == 8 ? nopad8_run(label, lat) : nopad1k_run(label, lat);
    } else if (strcmp(variant, "padded") == 0) {
        cap == 8 ? padded8_run(label, lat) : padded1k_run(label, lat);
    } else if (strcmp(variant, "cached") == 0) {
        cap == 8 ? cached8_run(label, lat) : cached1k_run(label, lat);
    } else {
        fprintf(stderr, "unknown variant: %s\n", variant);
        matched = 0;
    }

    free(lat);
    return matched ? 0 : 2;
}
