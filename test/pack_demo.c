#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

// 패딩 있음 (기본)
struct normal {
    uint16_t msg_type;
    uint32_t user_id;
    uint16_t body_len;
};

// 패딩 없음
#pragma pack(push, 1)
struct packed {
    uint16_t msg_type;
    uint32_t user_id;
    uint16_t body_len;
};
#pragma pack(pop)

int main(void) {
    printf("=== 기본 (패딩 있음) ===\n");
    printf("전체 크기: %zu 바이트\n", sizeof(struct normal));
    printf("msg_type 위치: %zu\n", offsetof(struct normal, msg_type));
    printf("user_id  위치: %zu\n", offsetof(struct normal, user_id));
    printf("body_len 위치: %zu\n\n", offsetof(struct normal, body_len));

    printf("=== pack(1) (패딩 없음) ===\n");
    printf("전체 크기: %zu 바이트\n", sizeof(struct packed));
    printf("msg_type 위치: %zu\n", offsetof(struct packed, msg_type));
    printf("user_id  위치: %zu\n", offsetof(struct packed, user_id));
    printf("body_len 위치: %zu\n", offsetof(struct packed, body_len));
    return 0;
}
