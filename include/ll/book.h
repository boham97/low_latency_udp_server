/*
 * book.h — L2 오더북 (price-aggregated)
 *
 * DEEP는 "이 가격 레벨의 집계 수량은 N"을 통째로 준다. 주문 ID 추적도, 주문
 * 노드 풀도 없다. 사이드당 {price,size} 정렬 배열 하나가 전부다.
 *
 * 레이아웃: 양쪽 다 **[0]이 best**다.
 *   bid — 가격 내림차순 (bid[0] = 최고 매수)
 *   ask — 가격 오름차순 (ask[0] = 최저 매도)
 * BBO 발행이 배열 첫 원소 두 개 읽기로 끝난다. 대신 best 근처 삽입/삭제가
 * memmove를 유발하는데, 실측 pcap의 최대 깊이가 사이드당 14레벨이라
 * 옮기는 양이 캐시라인 두어 개다. 깊이가 커지면 여기가 먼저 아프다.
 *
 * 이건 **의도적으로 순진한 첫 판**이다. CLAUDE.md의 다음 단계(price-tick 직접
 * 인덱싱 + best 포인터 캐싱)는 이 버전의 측정값이 있어야 비교 대상이 생긴다.
 */
#ifndef LL_BOOK_H
#define LL_BOOK_H

#include <stdalign.h>
#include <stdint.h>

#include <ll/deep_msg.h>
#include <ll/symtab.h>

#define BOOK_MAX_LEVELS 64 /* 실측 최대 깊이 14 — 초과분은 overflow로 센다 */
#define BOOK_MAX_SYMBOLS LL_SYMTAB_MAX_SYMBOLS

typedef struct {
    int64_t price;
    uint32_t size;
    uint32_t _pad;
} book_level_t;

_Static_assert(sizeof(book_level_t) == 16, "book_level_t must be 16 bytes");

typedef struct {
    /* 헤더를 캐시라인 하나로 밀어내 레벨 배열이 라인 경계에서 시작하게 한다.
       memmove가 라인을 하나 덜 건드린다 */
    alignas(64) uint16_t n[DEEP_SIDES];
    uint8_t _pad[60];
    book_level_t level[DEEP_SIDES][BOOK_MAX_LEVELS];
} book_t;

typedef struct {
    int64_t bid_px, ask_px;
    uint32_t bid_sz, ask_sz;
    uint8_t has_bid, has_ask;
} bbo_t;

/*
 * delete_missing에 대해: 갭의 흔적일 거라 짐작했는데 아니었다. 실측 pcap은
 * seq 1부터 시작(갭 0, 캡처 이전 상태 없음)인데도 16건이 나온다. 파이썬으로
 * 독립 재생해도 같은 16건 — 피드 자체가 없는 레벨에 삭제를 보낸다.
 * 그러니 이 카운터를 갭 지표로 읽으면 안 된다. 갭 판정은 RX_FLAG_AFTER_GAP이
 * 하고, 이건 "무시하고 넘어간 삭제"의 개수일 뿐이다.
 */
typedef struct {
    uint64_t applied;
    uint64_t inserted, updated, deleted;
    uint64_t delete_missing; /* size==0인데 그 레벨이 없었다 (아래 주석) */
    uint64_t overflow;       /* BOOK_MAX_LEVELS 초과로 최악 레벨을 버림 */
    uint32_t max_depth;      /* 관측된 사이드당 최대 레벨 수 */
} book_stats_t;

/*
 * 심볼별 book은 사전 할당 배열. 인덱스가 등장 순서라 실제로 만지는 건 앞쪽
 * 소수뿐이고, 나머지는 BSS 페이지째로 안 건드려진다 (물리 페이지도 안 잡힌다).
 */
typedef struct {
    book_t sym[BOOK_MAX_SYMBOLS];
    book_stats_t st;
} book_set_t;

void book_set_init(book_set_t *bs);

/* PLU 하나를 반영. size>0 → 갱신/삽입, size==0 → 삭제 */
void book_apply(book_set_t *bs, const deep_update_t *u);

/* 현재 BBO를 읽는다. 한쪽이 비었으면 has_bid/has_ask가 0 */
void book_bbo(const book_set_t *bs, uint32_t sym, bbo_t *out);

#endif /* LL_BOOK_H */
