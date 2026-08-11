/*
 * deep_decode.h — rx_msg_t(와이어 packed) → deep_update_t(정렬 완료)
 *
 * test/parse/의 파싱 로직 중 PLU에 해당하는 부분만 승격한 것. test/parse/는
 * 세 파일에 복붙 계보가 있었는데, 그건 "파일을 어떻게 읽는가"(fread/libpcap/mmap)가
 * 달랐을 뿐 메시지 해석은 같았다. 여기 한 벌만 남기고 파이프라인은 이것만 쓴다.
 */
#ifndef LL_DEEP_DECODE_H
#define LL_DEEP_DECODE_H

#include <ll/deep_msg.h>
#include <ll/rx_msg.h>
#include <ll/symtab.h>

typedef struct {
    uint64_t decoded;
    uint64_t sym_dropped; /* 심볼 테이블이 꽉 차 버린 메시지 */
} deep_stats_t;

/*
 * 헤더 인라인인 이유: 본문이 필드 재배치 몇 개라 호출 오버헤드가 본체보다 크다.
 * 스테이지를 나누는 게 이득인지 판단하려면 디코드 자체는 최소 비용이어야 한다.
 *
 * 반환 0 = 심볼 인덱스를 못 받아 버림 (out은 건드리지 않음).
 */
static inline int deep_decode(const rx_msg_t *in, ll_symtab_t *tab, deep_update_t *out,
                              deep_stats_t *st) {
    uint32_t sym = ll_symtab_intern(tab, ll_sym_key(in->plu.symbol));
    if (sym == LL_SYM_INVALID) {
        st->sym_dropped++;
        return 0;
    }

    out->rx_tsc = in->rx_tsc;
    out->seq = in->seq;
    out->rx_flags = in->rx_flags;

    /* packed 구조체에서 읽는 이 네 줄이 유일한 비정렬 로드다. 여기서 정렬된
       자리로 옮겨두면 북 적용 루프는 전부 정렬 접근이 된다 */
    out->ts_ns = in->plu.timestamp;
    out->price = in->plu.price;
    out->size = in->plu.size;
    out->sym = sym;

    /* 타입 바이트를 side로 접어둔다 — 북이 타입 상수를 다시 알 필요가 없다 */
    out->side = (in->plu.type == DEEP_PLU_BUY) ? (uint8_t)DEEP_SIDE_BID : (uint8_t)DEEP_SIDE_ASK;
    out->epc = DEEP_EVENT_COMPLETE(in->plu.flags) ? 1u : 0u;

    st->decoded++;
    return 1;
}

#endif /* LL_DEEP_DECODE_H */
