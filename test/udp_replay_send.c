/*
 * udp_replay_send.c — pcap 안의 IEX-TP 세그먼트(패킷 바디)만 UDP로 재생
 *
 *   pcap frame ─(eth/ip/udp 헤더 제거)─▶ IEX-TP 세그먼트 ─sendto()─▶ UDP 서버
 *
 * 원본 캡처의 이더넷/IP/UDP 껍데기는 버리고, UDP 페이로드(= IEX-TP 세그먼트)만
 * 새 UDP 데이터그램으로 서버에 쏜다. 서버는 이 바디를 구조체에 그대로 매핑한다.
 *
 * Build: gcc -O2 -Wall -o udp_replay_send udp_replay_send.c -lpcap
 * Run:   ./udp_replay_send <pcap> [host] [port] [delay_us]
 *        기본값: host=127.0.0.1 port=9004 delay_us=0(최대속도)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pcap.h>

/* pcap 프레임에서 UDP 페이로드(off, len)를 뽑는다. 실패 시 len=0. */
static const uint8_t *udp_payload(const uint8_t *f, uint32_t caplen, uint32_t *out_len) {
    *out_len = 0;
    if (caplen < 14) return NULL;
    uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
    uint32_t off = 14;
    while (eth == 0x8100 && off + 4 <= caplen) {       /* VLAN 스킵 */
        eth = (uint16_t)((f[off + 2] << 8) | f[off + 3]);
        off += 4;
    }
    if (eth != 0x0800 || off + 20 > caplen) return NULL;
    const uint8_t *ip = f + off;
    if ((ip[0] >> 4) != 4) return NULL;
    uint32_t ihl = (ip[0] & 0x0f) * 4;
    if (ihl < 20 || off + ihl > caplen || ip[9] != 17) return NULL;
    uint32_t uo = off + ihl;
    if (uo + 8 > caplen) return NULL;
    const uint8_t *udp = f + uo;
    uint16_t ulen = (uint16_t)((udp[4] << 8) | udp[5]);
    if (ulen < 8) return NULL;
    uint32_t po = uo + 8, pl = ulen - 8;
    if (po + pl > caplen) pl = caplen - po;            /* 캡처 잘림 방어 */
    *out_len = pl;
    return f + po;
}

int main(int argc, char **argv) {
    const char *path  = argc > 1 ? argv[1] : "20180127_IEXTP1_DEEP1.0.pcap";
    const char *host  = argc > 2 ? argv[2] : "127.0.0.1";
    int         port  = argc > 3 ? atoi(argv[3]) : 9004;
    useconds_t  delay = argc > 4 ? (useconds_t)atoi(argv[4]) : 0;

    char eb[PCAP_ERRBUF_SIZE];
    pcap_t *pc = pcap_open_offline(path, eb);  /*pcap 파일, 에러 메세지 */
    if (!pc) { fprintf(stderr, "pcap open: %s\n", eb); return 1; }

    int fd = socket(AF_INET, SOCK_DGRAM, 0); //sock_dgram -> udp, af_inet -> ipv4
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad host: %s\n", host); return 1;
    }

    uint64_t sent = 0, skipped = 0, bytes = 0;
    struct pcap_pkthdr *h;
    const u_char *d;
    int rc;
    while ((rc = pcap_next_ex(pc, &h, &d)) == 1) {
        uint32_t plen;
        const uint8_t *pl = udp_payload((const uint8_t *)d, h->caplen, &plen);
        if (!pl || plen == 0) { skipped++; continue; }
        ssize_t n = sendto(fd, pl, plen, 0, (struct sockaddr *)&dst, sizeof dst);
        if (n < 0) { perror("sendto"); break; }
        sent++; bytes += (uint64_t)n;
        if (delay) usleep(delay);
    }
    close(fd);
    pcap_close(pc);

    printf("replay 완료 -> %s:%d\n", host, port);
    printf("  세그먼트 전송 : %" PRIu64 "\n", sent);
    printf("  스킵(비DEEP)  : %" PRIu64 "\n", skipped);
    printf("  전송 바이트   : %" PRIu64 "\n", bytes);
    return 0;
}
