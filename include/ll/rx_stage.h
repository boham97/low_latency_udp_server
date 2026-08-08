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

/* 수신 소켓 생성 + bind. 실패 시 -1. 핫패스 밖(시작 시 1회). */
int rx_open_socket(int port, int idle_timeout_sec);

/*
 * Rx 스테이지 본체: recvfrom → IEX-TP 프레이밍 → PLU만 q에 push.
 * idle 타임아웃으로 수신이 멎으면 반환한다.
 */
void rx_stage_run(int fd, rx_queue_t *q, rx_stats_t *st);

#endif /* LL_RX_STAGE_H */
