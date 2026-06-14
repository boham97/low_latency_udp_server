/*
 * SPSC 큐의 tail/buf 캐시라인 분리(alignas(64) on buf) 효과 비교 벤치마크.
 *
 * OLD: buf가 tail과 같은 캐시라인을 공유하는 (수정 전) 레이아웃을 재현
 * NEW: include/ll/spsc_queue.h 의 현재 레이아웃 (buf에 alignas(64) 적용)
 *
 * producer/consumer를 별도 스레드로 실행, push-to-pop 지연시간을
 * clock_gettime(CLOCK_MONOTONIC)으로 측정해 p50/p99/p99.9 비교.
 *
 * 주의: WSL2/하이퍼바이저 환경 — 절대 수치 무의미, OLD/NEW 상대 비교만 참고.
 */
#define _GNU_SOURCE
#include <ll/spsc_queue.h>

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUEUE_CAP 8
#define NUM_ITEMS 2000000

typedef struct {
    uint64_t ts_ns;
    uint64_t seq;
} msg_t;

/* 수정 전 레이아웃 재현: buf에 alignas(64) 없음 -> tail과 캐시라인 공유 */
#define OLD_SPSC_QUEUE_DEFINE(NAME, TYPE, CAPACITY)                           \
    typedef struct {                                                          \
        alignas(64) _Atomic size_t head;                                      \
        alignas(64) _Atomic size_t tail;                                      \
        TYPE buf[CAPACITY];                                                   \
    } NAME##_t;                                                               \
                                                                               \
    static inline void NAME##_init(NAME##_t *q) {                            \
        atomic_store_explicit(&q->head, 0, memory_order_relaxed);            \
        atomic_store_explicit(&q->tail, 0, memory_order_relaxed);            \
    }                                                                         \
                                                                               \
    static inline int NAME##_push(NAME##_t *q, const TYPE *item) {           \
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);  \
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);  \
        if (tail - head == (CAPACITY)) {                                     \
            return 0;                                                        \
        }                                                                     \
        q->buf[tail & ((CAPACITY) - 1)] = *item;                             \
        atomic_store_explicit(&q->tail, tail + 1, memory_order_release);     \
        return 1;                                                             \
    }                                                                         \
                                                                               \
    static inline int NAME##_pop(NAME##_t *q, TYPE *item) {                  \
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);  \
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);  \
        if (head == tail) {                                                  \
            return 0;                                                        \
        }                                                                     \
        *item = q->buf[head & ((CAPACITY) - 1)];                             \
        atomic_store_explicit(&q->head, head + 1, memory_order_release);     \
        return 1;                                                             \
    }

OLD_SPSC_QUEUE_DEFINE(old_q, msg_t, QUEUE_CAP)
LL_SPSC_QUEUE_DEFINE(new_q, msg_t, QUEUE_CAP)

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

static uint64_t percentile(uint64_t *sorted, size_t n, double p) {
    size_t idx = (size_t)(p * (double)(n - 1));
    return sorted[idx];
}

/* ---- OLD 큐 벤치마크 ---- */
typedef struct {
    old_q_t *q;
    uint64_t *latencies;
} old_consumer_arg_t;

static void *old_producer(void *arg) {
    old_q_t *q = (old_q_t *)arg;
    pin_to_cpu(0);
    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
        msg_t m = {.ts_ns = now_ns(), .seq = i};
        while (!old_q_push(q, &m)) {
            /* 큐가 가득 차면 consumer가 처리할 때까지 스핀 */
        }
    }
    return NULL;
}

static void *old_consumer(void *arg) {
    old_consumer_arg_t *a = (old_consumer_arg_t *)arg;
    pin_to_cpu(1);
    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
        msg_t m;
        while (!old_q_pop(a->q, &m)) {
            /* 큐가 비어있으면 producer가 채울 때까지 스핀 */
        }
        a->latencies[i] = now_ns() - m.ts_ns;
    }
    return NULL;
}

/* ---- NEW 큐 벤치마크 ---- */
typedef struct {
    new_q_t *q;
    uint64_t *latencies;
} new_consumer_arg_t;

static void *new_producer(void *arg) {
    new_q_t *q = (new_q_t *)arg;
    pin_to_cpu(0);
    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
        msg_t m = {.ts_ns = now_ns(), .seq = i};
        while (!new_q_push(q, &m)) {
        }
    }
    return NULL;
}

static void *new_consumer(void *arg) {
    new_consumer_arg_t *a = (new_consumer_arg_t *)arg;
    pin_to_cpu(1);
    for (uint64_t i = 0; i < NUM_ITEMS; i++) {
        msg_t m;
        while (!new_q_pop(a->q, &m)) {
        }
        a->latencies[i] = now_ns() - m.ts_ns;
    }
    return NULL;
}

static void print_result(const char *label, uint64_t *latencies, uint64_t elapsed_ns) {
    qsort(latencies, NUM_ITEMS, sizeof(uint64_t), cmp_u64);
    uint64_t sum = 0;
    for (size_t i = 0; i < NUM_ITEMS; i++) {
        sum += latencies[i];
    }
    double avg = (double)sum / NUM_ITEMS;
    double throughput = (double)NUM_ITEMS / ((double)elapsed_ns / 1e9);

    printf("[%s]\n", label);
    printf("  p50  = %6lu ns\n", percentile(latencies, NUM_ITEMS, 0.50));
    printf("  p90  = %6lu ns\n", percentile(latencies, NUM_ITEMS, 0.90));
    printf("  p99  = %6lu ns\n", percentile(latencies, NUM_ITEMS, 0.99));
    printf("  p99.9= %6lu ns\n", percentile(latencies, NUM_ITEMS, 0.999));
    printf("  avg  = %8.1f ns\n", avg);
    printf("  throughput = %.2f M msg/s\n\n", throughput / 1e6);
}

int main(void) {
    printf("queue capacity = %d, items = %d\n\n", QUEUE_CAP, NUM_ITEMS);

    uint64_t *latencies = malloc(sizeof(uint64_t) * NUM_ITEMS);
    if (!latencies) {
        return 1;
    }

    /* OLD */
    {
        old_q_t *q = aligned_alloc(64, sizeof(old_q_t));
        old_q_init(q);
        old_consumer_arg_t carg = {.q = q, .latencies = latencies};

        pthread_t prod, cons;
        uint64_t t0 = now_ns();
        pthread_create(&prod, NULL, old_producer, q);
        pthread_create(&cons, NULL, old_consumer, &carg);
        pthread_join(prod, NULL);
        pthread_join(cons, NULL);
        uint64_t elapsed = now_ns() - t0;

        print_result("OLD: buf shares cacheline with tail", latencies, elapsed);
        free(q);
    }

    /* NEW */
    {
        new_q_t *q = aligned_alloc(64, sizeof(new_q_t));
        new_q_init(q);
        new_consumer_arg_t carg = {.q = q, .latencies = latencies};

        pthread_t prod, cons;
        uint64_t t0 = now_ns();
        pthread_create(&prod, NULL, new_producer, q);
        pthread_create(&cons, NULL, new_consumer, &carg);
        pthread_join(prod, NULL);
        pthread_join(cons, NULL);
        uint64_t elapsed = now_ns() - t0;

        print_result("NEW: buf separated from tail (alignas(64))", latencies, elapsed);
        free(q);
    }

    free(latencies);
    return 0;
}
