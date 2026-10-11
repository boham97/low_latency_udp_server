#ifndef LL_BENCH_BOOK_REV_H
#define LL_BENCH_BOOK_REV_H

#include <ll/book.h>

/* book_apply/book_bbo와 같은 계약, 레이아웃만 [n-1]이 best */
void book_rev_apply(book_set_t *bs, const deep_update_t *u);
void book_rev_bbo(const book_set_t *bs, uint32_t sym, bbo_t *out);

#endif
