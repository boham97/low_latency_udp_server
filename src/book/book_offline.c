/*
 * book_offline.c — pcap → 프레이밍 → 디코드 → 오더북 → BBO (네트워크 없이)
 *
 * CLAUDE.md 진행순서 1단계. 여기서 확정하려는 건 **정확성뿐이다** — 파싱,
 * 심볼 매핑, 북 적용, BBO 발행 시점. 지연은 안 잰다.
 *
 * 스테이지를 안 나눈 이유: 디코드 일감이 필드 재배치 몇 개라 큐 핸드오프
 * 비용(test/bench/results.md에서 잰 그 값)보다 작을 수 있다. 나눠놓고 시작하면
 * "나누는 게 이득인가"를 영영 못 잰다. 정확성을 먼저 고정해두고, 분리는
 * before/after가 있는 변경으로 따로 한다.
 *
 * 그래도 rx_queue는 그대로 통과시킨다. 오프라인에서 확정한 것이 라이브 경로와
 * 같은 코드여야 하기 때문 — 프레이밍은 rx_framer_segment 한 벌을 공유하고,
 * 큐는 여기서 세그먼트 단위 배치 버퍼로 쓰인다 (같은 스레드에서 push 후 drain).
 *
 * Run: book_offline <pcap> [--bbo] [--symbol SYM] [--depth SYM]
 */
#include <ll/book.h>
#include <ll/deep_decode.h>
#include <ll/rx_stage.h>
#include <ll/tsc.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pcapng 블록 헤더를 읽는 건 핫패스가 아니다 — 명시적 시프트로 단순하게 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── 전역 상태: 전부 사전 할당(BSS). book_set_t는 19MB라 memset하지 않는다 ── */
static rx_queue_t g_queue;
static ll_symtab_t g_symtab;
static book_set_t g_books;
static rx_stats_t g_rx;
static deep_stats_t g_dec;
static rx_framer_t g_framer;

static int g_emit_bbo = 0;
static uint64_t g_filter_key = 0; /* 0 = 전 심볼 */
static uint64_t g_bbo_lines = 0;
static uint64_t g_epc = 0;

/* 고정소수점 → 사람이 읽는 십진. 출력 지점에서만 한다 (북은 정수로만 다룬다) */
static void fmt_px(int64_t px, char *out, size_t n) {
    int neg = px < 0;
    uint64_t a = neg ? (uint64_t)(-px) : (uint64_t)px;
    snprintf(out, n, "%s%" PRIu64 ".%04" PRIu64, neg ? "-" : "", a / 10000, a % 10000);
}

static void on_update(const deep_update_t *u) {
    book_apply(&g_books, u);

    /* 배치 발행: Event Processing Complete가 서면 그 심볼의 책이 일관 상태다.
       매 메시지마다 발행하면 이벤트 중간의 찢어진 책을 내보내게 된다 */
    if (!u->epc) {
        return;
    }
    g_epc++;

    if (!g_emit_bbo) {
        return;
    }
    if (g_filter_key && g_symtab.name[u->sym] != g_filter_key) {
        return;
    }

    bbo_t bbo;
    book_bbo(&g_books, u->sym, &bbo);

    char sym[9], bp[32], ap[32];
    ll_symtab_name(&g_symtab, u->sym, sym);
    fmt_px(bbo.bid_px, bp, sizeof bp);
    fmt_px(bbo.ask_px, ap, sizeof ap);

    printf("%" PRId64 "\t%s\t%s\t%u\t%s\t%u\n", u->seq, sym, bbo.has_bid ? bp : "-",
           bbo.bid_sz, bbo.has_ask ? ap : "-", bbo.ask_sz);
    g_bbo_lines++;
}

/* 큐에 쌓인 PLU를 전부 소화 — 라이브에서는 이 루프가 별도 스레드가 된다 */
static void drain(void) {
    for (;;) {
        rx_msg_t *m = rx_queue_pop_begin(&g_queue);
        if (!m) {
            return;
        }
        deep_update_t u;
        if (deep_decode(m, &g_symtab, &u, &g_dec)) {
            /* pop_end 전에 처리를 끝낸다 — 슬롯 참조가 이 스코프를 안 벗어나므로
               mempool도, 인덱스 전달도 필요 없다 (CLAUDE.md의 그 판단) */
            rx_queue_pop_end(&g_queue);
            on_update(&u);
        } else {
            rx_queue_pop_end(&g_queue);
        }
    }
}

