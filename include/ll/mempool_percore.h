#ifndef LL_MEMPOOL_PERCORE_H
#define LL_MEMPOOL_PERCORE_H

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Per-core fixed-size block pools: each core gets its own free-index
 * stack with no cross-core synchronization, so alloc/free never touch
 * an atomic. The tradeoff is that a block allocated from one core's
 * pool must be freed back to that same core's pool — no migration
 * support, caller's responsibility.
 */
#define LL_MEMPOOL_PERCORE_DEFINE(NAME, TYPE, CAPACITY_PER_CORE, NCORES)      \
    typedef struct {                                                         \
        alignas(64) uint32_t free_idx[CAPACITY_PER_CORE];                    \
        uint32_t top;                                                        \
        TYPE slots[CAPACITY_PER_CORE];                                       \
    } NAME##_core_pool_t;                                                    \
                                                                               \
    typedef struct {                                                         \
        NAME##_core_pool_t core[NCORES];                                     \
    } NAME##_t;                                                              \
                                                                               \
    static inline void NAME##_init(NAME##_t *p) {                            \
        for (uint32_t c = 0; c < (NCORES); c++) {                            \
            NAME##_core_pool_t *cp = &p->core[c];                            \
            for (uint32_t i = 0; i < (CAPACITY_PER_CORE); i++) {             \
                cp->free_idx[i] = (CAPACITY_PER_CORE) - 1 - i;               \
            }                                                                 \
            cp->top = (CAPACITY_PER_CORE);                                   \
        }                                                                     \
    }                                                                         \
                                                                               \
    /* Returns NULL if core_id's pool is exhausted. */                       \
    static inline TYPE *NAME##_alloc(NAME##_t *p, uint32_t core_id) {        \
        NAME##_core_pool_t *cp = &p->core[core_id];                          \
        if (cp->top == 0) {                                                   \
            return NULL;                                                      \
        }                                                                     \
        return &cp->slots[cp->free_idx[--cp->top]];                          \
    }                                                                         \
                                                                               \
    /* item must have been allocated from this same core's pool. */         \
    static inline void NAME##_free(NAME##_t *p, uint32_t core_id,            \
                                    TYPE *item) {                             \
        NAME##_core_pool_t *cp = &p->core[core_id];                          \
        cp->free_idx[cp->top++] = (uint32_t)(item - cp->slots);              \
    }                                                                         \
                                                                               \
    static inline size_t NAME##_capacity_per_core(void) {                    \
        return (CAPACITY_PER_CORE);                                           \
    }

#endif /* LL_MEMPOOL_PERCORE_H */
