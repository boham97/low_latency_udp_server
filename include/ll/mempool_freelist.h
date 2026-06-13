#ifndef LL_MEMPOOL_FREELIST_H
#define LL_MEMPOOL_FREELIST_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Lock-free fixed-size block pool backed by an intrusive free list.
 * Free slots are linked via index (not pointer) so the free-list head
 * fits in a single atomic word, allowing alloc/free from multiple
 * threads via CAS. ABA is possible if a slot is freed, reallocated,
 * and freed again between one thread's load and CAS of free_head;
 * acceptable for produce-once/consume-once message buffers where a
 * slot is never double-freed in flight.
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
    /* Returns NULL if the pool is exhausted. */                             \
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