/* ── Ethernet / IPv4 / UDP 껍질 벗기기 ── */
static void parse_frame(const uint8_t *f, uint32_t caplen) {
    if (caplen < 14) return;
    uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
    uint32_t off = 14;
    while (eth == 0x8100 && off + 4 <= caplen) { /* VLAN */
        eth = (uint16_t)((f[off + 2] << 8) | f[off + 3]);
        off += 4;
    }
    if (eth != 0x0800 || off + 20 > caplen) return;

    const uint8_t *ip = f + off;
    if ((ip[0] >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;
    if (ihl < 20 || off + ihl > caplen) return;
    if (ip[9] != 17) return; /* UDP */

    uint32_t uo = off + ihl;
    if (uo + 8 > caplen) return;
    const uint8_t *udp = f + uo;
    uint16_t ulen = (uint16_t)((udp[4] << 8) | udp[5]);
    if (ulen < 8) return;

    uint32_t po = uo + 8, pl = (uint32_t)(ulen - 8);
    if (po + pl > caplen) pl = caplen - po; /* 캡처가 잘렸으면 실제 바이트로 */

    rx_framer_segment(&g_framer, f + po, pl, ll_rdtsc(), &g_queue, &g_rx);
    drain();
}

int main(int argc, char **argv) {
    const char *path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bbo") == 0) {
            g_emit_bbo = 1;
        } else if (strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
            /* 심볼은 와이어에서 공백패딩 8바이트 — 같은 모양으로 만들어야 키가 맞는다 */
            char padded[8];
            memset(padded, ' ', sizeof padded);
            size_t len = strlen(argv[++i]);
            if (len > 8) len = 8;
            memcpy(padded, argv[i], len);
            g_filter_key = ll_sym_key(padded);
            g_emit_bbo = 1;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s <pcap> [--bbo] [--symbol SYM]\n", argv[0]);
        return 2;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror("open");
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsz);
    if (!buf || fread(buf, 1, (size_t)fsz, fp) != (size_t)fsz) {
        fprintf(stderr, "read failed\n");
        return 1;
    }
    fclose(fp);

    if (fsz < 4 || rd32(buf) != 0x0A0D0D0A) {
        fprintf(stderr, "not a little-endian pcapng file\n");
        return 1;
    }

    rx_queue_init(&g_queue);
    ll_symtab_init(&g_symtab);
    book_set_init(&g_books);
    rx_framer_init(&g_framer);

    uint64_t packets = 0;
    long off = 0;
    while (off + 12 <= fsz) {
        uint32_t btype = rd32(buf + off);
        uint32_t blen = rd32(buf + off + 4);
        if (blen < 12 || off + (long)blen > fsz) break;

        if (btype == 0x00000006) { /* Enhanced Packet Block */
            uint32_t caplen = rd32(buf + off + 20);
            if (off + 28 + (long)caplen <= fsz) {
                parse_frame(buf + off + 28, caplen);
                packets++;
            }
        } else if (btype == 0x00000003) { /* Simple Packet Block */
            uint32_t orig = rd32(buf + off + 8);
            uint32_t caplen = blen - 16;
            if (caplen > orig) caplen = orig;
            parse_frame(buf + off + 12, caplen);
            packets++;
        }
        off += blen;
    }
    free(buf);

    FILE *o = stderr; /* stdout은 BBO 전용 — TSV를 그대로 diff에 물릴 수 있게 */
    fprintf(o, "\n== 입력 ==\n");
    fprintf(o, "packets       : %" PRIu64 "\n", packets);
    fprintf(o, "DEEP segments : %" PRIu64 "  (heartbeat %" PRIu64 ")\n", g_rx.datagrams,
            g_rx.heartbeats);
    fprintf(o, "messages      : %" PRIu64 "\n", g_rx.messages);
    fprintf(o, "  PLU (queue) : %" PRIu64 "\n", g_rx.pushed);
    fprintf(o, "  필터 제외   : %" PRIu64 "\n", g_rx.filtered);
    fprintf(o, "  큐 가득 드롭: %" PRIu64 "  (0이어야 함 — 세그먼트당 PLU < cap)\n",
            g_rx.dropped_full);
    fprintf(o, "seq GAP       : %" PRIu64 "  (누락 추정 %" PRIu64 ")\n", g_rx.gaps, g_rx.gap_msgs);

    fprintf(o, "\n== 디코드 ==\n");
    fprintf(o, "decoded       : %" PRIu64 "\n", g_dec.decoded);
    fprintf(o, "심볼 테이블 초과: %" PRIu64 "\n", g_dec.sym_dropped);
    fprintf(o, "distinct symbol : %u\n", g_symtab.count);

    fprintf(o, "\n== 오더북 ==\n");
    fprintf(o, "applied       : %" PRIu64 "\n", g_books.st.applied);
    fprintf(o, "  insert      : %" PRIu64 "\n", g_books.st.inserted);
    fprintf(o, "  update      : %" PRIu64 "\n", g_books.st.updated);
    fprintf(o, "  delete      : %" PRIu64 "\n", g_books.st.deleted);
    fprintf(o, "  delete 대상 없음: %" PRIu64 "  (갭 지표 아님 — book.h 주석)\n",
            g_books.st.delete_missing);
    fprintf(o, "  레벨 초과   : %" PRIu64 "  (BOOK_MAX_LEVELS=%d)\n", g_books.st.overflow,
            BOOK_MAX_LEVELS);
    fprintf(o, "관측 최대 깊이: %u\n", g_books.st.max_depth);

    fprintf(o, "\n== BBO ==\n");
    fprintf(o, "EPC 발행 시점 : %" PRIu64 "\n", g_epc);
    fprintf(o, "출력 라인     : %" PRIu64 "\n", g_bbo_lines);

    return 0;
}
