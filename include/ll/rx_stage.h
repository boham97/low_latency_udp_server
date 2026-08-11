#ifndef LL_RX_STAGE_H
#define LL_RX_STAGE_H

#include <stdint.h>

#include <ll/rx_msg.h>

/* 핫패스 밖 카운터 — 스테이지가 뭘 버렸는지가 드롭 정책의 근거가 된다 */
typedef struct {
    uint64_t datagrams;   /* DEEP 프로토콜로 확인된 세그먼트 */
    uint64_t non_deep;    /* 다른 프로토콜/너무 짧아 버린 데이터그램 */
    uint64_t heartbeats;  /* message_count == 0 */
    uint64_t messages;    /* 프레이밍에서 꺼낸 전체 메시지 */
    uint64_t filtered;    /* PLU가 아니라 큐에 안 실은 메시지 */
    uint64_t pushed;      /* 큐에 실은 PLU */
    uint64_t dropped_full;/* PLU였지만 큐가 가득 차 버린 것 */
    uint64_t truncated;   /* 블록이 잘려 세그먼트를 중도 포기한 횟수 */
    uint64_t gaps;        /* seq 불연속 발생 횟수 */
    uint64_t gap_msgs;    /* 갭으로 누락 추정된 메시지 수 */
    uint64_t reorders;    /* seq 역전 */
} rx_stats_t;

/*
 * 세그먼트 사이를 넘어가는 유일한 상태. recvfrom 루프 밖으로 빼둔 이유는
 * pcap 리플레이(오프라인)와 UDP 수신이 같은 프레이밍 코드를 쓰게 하기 위해서다.
 * 오프라인에서 확정한 파싱/필터/갭 판정이 라이브 경로에도 그대로 적용된다.
 */
typedef struct {
    int64_t expected_seq;   /* -1 = 아직 첫 세그먼트를 안 봄 */
    uint32_t pending_flags; /* 갭 표시는 "갭 이후 처음 실린 메시지"에 붙는다 */
} rx_framer_t;

void rx_framer_init(rx_framer_t *f);

/*
 * IEX-TP 세그먼트 하나(= UDP 페이로드 하나)를 해체해 PLU만 q에 push.
 * buf/n은 커널이 준 실제 바이트 — 와이어의 payload_len보다 이쪽이 우선한다.
 */
void rx_framer_segment(rx_framer_t *f, const uint8_t *buf, size_t n, uint64_t rx_tsc,
                       rx_queue_t *q, rx_stats_t *st);

/* 수신 소켓 생성 + bind. 실패 시 -1. 핫패스 밖(시작 시 1회). */
int rx_open_socket(int port, int idle_timeout_sec);

/*
 * Rx 스테이지 본체: recvfrom → rx_framer_segment 반복.
 * idle 타임아웃으로 수신이 멎으면 반환한다.
 */
void rx_stage_run(int fd, rx_queue_t *q, rx_stats_t *st);

#endif /* LL_RX_STAGE_H */
