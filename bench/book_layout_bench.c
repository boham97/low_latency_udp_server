/*
 * book_layout_bench.c — 오더북 레이아웃 비교: [0]=best (book.c) vs [n-1]=best (book_rev.c)
 *
 * 입력은 실측 pcap을 book_offline과 같은 경로(rx_framer_segment → deep_decode)로
 * 미리 디코드해 둔 deep_update_t 배열이다. 재는 건 book_apply 하나뿐 —
 * 프레이밍/디코드가 섞이면 북 차이가 그 노이즈에 묻힌다.
 *
 * 모드:
 *   check — 두 구현을 나란히 돌려 매 갱신 뒤 BBO, 끝에서 레벨 전부와 통계를 대조
 *   fwd   — book.c 측정
 *   rev   — book_rev.c 측정
 * fwd/rev는 별도 프로세스로 번갈아 여러 번 돌려 중앙값을 본다 (bench/results.md 관례:
 * 한 프로세스에서 연달아 돌리면 앞 실행이 뒤를 예열한다).
 *
 * 측정 두 가지:
 *   throughput — 루프 전체 TSC / 적용 수. 호출당 rdtsc가 없어 가장 깨끗하다.
 *   per-call   — 호출마다 rdtsc 쌍. rdtsc 오버헤드가 양쪽에 똑같이 붙으니 차이만 본다.
 *
 * Run: book_layout_bench <pcap> <check|fwd|rev> [reps]
 */
#include "book_rev.h"

#include <ll/book.h>
#include <ll/deep_decode.h>
#include <ll/rx_stage.h>
#include <ll/tsc.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static rx_queue_t g_queue;
static ll_symtab_t g_symtab;
static rx_stats_t g_rx;
static deep_stats_t g_dec;
static rx_framer_t g_framer;

static book_set_t g_a, g_b; /* BSS — 쓰는 심볼 페이지만 잡힌다 */

static deep_update_t *g_ups;
static size_t g_n, g_cap;

static void collect(void) {
    for (;;) {
        rx_msg_t *m = rx_queue_pop_begin(&g_queue);
        if (!m) return;
        deep_update_t u;
        int ok = deep_decode(m, &g_symtab, &u, &g_dec);
        rx_queue_pop_end(&g_queue);
        if (!ok) continue;
        if (g_n == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 65536;
            g_ups = realloc(g_ups, g_cap * sizeof *g_ups);
            if (!g_ups) { perror("realloc"); exit(1); }
        }
        g_ups[g_n++] = u;
    }
}

static void parse_frame(const uint8_t *f, uint32_t caplen) {
    if (caplen < 14) return;
    uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
    uint32_t off = 14;
    while (eth == 0x8100 && off + 4 <= caplen) {
        eth = (uint16_t)((f[off + 2] << 8) | f[off + 3]);
        off += 4;
    }
    if (eth != 0x0800 || off + 20 > caplen) return;
    const uint8_t *ip = f + off;
    if ((ip[0] >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;
    if (ihl < 20 || off + ihl > caplen || ip[9] != 17) return;
    uint32_t uo = off + ihl;
    if (uo + 8 > caplen) return;
    uint16_t ulen = (uint16_t)((f[uo + 4] << 8) | f[uo + 5]);
    if (ulen < 8) return;
    uint32_t po = uo + 8, pl = (uint32_t)(ulen - 8);
    if (po + pl > caplen) pl = caplen - po;
    rx_framer_segment(&g_framer, f + po, pl, 0, &g_queue, &g_rx);
    collect();
}

static void load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("open"); exit(1); }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (!buf || fread(buf, 1, (size_t)fsz, fp) != (size_t)fsz) { fprintf(stderr, "read failed\n"); exit(1); }
    fclose(fp);
    if (fsz < 4 || rd32(buf) != 0x0A0D0D0A) { fprintf(stderr, "not pcapng\n"); exit(1); }

    rx_queue_init(&g_queue);
    ll_symtab_init(&g_symtab);
    rx_framer_init(&g_framer);

    long off = 0;
    while (off + 12 <= fsz) {
        uint32_t btype = rd32(buf + off), blen = rd32(buf + off + 4);
        if (blen < 12 || off + (long)blen > fsz) break;
        if (btype == 6) {
            uint32_t caplen = rd32(buf + off + 20);
            if (off + 28 + (long)caplen <= fsz) parse_frame(buf + off + 28, caplen);
        } else if (btype == 3) {
            uint32_t orig = rd32(buf + off + 8), caplen = blen - 16;
            if (caplen > orig) caplen = orig;
            parse_frame(buf + off + 12, caplen);
        }
        off += blen;
    }
    free(buf);
}

