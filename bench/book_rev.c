/*
 * book_rev.c — 역순 정렬 배열 변형 (book.c와 같은 book_set_t를 쓴다)
 *
 * book.c는 [0]이 best라 best 삽입/삭제가 레벨 전부를 memmove한다. 실측 pcap에서
 * insert의 68%, delete의 83%가 best 자리다. 여기서는 [n-1]을 best로 둬서 그 경우
 * 옮길 원소가 0이 되게 한다. 탐색도 끝(best)에서 거꾸로 해 걸음 수는 같다.
 *
 * 별도 번역 단위로 둔 건 공정성 때문이다 — book.c의 book_apply가 벤치 루프에
 * 인라인되지 않는 외부 호출이므로 이쪽도 같은 조건이어야 한다.
 */
#include "book_rev.h"

#include <string.h>

void book_rev_apply(book_set_t *bs, const deep_update_t *u) {
    book_t *b = &bs->sym[u->sym];
    uint32_t s = u->side;
    book_level_t *lv = b->level[s];
    uint16_t n = b->n[s];

    /* book.c와 같은 키: 작을수록 best. 배열은 키 내림차순 → lv[n-1]이 best */
    int64_t sign = (s == DEEP_SIDE_BID) ? -1 : 1;
    int64_t key = sign * u->price;

    /* best부터 거꾸로. j = best 쪽에서 key보다 나은 레벨들이 끝나는 자리 */
    uint16_t j = n;
    while (j > 0 && sign * lv[j - 1].price < key) {
        j--;
    }
    int found = (j > 0 && lv[j - 1].price == u->price);

    if (u->size == 0) {
        if (!found) {
            bs->st.delete_missing++;
            bs->st.applied++;
            return;
        }
        /* lv[j-1] 삭제: 그보다 나은 n-j개만 당긴다. best 삭제면 0 */
        memmove(&lv[j - 1], &lv[j], (size_t)(n - j) * sizeof lv[0]);
        b->n[s] = --n;
        bs->st.deleted++;
    } else if (found) {
        lv[j - 1].size = u->size;
        bs->st.updated++;
    } else {
        if (n == BOOK_MAX_LEVELS) {
            bs->st.overflow++;
            if (j == 0) {
                /* 새 레벨이 최악 — book.c와 같은 정책으로 안 넣는다 */
                bs->st.applied++;
                return;
            }
            /* 최악(lv[0])을 버리고 [1, j)를 한 칸 당기면 j-1이 빈다. 그보다 나은
               레벨은 제자리라 두 번째 memmove를 하면 안 된다 (합성 검증이 잡은 버그) */
            memmove(&lv[0], &lv[1], (size_t)(j - 1) * sizeof lv[0]);
            j--;
        } else {
            /* lv[j]에 삽입: 그보다 나은 n-j개만 민다. 새 best면 0 */
            memmove(&lv[j + 1], &lv[j], (size_t)(n - j) * sizeof lv[0]);
            b->n[s] = ++n;
        }
        lv[j].price = u->price;
        lv[j].size = u->size;
        lv[j]._pad = 0;
        bs->st.inserted++;
    }

    bs->st.applied++;
    if (n > bs->st.max_depth) {
        bs->st.max_depth = n;
    }
}

void book_rev_bbo(const book_set_t *bs, uint32_t sym, bbo_t *out) {
    const book_t *b = &bs->sym[sym];
    uint16_t nb = b->n[DEEP_SIDE_BID], na = b->n[DEEP_SIDE_ASK];

    out->has_bid = nb > 0;
    out->has_ask = na > 0;
    out->bid_px = nb ? b->level[DEEP_SIDE_BID][nb - 1].price : 0;
    out->bid_sz = nb ? b->level[DEEP_SIDE_BID][nb - 1].size : 0;
    out->ask_px = na ? b->level[DEEP_SIDE_ASK][na - 1].price : 0;
    out->ask_sz = na ? b->level[DEEP_SIDE_ASK][na - 1].size : 0;
}
