#ifndef LL_MEMPOOL_INDEXSTACK_H
#define LL_MEMPOOL_INDEXSTACK_H

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Single-threaded fixed-size block pool. Free slot indices live in a
 * plain array-based LIFO stack (no pointer chasing, no atomics), so
 * alloc/free are pure array accesses — use when the pool is only
 * touched from one core's hot path.
 */
#define LL_MEMPOOL_INDEXSTACK_DEFINE(NAME, TYPE, CAPACITY)                    \
    typedef struct {                                                         \
        alignas(64) uint32_t free_idx[CAPACITY];                             \
        uint32_t top;                                                        \
        TYPE slots[CAPACITY];                                                \
    } NAME##_t;                                                              \
                                                                               \
    static inline void NAME##_init(NAME##_t *p) {                            \
        for (uint32_t i = 0; i < (CAPACITY); i++) {                          \
            p->free_idx[i] = (CAPACITY) - 1 - i;                             \
        }                                                                     \
        p->top = (CAPACITY);                                                  \
    }                                                                         \
                                                                               \
    /* Returns NULL if the pool is exhausted. */                             \
    static inline TYPE *NAME##_alloc(NAME##_t *p) {                          \
        if (p->top == 0) {                                                    \
            return NULL;                                                      \
        }                                                                     \
        return &p->slots[p->free_idx[--p->top]];                             \
    }                                                                         \
                                                                               \
    static inline void NAME##_free(NAME##_t *p, TYPE *item) {                \
        p->free_idx[p->top++] = (uint32_t)(item - p->slots);                 \
    }                                                                         \
                                                                               \
    static inline size_t NAME##_capacity(void) { return (CAPACITY); }

#endif /* LL_MEMPOOL_INDEXSTACK_H */
