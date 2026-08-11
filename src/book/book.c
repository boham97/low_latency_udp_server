#include <ll/book.h>

#include <string.h>

void book_set_init(book_set_t *bs) {
    /*
     * 레벨 배열은 일부러 안 지운다. book_set_t는 19MB가 넘고, memset은 그
     * 전부를 물리 페이지로 끌어온다 — 실제로 쓰는 심볼이 39개일 때도.
     * 정적 저장기간(BSS)이나 calloc으로 이미 0인 메모리를 전제하고,
     * 여기서는 n[]을 신뢰의 근거로 삼는다 (n==0이면 레벨 내용은 안 읽힌다).
     */
    memset(&bs->st, 0, sizeof bs->st);
}

void book_apply(book_set_t *bs, const deep_update_t *u) {
    book_t *b = &bs->sym[u->sym];
    uint32_t s = u->side;
    book_level_t *lv = b->level[s];
    uint16_t n = b->n[s];

    /*
     * bid는 내림차순, ask는 오름차순으로 정렬돼 있다. 부호를 뒤집으면 두 사이드가
     * 똑같이 "키 오름차순"이 되어 탐색/삽입 코드가 한 벌로 끝난다 — 사이드마다
     * 비교 방향이 다른 코드를 두 벌 두면 한쪽만 고치는 버그가 난다.
     */
    int64_t sign = (s == DEEP_SIDE_BID) ? -1 : 1;
    int64_t key = sign * u->price;

    /* 정렬 배열 선형 탐색. 실측 깊이가 14라 이분탐색의 분기 예측 실패가 더 비싸다 */
    uint16_t i = 0;
    while (i < n && sign * lv[i].price < key) {
        i++;
    }
    /* i = 키가 key 이상인 첫 자리. 그 자리가 같은 가격이면 갱신/삭제, 아니면 삽입점 */
    int found = (i < n && lv[i].price == u->price);

    if (u->size == 0) {
        if (!found) {
            /* 갭 때문만은 아니다 (book.h 주석) — 세는 것 말고 할 게 없다 */
            bs->st.delete_missing++;
            bs->st.applied++;
            return;
        }
        memmove(&lv[i], &lv[i + 1], (size_t)(n - i - 1) * sizeof lv[0]);
        b->n[s] = --n;
        bs->st.deleted++;
    } else if (found) {
        lv[i].size = u->size;
        bs->st.updated++;
    } else {
        if (n == BOOK_MAX_LEVELS) {
            bs->st.overflow++;
            if (i == n) {
                /* 새 레벨이 best에서 가장 먼 자리 — 어차피 밀려날 것이니 안 넣는다 */
                bs->st.applied++;
                return;
            }
            n--; /* 최악 레벨 하나를 떨어뜨려 자리를 만든다 (BBO는 영향 없음) */
        }
        memmove(&lv[i + 1], &lv[i], (size_t)(n - i) * sizeof lv[0]);
        lv[i].price = u->price;
        lv[i].size = u->size;
        lv[i]._pad = 0;
        b->n[s] = ++n;
        bs->st.inserted++;
    }

    bs->st.applied++;
    if (n > bs->st.max_depth) {
        bs->st.max_depth = n;
    }
}

void book_bbo(const book_set_t *bs, uint32_t sym, bbo_t *out) {
    const book_t *b = &bs->sym[sym];

    out->has_bid = b->n[DEEP_SIDE_BID] > 0;
    out->has_ask = b->n[DEEP_SIDE_ASK] > 0;
    out->bid_px = out->has_bid ? b->level[DEEP_SIDE_BID][0].price : 0;
    out->bid_sz = out->has_bid ? b->level[DEEP_SIDE_BID][0].size : 0;
    out->ask_px = out->has_ask ? b->level[DEEP_SIDE_ASK][0].price : 0;
    out->ask_sz = out->has_ask ? b->level[DEEP_SIDE_ASK][0].size : 0;
}
