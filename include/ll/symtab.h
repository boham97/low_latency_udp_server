/*
 * symtab.h — 심볼(8바이트) → 조밀한 인덱스
 *
 * 오더북은 심볼당 고정 구조체를 사전 할당한다. 그러려면 와이어의 8바이트
 * 심볼을 배열 첨자로 바꿔야 하는데, 이 변환이 디코드 핫패스에서 유일하게
 * 자명하지 않은 비용이다.
 *
 * 왜 해시인가: 심볼은 공백패딩 ASCII 8바이트 = uint64 하나로 통째로 읽힌다
 * (strcmp도, 길이 계산도 필요 없다). 오픈 어드레싱 + 선형 탐사로 슬롯 배열만
 * 사전 할당해두면 런타임 할당이 0이다.
 *
 * 왜 테이블이 커도 되는가: 슬롯 배열은 16K개(256KB)라 L2를 넘지만, 한 세션에서
 * 실제로 탐사되는 슬롯은 **활성 심볼 수**만큼이다 (실측 pcap 39개). 안 닿는
 * 슬롯은 캐시에 안 올라온다 — 테이블 크기가 아니라 활성 심볼 수가 워킹셋이다.
 *
 * 인덱스는 **등장 순서대로** 발급된다. 파일/세션이 바뀌면 같은 심볼도 다른
 * 인덱스를 받는다 — 인덱스를 세션 밖으로 들고 나가지 말 것.
 */
#ifndef LL_SYMTAB_H
#define LL_SYMTAB_H

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#define LL_SYMTAB_SLOTS 16384u  /* 2의 거듭제곱 — 마스크로 감쌀 수 있어야 한다 */
#define LL_SYMTAB_SLOT_BITS 14
#define LL_SYMTAB_MAX_SYMBOLS 9216u /* IEX 유니버스(실측 8,572) + 여유 */
#define LL_SYM_INVALID UINT32_MAX

typedef struct {
    uint64_t key; /* 심볼 8바이트 그대로. 0 = 빈 슬롯 */
    uint32_t idx;
    uint32_t _pad;
} ll_symtab_slot_t;

typedef struct {
    alignas(64) ll_symtab_slot_t slot[LL_SYMTAB_SLOTS];
    uint64_t name[LL_SYMTAB_MAX_SYMBOLS]; /* idx → 원본 8바이트 (출력용, 핫패스 아님) */
    uint32_t count;
    uint64_t full_drops; /* 테이블이 차서 인덱스를 못 준 횟수 */
} ll_symtab_t;

/*
 * 와이어의 심볼 8바이트를 키로. memcpy인 이유는 두 가지 —
 * PLU가 packed 구조체라 symbol이 off10(비정렬)이고, char*를 uint64*로 캐스팅하면
 * strict aliasing 위반이다. -O2에서 memcpy는 mov 한 개로 접힌다.
 *
 * 키 0은 "빈 슬롯"으로 예약돼 있다. 심볼은 공백패딩 ASCII라 전부 0인 8바이트는
 * 나올 수 없으므로 충돌하지 않는다.
 */
static inline uint64_t ll_sym_key(const char sym[8]) {
    uint64_t k;
    memcpy(&k, sym, 8);
    return k;
}

void ll_symtab_init(ll_symtab_t *t);

/* 없으면 새 인덱스를 발급, 있으면 기존 인덱스. 테이블이 꽉 차면 LL_SYM_INVALID */
uint32_t ll_symtab_intern(ll_symtab_t *t, uint64_t key);

/* idx → 공백 제거한 C 문자열 (out은 9바이트 이상) */
void ll_symtab_name(const ll_symtab_t *t, uint32_t idx, char out[9]);

#endif /* LL_SYMTAB_H */
