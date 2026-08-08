/*
 * udp_deep_server.c — UDP 수신 → 버퍼를 구조체에 그대로 매핑 (Rx 스테이지)
 *
 *   recvfrom() ─▶ [수신버퍼] ─(캐스팅)─▶ iextp_header_t* / deep_plu_t*
 *
 * byte-shift 파싱 없이 packed 구조체 포인터로 캐스팅해 필드를 바로 읽는다
 * (iex_deep_wire.h 참고: LE 호스트 + pack(1) 전제).
 *
 * 실제 UDP 경로로 넘어왔으므로 여기서 처음으로 "진짜" 시퀀스 갭이 생길 수 있다
 * (소켓 버퍼 오버플로로 인한 드롭, 순서역전). 그래서 Rx 단계에서 seq 연속성을
 * 검사한다 — CLAUDE.md의 "시퀀스 갭 감지 → 드롭/백프레셔 정책"의 실제 근거.
 *
 * Build: gcc -O2 -Wall -I../../include -o udp_deep_server udp_deep_server.c
 * Run:   ./udp_deep_server [port]      (기본 9004)
 *        데이터가 2초간 안 오면 요약 출력 후 종료.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <stdalign.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <ll/iex_deep_wire.h>

static void fmt_sym(const char *p, char *out) {
    int len = 8;
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == 0)) len--;
    memcpy(out, p, len);
    out[len] = 0;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 9004;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    /* 버스트 드롭을 줄이려 수신 버퍼를 키운다 (핫패스 밖, 시작 시 1회) */
    int rcvbuf = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

    /* 리플레이가 끝나 데이터가 멎으면 빠져나오도록 수신 타임아웃 2초 */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);/*모든 주소 허용*/
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }

    printf("UDP DEEP 서버 listen :%d  (2초 idle 시 종료)\n\n", port);
    fflush(stdout);   /* 파이프/파일 리다이렉트 시 배너가 버퍼에 갇히지 않게 */

    /* 
    캐시라인 정렬 수신 버퍼 (핫패스, 재사용 — 데이터그램마다 malloc 안 함) 
    uint8_t buf[2048];)로 둬도 성능은 사실상 동일
    스택 2KB를 안 쓰는 것과, 나중에 이 로직이 함수로 분리되거나 버퍼가 커졌을 때 안전

    체크해보기
    1. 구조체가 캐시라인을 걸치는 것 방지 (약간 유효)

    buf가 정렬 안 돼서 예를 들어 0x...30에서 시작하면, 40바이트 IEX-TP 헤더가 라인 두 개에 걸칩니다. 그럼 헤더 읽는 데 캐시라인 fetch가 2번. 64
    정렬이면 헤더 하나가 딱 라인 하나에 들어갑니다(40 ≤ 64).

    다만 뒤이어 메시지 블록들을 어차피 순회하니 그 라인들은 어차피 읽습니다. 진짜 이득은 첫 접근 시점뿐이라 실측하면 노이즈에 묻힐 가능성이 큽니다.

    2. 커널의 copy_to_user 복사 경로 (약간 유효)

    recvfrom이 커널 버퍼 → buf로 복사할 때, 목적지가 정렬돼 있으면 rep movsb/AVX 경로가 head 처리 없이 바로 탑니다. 역시 2KB 복사에서 몇 ns 수준.
    */
    //alignas(64) static uint8_t buf[2048];
    uint8_t buf[2048];

    uint64_t datagrams = 0, heartbeats = 0, messages = 0, plu_cnt = 0;
    uint64_t gaps = 0, gap_msgs = 0, reorders = 0;
    int64_t  expected_seq = -1;   /* -1 = 아직 첫 세그먼트 안 봄 */
    int      shown = 0;

    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL); //송신자 주소 NULL 
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  /* idle 타임아웃 */
            perror("recvfrom");
            break;
        }
        /* 헤더도 못 채운 데이터그램은 캐스팅 순간 버퍼 밖을 읽게 되므로 먼저 쳐낸다.
           같은 포트로 들어온 무관한 패킷일 수 있어 에러가 아니라 무시로 처리 */
        if ((size_t)n < sizeof(iextp_header_t)) continue;

        /* ── 여기서 zero-copy 매핑: 수신 버퍼를 헤더 구조체로 그대로 캐스팅 ── */
        const iextp_header_t *h = (const iextp_header_t *)buf;
        /* IEX-TP는 DEEP/TOPS를 같은 전송 헤더로 실어 나른다. 뒤따르는 메시지 바디
           해석이 프로토콜마다 다르므로 여기서 갈라내지 않으면 오파싱 */
        if (h->protocol_id != IEXTP_PROTO_DEEP) continue;
        datagrams++;

        uint16_t mc = h->message_count;

        /* seq 연속성 검사 (heartbeat는 seq를 전진시키지 않음) */
        if (mc > 0) {
            if (expected_seq >= 0 && h->first_seq != expected_seq) {
                if (h->first_seq > expected_seq) {
                    gaps++; gap_msgs += (uint64_t)(h->first_seq - expected_seq);
                    if (gaps <= 10)
                        printf("[GAP ] expected=%" PRId64 " got=%" PRId64
                               " (누락 %" PRId64 ")\n",
                               expected_seq, h->first_seq, h->first_seq - expected_seq);
                } else {
                    reorders++;
                    if (reorders <= 10)
                        printf("[REORDER] expected=%" PRId64 " got=%" PRId64 "\n",
                               expected_seq, h->first_seq);
                }
            }
            expected_seq = h->first_seq + mc;
        } else {
            heartbeats++;
            continue;
        }

        /* 메시지 블록 순회: [2B len][body] 반복 */
        const uint8_t *q   = buf + sizeof(iextp_header_t);
        const uint8_t *end = q + h->payload_len;
        /* payload_len은 와이어에서 온 값이라 신뢰할 수 없다. 실제 수신 바이트 수로
           상한을 눌러야 잘린 세그먼트에서 버퍼 밖을 읽지 않는다 */
        if (end > buf + n) end = buf + n;

        /* 종료 조건이 둘인 이유: 개수는 헤더가 주장하는 값, 경계는 실제 도착한
           바이트 — 둘 중 먼저 소진되는 쪽을 따라야 안전하다 */
        for (uint16_t i = 0; i < mc && q + 2 <= end; ++i) {
            uint16_t mlen = *(const uint16_t *)q;       /* 길이 프리픽스 (LE 매핑) */
            q += 2;
            /* 바디가 잘렸다면 이후 블록의 시작 위치도 알 수 없다 — 세그먼트 포기 */
            if (q + mlen > end) break;
            messages++;

            uint8_t type = q[0];
            /* 타입만 보고 캐스팅하면 mlen이 30보다 짧은 손상 블록에서 구조체가
               블록 경계를 넘어 읽는다. 길이를 먼저 확인해야 캐스팅이 성립 */
            if ((type == DEEP_PLU_BUY || type == DEEP_PLU_SELL) &&
                mlen >= sizeof(deep_plu_t)) {
                /* ── PLU 메시지를 구조체로 그대로 매핑 ── */
                const deep_plu_t *p = (const deep_plu_t *)q;
                plu_cnt++;
                if (shown < 15) {
                    char sym[16]; fmt_sym(p->symbol, sym);
                    printf("  PLU %-4s %-8s size=%-8u price=%" PRId64
                           ".%04" PRId64 "  %s\n",
                           type == DEEP_PLU_BUY ? "BUY" : "SELL", sym, p->size,
                           p->price / 10000, (p->price % 10000 < 0 ? -p->price : p->price) % 10000,
                           DEEP_EVENT_COMPLETE(p->flags) ? "[complete]" : "");
                    shown++;
                }
            }
            q += mlen;
        }
    }

    close(fd);
    printf("\n== Rx 요약 ==\n");
    printf("datagrams   : %" PRIu64 "\n", datagrams);
    printf("heartbeats  : %" PRIu64 "\n", heartbeats);
    printf("messages    : %" PRIu64 "\n", messages);
    printf("  PLU       : %" PRIu64 "\n", plu_cnt);
    printf("seq GAP     : %" PRIu64 "  (누락 추정 %" PRIu64 " msg) <- UDP 드롭\n", gaps, gap_msgs);
    printf("reorder     : %" PRIu64 "\n", reorders);
    if (gaps == 0 && reorders == 0)
        printf("=> 무손실 수신. 원본 pcap과 동일한 seq 연속성 확보.\n");
    else
        printf("=> 손실/역전 발생. 드롭 정책·수신버퍼·페이싱 검토 지점.\n");
    return 0;
}
