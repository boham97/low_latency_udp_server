/*
 * iex_deep_wire.h — IEX-TP / DEEP 와이어 포맷을 그대로 얹는 packed 구조체
 *
 * 수신 버퍼(바이트 배열)를 이 구조체 포인터로 캐스팅하면 필드가 곧바로 읽힌다
 * (zero-copy struct mapping). byte-shift 파싱을 안 쓰는 대신 두 가지 전제가 있다:
 *
 *   1) 호스트가 little-endian (DEEP 와이어가 LE). x86-64/WSL2는 LE라 그대로 맞음.
 *      big-endian 호스트에선 각 다중바이트 필드를 bswap 해야 한다.
 *   2) 필드가 자연 정렬 경계에 안 맞음 (예: timestamp가 off2, price가 off22).
 *      그래서 #pragma pack(1) 로 패딩을 없애고, 컴파일러가 misaligned 접근을
 *      바이트 단위로 처리하게 한다. x86은 misaligned load 자체도 허용.
 *
 * 오프셋은 IEX DEEP 스펙 v1.x 기준. 버전 바뀌면 스펙 PDF로 재대조할 것.
 */
#ifndef IEX_DEEP_WIRE_H
#define IEX_DEEP_WIRE_H

#include <stdint.h>
#include <assert.h>

#define IEXTP_PROTO_DEEP 0x8004

#define DEEP_PLU_BUY  0x38   /* '8' Price Level Update (Buy)  */
#define DEEP_PLU_SELL 0x35   /* '5' Price Level Update (Sell) */

#pragma pack(push, 1)

/* IEX-TP 전송 헤더 (40바이트). UDP 페이로드 맨 앞에 위치. */
typedef struct {
    uint8_t  version;        /* off 0  */
    uint8_t  reserved;       /* off 1  */
    uint16_t protocol_id;    /* off 2  0x8004 = DEEP */
    uint32_t channel_id;     /* off 4  */
    uint32_t session_id;     /* off 8  */
    uint16_t payload_len;    /* off 12 이 헤더 뒤 메시지 블록들의 총 바이트 */
    uint16_t message_count;  /* off 14 뒤따르는 메시지 개수 (0 = heartbeat) */
    int64_t  stream_offset;  /* off 16 */
    int64_t  first_seq;      /* off 24 첫 메시지의 시퀀스 번호 */
    int64_t  send_time;      /* off 32 세그먼트 송신 시각 (ns) */
} iextp_header_t;

/* 각 메시지 블록 앞의 2바이트 길이 프리픽스 */
typedef struct {
    uint16_t length;         /* 뒤따르는 메시지 바디 바이트 수 */
    uint8_t  body[];         /* length 바이트 */
} iextp_msg_block_t;

/* 모든 DEEP 메시지의 공통 앞부분 (타입 판별용) */
typedef struct {
    uint8_t type;            /* off 0 '8','5','T',... */
    uint8_t flags;           /* off 1 타입별 (PLU=Event Flags) */
    int64_t timestamp;       /* off 2 ns since epoch */
} deep_common_t;

/* Price Level Update (Buy=0x38 / Sell=0x35), 30바이트 */
typedef struct {
    uint8_t  type;           /* off 0  */
    uint8_t  flags;          /* off 1  bit0 = Event Processing Complete */
    int64_t  timestamp;      /* off 2  */
    char     symbol[8];      /* off 10 공백패딩 */
    uint32_t size;           /* off 18 레벨 집계 수량 (0 = 레벨 제거) */
    int64_t  price;          /* off 22 고정소수점 x10000 */
} deep_plu_t;

#pragma pack(pop)

/* 와이어 크기가 스펙과 어긋나면 빌드 자체를 실패시킨다 */
_Static_assert(sizeof(iextp_header_t) == 40, "IEX-TP header must be 40 bytes");
_Static_assert(sizeof(deep_plu_t)     == 30, "DEEP PLU must be 30 bytes");

#define DEEP_EVENT_COMPLETE(flags) (((flags) & 0x01) != 0)

#endif /* IEX_DEEP_WIRE_H */
