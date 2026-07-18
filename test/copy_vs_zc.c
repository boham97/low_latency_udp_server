#include <stdio.h>
#include <string.h>
#include <stdint.h>

// 들어온 패킷이 이미 여기 있다고 하자 (수신 버퍼)
static uint8_t rx_buffer[1500];

// -------- 나쁜 습관: 패킷을 내 구조체로 통째로 복사한 뒤 파싱 --------
typedef struct { uint8_t type; uint8_t payload[1499]; } BigMsg;

uint32_t parse_with_copy(const uint8_t *buf, int len) {
    BigMsg m;
    memcpy(&m, buf, len);            // ← 1500바이트 통째 복사! (낭비)
    uint32_t size;
    memcpy(&size, m.payload, 4);     // 그 복사본에서 필드 읽기
    return size;
}

// -------- 제로카피: 통째 복사 없이, 들어온 버퍼 위에서 필드만 --------
uint32_t parse_zero_copy(const uint8_t *buf, int len) {
    (void)len;
    uint32_t size;
    memcpy(&size, buf + 1, 4);       // ← 필요한 4바이트만. 1500바이트 복사 없음
    return size;
}

int main(void) {
    rx_buffer[0] = 0x38;
    uint32_t v = 100;
    memcpy(rx_buffer + 1, &v, 4);

    printf("복사 방식   : size=%u  (1500B 통째 복사 후 읽음)\n",
           parse_with_copy(rx_buffer, 1500));
    printf("제로카피    : size=%u  (버퍼 그 자리에서 4B만 읽음)\n",
           parse_zero_copy(rx_buffer, 1500));
    return 0;
}
