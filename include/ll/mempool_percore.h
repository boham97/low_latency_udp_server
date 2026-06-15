#ifndef LL_MEMPOOL_PERCORE_H
#define LL_MEMPOOL_PERCORE_H

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 코어별 고정 크기 블록 풀: 각 코어가 자신만의 free-index 스택을
 * 가지며 코어 간 동기화가 없으므로 alloc/free가 atomic을 전혀 건드리지
 * 않음. 단, 한 코어의 풀에서 할당한 블록은 반드시 같은 코어의 풀로
 * 반환해야 함 — 마이그레이션 미지원, 호출자 책임.
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
    /* core_id의 풀이 고갈되면 NULL 반환. */                                  \
    static inline TYPE *NAME##_alloc(NAME##_t *p, uint32_t core_id) {        \
        NAME##_core_pool_t *cp = &p->core[core_id];                          \
        if (cp->top == 0) {                                                   \
            return NULL;                                                      \
        }                                                                     \
        return &cp->slots[cp->free_idx[--cp->top]];                          \
    }                                                                         \
                                                                               \
    /* item은 반드시 같은 코어의 풀에서 할당된 것이어야 함. */              \
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
