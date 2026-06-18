#ifndef LL_SPSC_QUEUE_PADDED_H
#define LL_SPSC_QUEUE_PADDED_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>

/*
 * SPSC 링버퍼 변형 2/3: 캐시라인 분리만 적용 (캐싱 없음).
 * head / tail / buf를 각각 alignas(64)로 다른 캐시라인에 배치해
 * producer와 consumer가 서로의 라인을 invalidate 하는 false sharing을 제거.
 * 다만 push는 매번 consumer 소유 head를, pop은 매번 producer 소유 tail을
 * atomic load 하므로, 그 라인이 상대 코어에서 수정될 때마다 코히런스
 * 트래픽은 여전히 남는다 — 변형 3(cached)과 비교하기 위한 중간 단계.
 * CAPACITY는 2의 거듭제곱이어야 함 (modulo 대신 마스킹 사용).
 */
#define LL_SPSC_PADDED_DEFINE(NAME, TYPE, CAPACITY)                           \
    _Static_assert(((CAPACITY) & ((CAPACITY) - 1)) == 0,                      \
                   #NAME " capacity must be a power of two");                \
                                                                               \
    typedef struct {                                                          \
        alignas(64) _Atomic size_t head; /* consumer 소유 read index */      \
        alignas(64) _Atomic size_t tail; /* producer 소유 write index */     \
        alignas(64) TYPE buf[CAPACITY];                                       \
    } NAME##_t;                                                               \
                                                                               \
    static inline void NAME##_init(NAME##_t *q) {                            \
        atomic_store_explicit(&q->head, 0, memory_order_relaxed);            \
        atomic_store_explicit(&q->tail, 0, memory_order_relaxed);            \
    }                                                                         \
                                                                               \
    /* producer 측: 큐가 가득 차면 0 반환 */                                  \
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
    /* consumer 측: 큐가 비어있으면 0 반환 */                                 \
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

#endif /* LL_SPSC_QUEUE_PADDED_H */