/* 레벨 배열은 n[]이 신뢰의 근거라 n만 지우면 빈 책이다 (book_set_init과 같은 전제) */
static void reset(book_set_t *bs) {
    for (uint32_t s = 0; s < g_symtab.count; ++s) {
        bs->sym[s].n[0] = bs->sym[s].n[1] = 0;
    }
    memset(&bs->st, 0, sizeof bs->st);
}

static int bbo_eq(const bbo_t *x, const bbo_t *y) {
    return x->has_bid == y->has_bid && x->has_ask == y->has_ask && x->bid_px == y->bid_px &&
           x->bid_sz == y->bid_sz && x->ask_px == y->ask_px && x->ask_sz == y->ask_sz;
}

static int check(void) {
    book_set_init(&g_a);
    book_set_init(&g_b);
    for (size_t i = 0; i < g_n; ++i) {
        book_apply(&g_a, &g_ups[i]);
        book_rev_apply(&g_b, &g_ups[i]);
        bbo_t x, y;
        book_bbo(&g_a, g_ups[i].sym, &x);
        book_rev_bbo(&g_b, g_ups[i].sym, &y);
        if (!bbo_eq(&x, &y)) {
            fprintf(stderr, "BBO mismatch at update %zu (seq %" PRId64 ")\n", i, g_ups[i].seq);
            return 1;
        }
    }
    /* BBO만 맞고 깊은 레벨이 틀릴 수 있으니 끝에서 전 레벨 대조 */
    for (uint32_t s = 0; s < g_symtab.count; ++s) {
        for (uint32_t d = 0; d < DEEP_SIDES; ++d) {
            uint16_t n = g_a.sym[s].n[d];
            if (n != g_b.sym[s].n[d]) { fprintf(stderr, "depth mismatch sym %u side %u\n", s, d); return 1; }
            for (uint16_t k = 0; k < n; ++k) {
                const book_level_t *p = &g_a.sym[s].level[d][k];
                const book_level_t *q = &g_b.sym[s].level[d][n - 1 - k];
                if (p->price != q->price || p->size != q->size) {
                    fprintf(stderr, "level mismatch sym %u side %u lvl %u\n", s, d, k);
                    return 1;
                }
            }
        }
    }
    if (memcmp(&g_a.st, &g_b.st, sizeof g_a.st) != 0) { fprintf(stderr, "stats mismatch\n"); return 1; }

    printf("check OK: %zu updates, %u symbols — 매 갱신 BBO / 최종 전 레벨 / 통계 일치\n", g_n, g_symtab.count);
    printf("  insert %" PRIu64 " update %" PRIu64 " delete %" PRIu64 " delete_missing %" PRIu64
           " overflow %" PRIu64 " max_depth %u\n",
           g_a.st.inserted, g_a.st.updated, g_a.st.deleted, g_a.st.delete_missing, g_a.st.overflow,
           g_a.st.max_depth);
    return 0;
}

/*
 * 실측 pcap은 overflow가 0건이라 BOOK_MAX_LEVELS 초과 경로를 안 탄다. 두 구현이
 * 가장 다르게 생긴 곳이 거기라, 가격 폭을 레벨 한도보다 넓힌 랜덤 스트림으로 따로 민다.
 */
