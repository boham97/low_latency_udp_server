#ifndef LL_SPSC_QUEUE_H
#define LL_SPSC_QUEUE_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>

/*
 * Lock-free SPSC 링버퍼. 단일 producer, 단일 consumer 전용.
 * head는 consumer만, tail은 producer만 갱신하므로 CAS 불필요 —
 * acquire/release 순서의 단순 atomic load/store만으로 안전하게 publish 가능.
 * CAPACITY는 2의 거듭제곱이어야 함 (modulo 대신 마스킹 사용).
 */
#define LL_SPSC_QUEUE_DEFINE(NAME, TYPE, CAPACITY)                            \
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

#endif /* LL_SPSC_QUEUE_H */
