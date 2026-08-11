/*
 * deep_msg.h — 디코드 스테이지의 출력 단위
 *
 * rx_msg_t가 "와이어 그대로 30B packed"라면 deep_update_t는 "북이 바로 쓸 수
 * 있는 정렬된 값"이다. 디코드가 하는 일은 셋:
 *   1) packed → 자연 정렬 (북 적용 루프가 비정렬 로드를 안 하게)
 *   2) 심볼 8바이트 → 조밀한 인덱스 (배열 첨자로 쓸 수 있게)
 *   3) 타입 바이트 → side, Event Flags → EPC 비트 (분기를 여기서 끝냄)
 *
 * 가격은 **고정소수점 x10000 그대로 둔다.** CLAUDE.md의 "가격 스케일 해제"에서
 * 벗어난 선택인데 근거가 있다 — 북의 핫패스는 가격을 *비교*하고 *찾을* 뿐
 * 산술을 하지 않는다. double로 바꾸면 정확한 정수 비교가 부동소수점 비교가 되고
 * (틱 경계에서 == 가 어긋날 수 있다), 디코드마다 cvtsi2sd + 나눗셈이 붙는다.
 * 사람이 읽는 십진 변환은 출력 지점에서만 한다.
 */
#ifndef LL_DEEP_MSG_H
#define LL_DEEP_MSG_H

#include <stdalign.h>
#include <stdint.h>

#define DEEP_SIDE_BID 0u
#define DEEP_SIDE_ASK 1u
#define DEEP_SIDES 2u

typedef struct {
    alignas(64) uint64_t rx_tsc; /* Rx가 찍은 기준점 — 구간 지연은 전부 이것과의 차 */
    int64_t seq;                 /* IEX-TP 시퀀스 */
    int64_t ts_ns;               /* 거래소 타임스탬프 */
    int64_t price;               /* 고정소수점 x10000 (위 주석 참고) */
    uint32_t size;               /* 레벨 집계 수량. 0 = 레벨 제거 */
    uint32_t sym;                /* 심볼 인덱스. LL_SYM_INVALID면 버려야 함 */
    uint32_t rx_flags;           /* RX_FLAG_AFTER_GAP 등 */
    uint8_t side;                /* DEEP_SIDE_BID / DEEP_SIDE_ASK */
    uint8_t epc;                 /* Event Processing Complete → 여기서만 BBO 발행 */
    uint8_t _pad[2];
} deep_update_t;

_Static_assert(sizeof(deep_update_t) == 64, "deep_update_t must be exactly one cache line");

#endif /* LL_DEEP_MSG_H */
