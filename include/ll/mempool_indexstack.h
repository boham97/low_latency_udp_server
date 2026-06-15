#ifndef LL_MEMPOOL_INDEXSTACK_H
#define LL_MEMPOOL_INDEXSTACK_H

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 단일 스레드용 고정 크기 블록 풀. 빈 슬롯 인덱스를 단순 배열 기반
 * LIFO 스택에 보관(포인터 추적 없음, atomic 없음)하므로 alloc/free가
 * 순수 배열 접근만으로 끝남 — 풀을 한 코어의 핫패스에서만 다룰 때 사용.
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
    /* 풀이 고갈되면 NULL 반환. */                                            \
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