static int check_synth(void) {
    book_set_init(&g_a);
    book_set_init(&g_b);
    reset(&g_a);
    reset(&g_b);
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 2000000; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        deep_update_t u = {0};
        u.sym = (uint32_t)(x % 4);
        u.side = (uint8_t)((x >> 8) & 1);
        u.price = 1000000 + (int64_t)((x >> 16) % 200) * 100;
        u.size = ((x >> 40) % 4 == 0) ? 0 : (uint32_t)((x >> 44) % 1000 + 1);
        book_apply(&g_a, &u);
        book_rev_apply(&g_b, &u);
        bbo_t p, q;
        book_bbo(&g_a, u.sym, &p);
        book_rev_bbo(&g_b, u.sym, &q);
        if (!bbo_eq(&p, &q)) { fprintf(stderr, "synth BBO mismatch at %d\n", i); return 1; }
    }
    for (uint32_t s = 0; s < 4; ++s) {
        for (uint32_t d = 0; d < DEEP_SIDES; ++d) {
            uint16_t n = g_a.sym[s].n[d];
            if (n != g_b.sym[s].n[d]) { fprintf(stderr, "synth depth mismatch\n"); return 1; }
            for (uint16_t k = 0; k < n; ++k) {
                if (g_a.sym[s].level[d][k].price != g_b.sym[s].level[d][n - 1 - k].price ||
                    g_a.sym[s].level[d][k].size != g_b.sym[s].level[d][n - 1 - k].size) {
                    fprintf(stderr, "synth level mismatch\n");
                    return 1;
                }
            }
        }
    }
    if (memcmp(&g_a.st, &g_b.st, sizeof g_a.st) != 0) { fprintf(stderr, "synth stats mismatch\n"); return 1; }
    if (g_a.st.overflow == 0) { fprintf(stderr, "synth did not hit overflow\n"); return 1; }
    printf("synth OK: 2,000,000 updates, overflow %" PRIu64 " — 초과 경로 일치\n", g_a.st.overflow);
    return 0;
}

typedef void (*apply_fn)(book_set_t *, const deep_update_t *);

#define HIST_MAX 4096
static uint64_t g_hist[HIST_MAX + 1];

static uint32_t pct(uint64_t total, double p) {
    uint64_t want = (uint64_t)(p * (double)total), acc = 0;
    for (uint32_t c = 0; c <= HIST_MAX; ++c) {
        acc += g_hist[c];
        if (acc > want) return c;
    }
    return HIST_MAX;
}

static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int run(apply_fn fn, const char *name, int reps) {
    book_set_init(&g_a);
    double *tp = malloc((size_t)reps * sizeof *tp);

    /* 예열 1회 — 쓰는 심볼 페이지를 물리 페이지로 끌어오고 캐시를 채운다 */
    reset(&g_a);
    for (size_t i = 0; i < g_n; ++i) fn(&g_a, &g_ups[i]);

    for (int r = 0; r < reps; ++r) {
        reset(&g_a);
        uint64_t t0 = ll_rdtsc();
        for (size_t i = 0; i < g_n; ++i) fn(&g_a, &g_ups[i]);
        uint64_t t1 = ll_rdtsc();
        tp[r] = (double)(t1 - t0) / (double)g_n;
    }
    qsort(tp, (size_t)reps, sizeof *tp, cmp_dbl);

    uint64_t total = 0;
    for (int r = 0; r < reps; ++r) {
        reset(&g_a);
        for (size_t i = 0; i < g_n; ++i) {
            uint64_t t0 = ll_rdtsc();
            fn(&g_a, &g_ups[i]);
            uint64_t d = ll_rdtsc() - t0;
            g_hist[d > HIST_MAX ? HIST_MAX : d]++;
            total++;
        }
    }

    /* 한 줄 TSV — 여러 번 돌린 결과를 그대로 모아 정리할 수 있게 */
    printf("%s\tcyc/apply_med=%.2f\tmin=%.2f\tmax=%.2f\tpercall_p50=%u\tp99=%u\tp99.9=%u\n", name,
           tp[reps / 2], tp[0], tp[reps - 1], pct(total, 0.50), pct(total, 0.99), pct(total, 0.999));
    free(tp);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pcap> <check|fwd|rev> [reps]\n", argv[0]);
        return 2;
    }
    int reps = argc > 3 ? atoi(argv[3]) : 200;
    load(argv[1]);
    if (g_rx.dropped_full) { fprintf(stderr, "queue dropped %" PRIu64 "\n", g_rx.dropped_full); return 1; }

    if (strcmp(argv[2], "check") == 0) return check() || check_synth();
    if (strcmp(argv[2], "fwd") == 0) return run(book_apply, "fwd", reps);
    if (strcmp(argv[2], "rev") == 0) return run(book_rev_apply, "rev", reps);
    fprintf(stderr, "unknown mode %s\n", argv[2]);
    return 2;
}
