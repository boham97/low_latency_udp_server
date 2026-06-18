#ifndef LL_SPSC_QUEUE_CACHED_H
#define LL_SPSC_QUEUE_CACHED_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>

/*
 * SPSC 링버퍼 변형 3/3: 캐시라인 분리 + cached_head / cached_tail.
 * 변형 2처럼 head/tail/buf를 라인 분리하되, 각 측이 상대 인덱스의 마지막
 * 관측값을 자기 라인에 캐싱한다. push는 cached_head로 먼저 판단하고 가득 차
 * 보일 때만 실제 head를 acquire load, pop은 cached_tail로 먼저 판단하고 비어
 * 보일 때만 실제 tail을 acquire load. 평상시엔 로컬 캐시만 보므로 상대 코어가
 * 소유한 라인을 건드리지 않아 코히런스 트래픽이 사라진다.
 * CAPACITY는 2의 거듭제곱이어야 함 (modulo 대신 마스킹 사용).
 */
#define LL_SPSC_CACHED_DEFINE(NAME, TYPE, CAPACITY)                           \
    _Static_assert(((CAPACITY) & ((CAPACITY) - 1)) == 0,                      \
                   #NAME " capacity must be a power of two");                \
                                                                               \
    typedef struct {                                                          \
        /* consumer 라인: head는 consumer가 쓰고, cached_tail도 consumer만 본다 */ \
        alignas(64) _Atomic size_t head; /* consumer 소유 read index */      \
        size_t cached_tail;              /* consumer 로컬: tail 마지막 관측값 */ \
        /* producer 라인: tail은 producer가 쓰고, cached_head도 producer만 본다 */ \
        alignas(64) _Atomic size_t tail; /* producer 소유 write index */     \
        size_t cached_head;              /* producer 로컬: head 마지막 관측값 */ \
        alignas(64) TYPE buf[CAPACITY];                                       \
    } NAME##_t;                                                               \
                                                                               \
    static inline void NAME##_init(NAME##_t *q) {                            \
        atomic_store_explicit(&q->head, 0, memory_order_relaxed);            \
        atomic_store_explicit(&q->tail, 0, memory_order_relaxed);            \
        q->cached_tail = 0;                                                  \
        q->cached_head = 0;                                                  \
    }                                                                         \
                                                                               \
    /* producer 측: 큐가 가득 차면 0 반환 */                                  \
    static inline int NAME##_push(NAME##_t *q, const TYPE *item) {           \
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);  \
        /* 캐시된 head로 먼저 판단, 가득 차 보일 때만 실제 head를 재확인 */    \
        if (tail - q->cached_head == (CAPACITY)) {                           \
            q->cached_head =                                                 \
                atomic_load_explicit(&q->head, memory_order_acquire);        \
            if (tail - q->cached_head == (CAPACITY)) {                       \
                return 0;                                                    \
            }                                                                 \
        }                                                                     \
        q->buf[tail & ((CAPACITY) - 1)] = *item;                             \
        atomic_store_explicit(&q->tail, tail + 1, memory_order_release);     \
        return 1;                                                             \
    }                                                                         \
                                                                               \
    /* consumer 측: 큐가 비어있으면 0 반환 */                                 \
    static inline int NAME##_pop(NAME##_t *q, TYPE *item) {                  \
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);  \
        /* 캐시된 tail로 먼저 판단, 비어 보일 때만 실제 tail을 재확인 */       \
        if (head == q->cached_tail) {                                        \
            q->cached_tail =                                                 \
                atomic_load_explicit(&q->tail, memory_order_acquire);        \
            if (head == q->cached_tail) {                                    \
                return 0;                                                    \
            }                                                                 \
        }                                                                     \
        *item = q->buf[head & ((CAPACITY) - 1)];                             \
        atomic_store_explicit(&q->head, head + 1, memory_order_release);     \
        return 1;                                                             \
    }

#endif /* LL_SPSC_QUEUE_CACHED_H */
