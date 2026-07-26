#include <stdio.h>
#include <string.h>
#include <stdint.h>

// 가짜 패킷: 앞 1바이트는 종류, 그 뒤 4바이트는 size, 그 뒤 4바이트는 price.
// (리틀엔디언으로 넣어둠. size=100, price=12345)
static uint8_t packet[] = {
    0x38,                    // [0]    종류 (Buy = 0x38)
    0x64, 0x00, 0x00, 0x00,  // [1..4] size  = 100
    0x39, 0x30, 0x00, 0x00,  // [5..8] price = 12345
};

int main(void) {
    // ---------- 방법 A: 복사 방식 ----------
    uint32_t size_copied;
    memcpy(&size_copied, packet + 1, 4);   // 버퍼의 바이트를 '새 변수'로 옮겨 담음
    printf("[복사]   size = %u\n", size_copied);

    // ---------- 방법 B: 제로카피 (그 자리에서 읽기) ----------
    // 버퍼 주소에 "여기는 uint32_t다"라고 알려주고, 옮기지 않고 바로 읽음
    uint32_t size_inplace;
    memcpy(&size_inplace, packet + 1, 4);  // ← 사실 필드 4바이트는 이것도 memcpy가 정답
    printf("[제로카피] size = %u (버퍼 원본 주소 %p 에서 바로 읽음)\n",
           size_inplace, (void *)(packet + 1));

    // 핵심 차이: 복사 방식은 '패킷 전체'를 큰 버퍼로 통째로 옮긴 뒤 파싱하지만,
    // 제로카피는 들어온 버퍼 위에서 필요한 필드만 그때그때 읽는다.
    return 0;
}
