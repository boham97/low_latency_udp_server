#include <ll/symtab.h>

void ll_symtab_init(ll_symtab_t *t) {
    memset(t, 0, sizeof *t);
}

/*
 * Fibonacci hashing: 황금비 상수를 곱하고 상위 비트만 취한다.
 * 심볼 키는 ASCII라 하위 바이트가 몰려 있어(대문자 A-Z 구간) 하위 비트를
 * 그대로 마스킹하면 군집이 그대로 슬롯 군집이 된다. 곱셈은 그 엔트로피를
 * 상위로 밀어올린다 — imul 1개 + shr 1개.
 */
static inline uint32_t sym_hash(uint64_t key) {
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - LL_SYMTAB_SLOT_BITS));
}

uint32_t ll_symtab_intern(ll_symtab_t *t, uint64_t key) {
    uint32_t i = sym_hash(key);

    for (uint32_t probe = 0; probe < LL_SYMTAB_SLOTS; ++probe) {
        ll_symtab_slot_t *s = &t->slot[i];

        if (s->key == key) {
            return s->idx;
        }
        if (s->key == 0) {
            /* 빈 슬롯을 만났다 = 이 키는 테이블에 없다. 선형 탐사라 중간에
               빈 칸이 있으면 그 뒤는 볼 필요가 없다 (삭제가 없으므로 성립) */
            if (t->count >= LL_SYMTAB_MAX_SYMBOLS) {
                t->full_drops++;
                return LL_SYM_INVALID;
            }
            uint32_t idx = t->count++;
            s->key = key;
            s->idx = idx;
            t->name[idx] = key;
            return idx;
        }
        i = (i + 1) & (LL_SYMTAB_SLOTS - 1);
    }

    /* 슬롯을 한 바퀴 다 돌았다 — MAX_SYMBOLS < SLOTS면 도달 불가 */
    t->full_drops++;
    return LL_SYM_INVALID;
}

void ll_symtab_name(const ll_symtab_t *t, uint32_t idx, char out[9]) {
    if (idx >= t->count) {
        out[0] = '?';
        out[1] = 0;
        return;
    }
    memcpy(out, &t->name[idx], 8);
    int len = 8;
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == 0)) len--;
    out[len] = 0;
}
