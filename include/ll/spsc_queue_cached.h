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
    }                                                                         \
                                                                               \
    /* ── 슬롯 대여 API ────────────────────────────────────────────────────  \
     * push/pop이 강제하는 `buf[i] = *item` 복사를 없앤다. 큐가 슬롯 주소를    \
     * 빌려주고 호출자가 거기에 직접 쓴다. commit 전까지 tail이 안 올라가므로  \
     * 중간에 포기하면 커밋을 안 하는 것만으로 롤백이 끝난다.                  \
     *                                                                        \
     * SPSC 전제가 함수 밖으로 새는 대가가 있다 — 호출 규칙:                   \
     *   1) push_commit 전에 push_begin을 다시 부르지 않는다 (같은 슬롯 반환)  \
     *   2) push_commit 이후 그 포인터를 만지지 않는다 (소비자 소유)           \
     *   3) pop_begin 포인터는 pop_end 전까지만 유효                           \
     */                                                                       \
    /* producer 측: 빈 슬롯 주소, 가득 차면 NULL. tail은 아직 안 올린다 */    \
    static inline TYPE *NAME##_push_begin(NAME##_t *q) {                     \
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);  \
        if (tail - q->cached_head == (CAPACITY)) {                           \
            q->cached_head =                                                 \
                atomic_load_explicit(&q->head, memory_order_acquire);        \
            if (tail - q->cached_head == (CAPACITY)) {                       \
                return NULL;                                                 \
            }                                                                 \
        }                                                                     \
        return &q->buf[tail & ((CAPACITY) - 1)];                             \
    }                                                                         \
                                                                               \
    /* 이 release-store 시점부터 슬롯은 소비자 소유 — 이전 쓰기가 모두 보인다 */ \
    static inline void NAME##_push_commit(NAME##_t *q) {                     \
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);  \
        atomic_store_explicit(&q->tail, tail + 1, memory_order_release);     \
    }                                                                         \
                                                                               \
    /* consumer 측: 다음 슬롯 주소, 비면 NULL. head는 아직 안 올린다 */       \
    static inline TYPE *NAME##_pop_begin(NAME##_t *q) {                      \
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);  \
        if (head == q->cached_tail) {                                        \
            q->cached_tail =                                                 \
                atomic_load_explicit(&q->tail, memory_order_acquire);        \
            if (head == q->cached_tail) {                                    \
                return NULL;                                                 \
            }                                                                 \
        }                                                                     \
        return &q->buf[head & ((CAPACITY) - 1)];                             \
    }                                                                         \
                                                                               \
    /* release여야 슬롯 읽기가 이 store를 넘어가지 않는다 — 넘어가면 생산자가  \
       재사용 승인을 먼저 보고 아직 안 읽은 슬롯을 덮어쓴다 */                 \
    static inline void NAME##_pop_end(NAME##_t *q) {                         \
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);  \
        atomic_store_explicit(&q->head, head + 1, memory_order_release);     \
    }

#endif /* LL_SPSC_QUEUE_CACHED_H */
