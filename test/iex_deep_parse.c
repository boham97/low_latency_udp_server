/*
 * iex_deep_parse.c
 *
 * Self-contained parser for an IEX DEEP 1.0 capture in pcapng format.
 * No external libraries (no libpcap) — parses every layer by hand:
 *
 *   pcapng block  ->  Ethernet  ->  IPv4  ->  UDP  ->  IEX-TP segment  ->  DEEP messages
 *
 * Build:  gcc -O2 -Wall -o iex_deep_parse iex_deep_parse.c
 * Run:    ./iex_deep_parse 20180127_IEXTP1_DEEP1.0.pcap
 *
 * The capture file and the wire format are little-endian; we read every
 * multi-byte field with explicit byte shifts so the code is independent of
 * the host's alignment/endianness.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

/* ---- little-endian readers over a raw byte buffer ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
static int64_t  rd64s(const uint8_t *p) { return (int64_t)rd64(p); }

/* ---- formatting helpers ---- */

/* IEX prices: signed 8-byte int, 4 implied decimals (123400 => $12.34) */
static void fmt_price(int64_t px, char *out, size_t n) {
    int neg = px < 0;
    uint64_t a = neg ? (uint64_t)(-px) : (uint64_t)px;
    snprintf(out, n, "%s%" PRIu64 ".%04" PRIu64, neg ? "-" : "",
             a / 10000, a % 10000);
}

/* IEX timestamps: ns since POSIX epoch UTC */
static void fmt_ts(int64_t ns, char *out, size_t n) {
    time_t s = (time_t)(ns / 1000000000LL);
    long frac = (long)(ns % 1000000000LL);
    struct tm tm;
    gmtime_r(&s, &tm);
    char base[24];
    strftime(base, sizeof base, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(out, n, "%.21s.%09ld", base, frac);
}

/* 8-byte space-padded symbol -> trimmed C string */
static void fmt_sym(const uint8_t *p, char *out) {
    int len = 8;
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == 0)) len--;
    memcpy(out, p, len);
    out[len] = 0;
}

/* ---- per-message-type counters ---- */
typedef struct { uint8_t type; const char *name; uint64_t count; } TypeStat;
static TypeStat stats[] = {
    {'S', "System Event",            0},
    {'D', "Security Directory",      0},
    {'H', "Trading Status",          0},
    {'O', "Operational Halt",        0},
    {'P', "Short Sale Price Test",   0},
    {'I', "Retail Liquidity Ind.",   0},
    {'E', "Security Event",          0},
    {'8', "Price Level Update Buy",  0},
    {'5', "Price Level Update Sell", 0},
    {'T', "Trade Report",            0},
    {'X', "Official Price",          0},
    {'B', "Trade Break",             0},
    {'A', "Auction Information",     0},
    {0,   "(unknown)",               0},
};
static void bump(uint8_t t) {
    for (size_t i = 0; ; ++i) {
        if (stats[i].type == t || stats[i].type == 0) { stats[i].count++; return; }
    }
}

/* limit how many of each kind we print in detail */
static int show_trade = 8, show_plu = 8, show_admin = 12, show_auction = 4;

