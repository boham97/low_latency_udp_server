#ifndef LL_RX_MSG_H
#define LL_RX_MSG_H

#include <stdalign.h>
#include <stdint.h>

#include <ll/iex_deep_wire.h>
#include <ll/spsc_queue_cached.h>

/*
 * Rx → 디코드 스테이지 큐에 실리는 단위.
 *
 * 데이터그램 전체가 아니라 **Price Level Update 메시지 하나**가 슬롯 하나다.
 * Rx가 IEX-TP 프레이밍을 벗기면서 오더북에 쓰이지 않는 메시지(체결/관리/
 * heartbeat)를 여기서 쳐내므로, 큐를 건너는 것은 전부 북에 반영될 메시지다.
 *
 * 대신 CLAUDE.md가 상정한 "recvfrom이 링 슬롯에 직접 쓴다"는 성립하지 않는다 —
 * 데이터그램 안에서 필요한 조각만 골라내는 이상 커널이 최종 목적지를 알 수 없다.
 * PLU 30바이트 복사 한 번을 내주고 얻는 것:
 *   - 슬롯 2KB → 64B (cap=1024 기준 링 2MB → 64KB, L2에 들어간다)
 *   - 비-PLU 트래픽이 스테이지 경계를 넘지 않는다 (다음 스테이지 일감 자체가 줄어듦)
 *   - 슬롯 하나 = 캐시라인 하나 (아래 _Static_assert)
 *
 * plu는 와이어 레이아웃 그대로 둔다. 정렬 복원/심볼→인덱스/가격 스케일은
 * 디코드 스테이지의 일이다.
 */

#define RX_FLAG_AFTER_GAP 0x1u /* 직전에 seq 갭 — 이 시점 북 상태는 불완전 */

typedef struct {
    /* 첫 멤버 alignas(64) → 구조체 크기/정렬이 64로 올라가 슬롯이 라인을 안 걸친다 */
    alignas(64) uint64_t rx_tsc; /* recvfrom 복귀 직후 (데이터그램당 1회) */
    int64_t seq;                 /* IEX-TP 시퀀스 — 갭 판정 근거를 그대로 전달 */
    uint32_t rx_flags;
    deep_plu_t plu; /* 와이어 그대로 30B */
} rx_msg_t;

_Static_assert(sizeof(rx_msg_t) == 64, "rx_msg_t must be exactly one cache line");

/* cached 변형만 인스턴스화한다 — bench/results.md에서 두 깊이 모두 throughput 최상위 */
#define RX_QUEUE_CAPACITY 1024
LL_SPSC_CACHED_DEFINE(rx_queue, rx_msg_t, RX_QUEUE_CAPACITY)

#endif /* LL_RX_MSG_H */
