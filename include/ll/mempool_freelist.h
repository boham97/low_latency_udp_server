#ifndef LL_MEMPOOL_FREELIST_H
#define LL_MEMPOOL_FREELIST_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * intrusive free list 기반의 lock-free 고정 크기 블록 풀.
 * 빈 슬롯을 포인터가 아닌 인덱스로 연결하므로 free-list head가
 * 하나의 atomic 워드에 들어가며, 여러 스레드에서 CAS로 alloc/free
 * 가능. 한 스레드가 free_head를 load하고 CAS하는 사이에 슬롯이
 * free → 재할당 → 다시 free되면 ABA 문제가 발생할 수 있음. 슬롯이
 * 진행 중에 이중 free되지 않는 produce-once/consume-once 메시지
 * 버퍼에서는 허용 가능.
 */
#define LL_MEMPOOL_FREELIST_DEFINE(NAME, TYPE, CAPACITY)                      \
    typedef struct {                                                         \
        TYPE data;                                                           \
        _Atomic uint32_t next;                                               \
    } NAME##_slot_t;                                                         \
                                                                               \
    typedef struct {                                                         \
        alignas(64) _Atomic uint32_t free_head;                              \
        NAME##_slot_t slots[CAPACITY];                                       \
    } NAME##_t;                                                              \
                                                                               \
    enum { NAME##_NIL = UINT32_MAX };                                        \
                                                                               \
    static inline void NAME##_init(NAME##_t *p) {                            \
        for (uint32_t i = 0; i < (CAPACITY); i++) {                           \
            uint32_t next = (i + 1 < (CAPACITY)) ? i + 1 : NAME##_NIL;        \
            atomic_store_explicit(&p->slots[i].next, next,                   \
                                   memory_order_relaxed);                     \
        }                                                                     \
        atomic_store_explicit(&p->free_head, 0, memory_order_relaxed);       \
    }                                                                         \
                                                                               \
    /* 풀이 고갈되면 NULL 반환. */                                            \
    static inline TYPE *NAME##_alloc(NAME##_t *p) {                          \
        uint32_t head =                                                      \
            atomic_load_explicit(&p->free_head, memory_order_acquire);       \
        while (head != NAME##_NIL) {                                         \
            uint32_t next = atomic_load_explicit(&p->slots[head].next,       \
                                                   memory_order_relaxed);     \
            if (atomic_compare_exchange_weak_explicit(                       \
                    &p->free_head, &head, next, memory_order_acquire,        \
                    memory_order_acquire)) {                                 \
                return &p->slots[head].data;                                 \
            }                                                                 \
        }                                                                     \
        return NULL;                                                         \
    }                                                                         \
                                                                               \
    static inline void NAME##_free(NAME##_t *p, TYPE *item) {                \
        NAME##_slot_t *slot = (NAME##_slot_t *)item;                         \
        uint32_t idx = (uint32_t)(slot - p->slots);                          \
        uint32_t head =                                                      \
            atomic_load_explicit(&p->free_head, memory_order_relaxed);       \
        do {                                                                  \
            atomic_store_explicit(&slot->next, head, memory_order_relaxed);  \
        } while (!atomic_compare_exchange_weak_explicit(                     \
            &p->free_head, &head, idx, memory_order_release,                 \
            memory_order_relaxed));                                          \
    }                                                                         \
                                                                               \
    static inline size_t NAME##_capacity(void) { return (CAPACITY); }

#endif /* LL_MEMPOOL_FREELIST_H */