/* ---- decode one DEEP message (len bytes starting at m) ---- */
static void decode_deep(const uint8_t *m, uint16_t len) {
    if (len < 1) return;
    uint8_t t = m[0];
    bump(t);

    char sym[16], pxs[32], pxs2[32], ts[40];

    switch (t) {
    case '8': case '5': { /* Price Level Update */
        if (len < 30 || show_plu <= 0) break;
        uint8_t flags = m[1];
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        uint32_t size = rd32(m + 18);
        fmt_price(rd64s(m + 22), pxs, sizeof pxs);
        printf("  PLU %-4s %-8s flags=%u  %10u @ %-12s  %s\n",
               t == '8' ? "BUY" : "SELL", sym, flags, size, pxs, ts);
        show_plu--;
        break;
    }
    case 'T': case 'B': { /* Trade Report / Trade Break */
        if (len < 38 || show_trade <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        uint32_t size = rd32(m + 18);
        fmt_price(rd64s(m + 22), pxs, sizeof pxs);
        uint64_t tid = rd64(m + 30);
        printf("  %-11s %-8s %10u @ %-12s  id=%" PRIu64 "  %s\n",
               t == 'T' ? "TRADE" : "TRADE-BREAK", sym, size, pxs, tid, ts);
        show_trade--;
        break;
    }
    case 'X': { /* Official Price */
        if (len < 26 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        fmt_price(rd64s(m + 18), pxs, sizeof pxs);
        printf("  OFFICIAL   %-8s type=%c price=%-12s  %s\n",
               sym, m[1], pxs, ts);
        show_admin--;
        break;
    }
    case 'S': { /* System Event */
        if (len < 10 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        printf("  SYS-EVENT  '%c'  %s\n", m[1], ts);
        show_admin--;
        break;
    }
    case 'D': { /* Security Directory */
        if (len < 31 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        uint32_t lot = rd32(m + 18);
        fmt_price(rd64s(m + 22), pxs, sizeof pxs);
        printf("  SEC-DIR    %-8s flags=0x%02x lot=%u poc=%-10s luld=%u  %s\n",
               sym, m[1], lot, pxs, m[30], ts);
        show_admin--;
        break;
    }
    case 'H': { /* Trading Status */
        if (len < 22 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        char reason[5]; memcpy(reason, m + 18, 4); reason[4] = 0;
        printf("  TRADE-STAT %-8s status=%c reason=%-4s  %s\n",
               sym, m[1], reason, ts);
        show_admin--;
        break;
    }
    case 'O': { /* Operational Halt */
        if (len < 18 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        printf("  OPS-HALT   %-8s status=%c  %s\n", sym, m[1], ts);
        show_admin--;
        break;
    }
    case 'P': { /* Short Sale Price Test */
        if (len < 19 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        printf("  SSR        %-8s test=%u detail=%c  %s\n",
               sym, m[1], m[18] ? m[18] : ' ', ts);
        show_admin--;
        break;
    }
    case 'I': { /* Retail Liquidity Indicator */
        if (len < 18 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        printf("  RETAIL-LQ  %-8s ind=%c  %s\n", sym, m[1], ts);
        show_admin--;
        break;
    }
    case 'E': { /* Security Event */
        if (len < 18 || show_admin <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        printf("  SEC-EVENT  %-8s '%c'  %s\n", sym, m[1], ts);
        show_admin--;
        break;
    }
    case 'A': { /* Auction Information */
        if (len < 80 || show_auction <= 0) break;
        fmt_ts(rd64s(m + 2), ts, sizeof ts);
        fmt_sym(m + 10, sym);
        uint32_t paired = rd32(m + 18);
        fmt_price(rd64s(m + 22), pxs, sizeof pxs);   /* reference price */
        fmt_price(rd64s(m + 30), pxs2, sizeof pxs2); /* indicative clearing */
        uint32_t imb = rd32(m + 38);
        printf("  AUCTION    %-8s type=%c paired=%u ref=%s indic=%s imb=%u side=%c  %s\n",
               sym, m[1], paired, pxs, pxs2, imb, m[42], ts);
        show_auction--;
        break;
    }
    default:
        break;
    }
}

/* ---- parse one IEX-TP segment (UDP payload) ---- */
static uint64_t g_segments = 0, g_messages = 0, g_heartbeats = 0;

static void parse_iextp(const uint8_t *p, uint32_t len) {
    if (len < 40) return;                 /* need full header */
    uint16_t proto = rd16(p + 2);
    if (proto != 0x8004) return;          /* not DEEP */
    g_segments++;

    uint16_t payload_len = rd16(p + 12);
    uint16_t msg_count   = rd16(p + 14);
    if (msg_count == 0) { g_heartbeats++; return; }   /* heartbeat / gap-fill test */

    const uint8_t *q   = p + 40;          /* first Message Block */
    const uint8_t *end = p + 40 + payload_len;
    if (end > p + len) end = p + len;     /* clamp to captured bytes */

    for (uint16_t i = 0; i < msg_count && q + 2 <= end; ++i) {
        uint16_t mlen = rd16(q);
        q += 2;
        if (q + mlen > end) break;        /* truncated capture */
        decode_deep(q, mlen);
        g_messages++;
        q += mlen;
    }
}

/* ---- Ethernet/IPv4/UDP for one captured frame ---- */
static void parse_frame(const uint8_t *f, uint32_t caplen) {
    if (caplen < 14) return;
    uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
    uint32_t off = 14;
    while (eth == 0x8100 && off + 4 <= caplen) {      /* skip VLAN tags */
        eth = (uint16_t)((f[off + 2] << 8) | f[off + 3]);
        off += 4;
    }
    if (eth != 0x0800) return;                        /* IPv4 only */
    if (off + 20 > caplen) return;

    const uint8_t *ip = f + off;
    if ((ip[0] >> 4) != 4) return;
    uint32_t ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || off + ihl > caplen) return;
    if (ip[9] != 17) return;                          /* UDP */

    uint32_t udp_off = off + ihl;
    if (udp_off + 8 > caplen) return;
    const uint8_t *udp = f + udp_off;
    uint16_t ulen = (uint16_t)((udp[4] << 8) | udp[5]);
    if (ulen < 8) return;

    uint32_t pl_off = udp_off + 8;
    uint32_t pl_len = ulen - 8;
    if (pl_off + pl_len > caplen) pl_len = caplen - pl_off;  /* clamp */
    parse_iextp(f + pl_off, pl_len);
}

/* ---- pcapng walk ---- */
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "20180127_IEXTP1_DEEP1.0.pcap";
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("open"); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(fsz);
    if (!buf || fread(buf, 1, fsz, fp) != (size_t)fsz) {
        fprintf(stderr, "read failed\n"); return 1;
    }
    fclose(fp);

    if (fsz < 4 || rd32(buf) != 0x0A0D0D0A) {
        fprintf(stderr, "not a little-endian pcapng file\n");
        return 1;
    }

    printf("File: %s (%ld bytes)\n", path, fsz);
    printf("Format: pcapng, parsing Ethernet -> IPv4 -> UDP -> IEX-TP(0x8004) -> DEEP\n\n");
    printf("== sample decoded messages ==\n");

    uint64_t packets = 0;
    long off = 0;
    while (off + 12 <= fsz) {
        uint32_t btype = rd32(buf + off);
        uint32_t blen  = rd32(buf + off + 4);
        if (blen < 12 || off + (long)blen > fsz) break;   /* corrupt / EOF */

        if (btype == 0x00000006) {            /* Enhanced Packet Block */
            uint32_t caplen = rd32(buf + off + 20);
            const uint8_t *frame = buf + off + 28;
            if (off + 28 + (long)caplen <= fsz) {
                parse_frame(frame, caplen);
                packets++;
            }
        } else if (btype == 0x00000003) {     /* Simple Packet Block */
            uint32_t orig = rd32(buf + off + 8);
            const uint8_t *frame = buf + off + 12;
            uint32_t caplen = blen - 16;
            if (caplen > orig) caplen = orig;
            parse_frame(frame, caplen);
            packets++;
        }
        /* SHB(0x0A0D0D0A), IDB(1), and others: skip via block length */
        off += blen;
    }

    printf("\n== summary ==\n");
    printf("packets (EPB)        : %" PRIu64 "\n", packets);
    printf("IEX-TP DEEP segments : %" PRIu64 "\n", g_segments);
    printf("  of which heartbeats: %" PRIu64 "\n", g_heartbeats);
    printf("DEEP messages decoded: %" PRIu64 "\n\n", g_messages);

    printf("== message type counts ==\n");
    uint64_t tot = 0;
    for (size_t i = 0; ; ++i) {
        if (stats[i].count)
            printf("  %c  %-26s %12" PRIu64 "\n",
                   stats[i].type ? stats[i].type : '?', stats[i].name, stats[i].count);
        tot += stats[i].count;
        if (stats[i].type == 0) break;
    }
    printf("  %-29s %12" PRIu64 "\n", "TOTAL", tot);

    free(buf);
    return 0;
}
