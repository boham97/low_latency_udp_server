/*
 * rx_stage.c — UDP 수신 → IEX-TP 프레이밍 해체 → 필요한 메시지만 SPSC 큐로
 *
 *   recvfrom ─▶ [수신버퍼] ─▶ IEX-TP 헤더/seq ─▶ [2B len][body] 순회
 *                                                   │
 *                                    PLU(0x38/0x35)만 ─▶ push_begin/commit
 *                                    나머지          ─▶ 여기서 버림
 *
 * test/udp/udp_deep_server.c의 파싱을 스테이지로 승격한 것. 차이는 하나 —
 * 파싱 결과를 출력하는 대신 오더북에 필요한 메시지만 다음 스테이지로 넘긴다.
 *
 * 왜 여기서 거르나: 필터가 디코드 스테이지에 있으면 heartbeat와 체결/관리
 * 메시지까지 큐를 건너간다. 그 트래픽은 링 슬롯을 먹고, 캐시라인을 먹고,
 * 소비자가 타입 바이트를 보고 버리는 데까지 코어를 쓴다. 프레이밍을 이미
 * 벗겨 타입 바이트가 손에 있는 지점에서 거르는 게 가장 싸다.
 */
#include <ll/rx_stage.h>
#include <ll/tsc.h>

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int rx_open_socket(int port, int idle_timeout_sec) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    /* 버스트 드롭을 줄이려 수신 버퍼를 키운다. 여기서 넘치면 seq 갭으로 나타난다 */
    int rcvbuf = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);

    /* 리플레이가 끝나 수신이 멎으면 루프가 빠져나오도록 */
    struct timeval tv = {.tv_sec = idle_timeout_sec, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void rx_stage_run(int fd, rx_queue_t *q, rx_stats_t *st) {
    /* 데이터그램마다 재사용 — 핫패스에 할당 없음 */
    alignas(64) uint8_t buf[2048];

    int64_t expected_seq = -1;  /* -1 = 아직 첫 세그먼트 안 봄 */
    uint32_t pending_flags = 0; /* 갭 표시는 "갭 이후 처음 실린 메시지"에 붙인다 */

    for (;;) {
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* idle 타임아웃 = 리플레이 종료 */
            }
            break;
        }
        /* 커널에서 돌아온 직후 = 이 파이프라인이 데이터를 처음 만진 시점.
           이후 구간 지연은 전부 이 기준점과의 차이로 잰다 */
        uint64_t rx_tsc = ll_rdtsc();

        /* 헤더도 못 채운 데이터그램은 캐스팅 순간 버퍼 밖을 읽는다 */
        if ((size_t)n < sizeof(iextp_header_t)) {
            st->non_deep++;
            continue;
        }

        const iextp_header_t *h = (const iextp_header_t *)buf;
        /* IEX-TP는 DEEP/TOPS를 같은 전송 헤더로 실어 나른다 — 여기서 안 갈라내면 오파싱 */
        if (h->protocol_id != IEXTP_PROTO_DEEP) {
            st->non_deep++;
            continue;
        }
        st->datagrams++;

        uint16_t mc = h->message_count;
        if (mc == 0) {
            /* heartbeat는 seq를 전진시키지 않는다 — 큐에 실을 것도 없다 */
            st->heartbeats++;
            continue;
        }

        if (expected_seq >= 0 && h->first_seq != expected_seq) {
            if (h->first_seq > expected_seq) {
                st->gaps++;
                st->gap_msgs += (uint64_t)(h->first_seq - expected_seq);
                /* 북 상태가 이미 불완전하다 — 소비자가 알아야 복구 정책을 걸 수 있다 */
                pending_flags |= RX_FLAG_AFTER_GAP;
            } else {
                st->reorders++;
            }
        }
        expected_seq = h->first_seq + mc;

        /* 메시지 블록 순회: [2B len][body] 반복 */
        const uint8_t *p = buf + sizeof(iextp_header_t);
        const uint8_t *end = p + h->payload_len;
        /* payload_len은 와이어 값이라 신뢰 불가 — 실제 수신 바이트로 상한을 누른다 */
        if (end > buf + n) {
            end = buf + n;
        }

        /* 종료 조건이 둘인 이유: 개수는 헤더 주장값, 경계는 실제 도착 바이트 */
        for (uint16_t i = 0; i < mc && p + 2 <= end; ++i) {
            uint16_t mlen = *(const uint16_t *)p;
            p += 2;
            /* 바디가 잘렸다면 다음 블록 시작 위치도 알 수 없다 — 세그먼트 포기 */
            if (p + mlen > end) {
                st->truncated++;
                break;
            }
            st->messages++;

            uint8_t type = p[0];
            /* 타입만 보고 캐스팅하면 mlen이 30보다 짧은 손상 블록에서 블록 경계를
               넘어 읽는다. 길이 확인이 캐스팅의 전제 */
            if ((type != DEEP_PLU_BUY && type != DEEP_PLU_SELL) ||
                mlen < sizeof(deep_plu_t)) {
                st->filtered++;
                p += mlen;
                continue;
            }

            /* ── 여기부터가 큐에 실리는 유일한 경로 ── */
            rx_msg_t *slot = rx_queue_push_begin(q);
            if (!slot) {
                /* 백프레셔 지점. 지금 정책은 "최신을 버린다"(drop-newest) —
                   생산자를 멈추면 소켓 버퍼가 넘쳐 어차피 커널이 버리고,
                   그건 seq 갭으로만 보여 어디서 잃었는지 알 수 없게 된다.
                   여기서 버리면 최소한 잃은 개수를 정확히 안다 */
                st->dropped_full++;
                p += mlen;
                continue;
            }

            slot->rx_tsc = rx_tsc;
            slot->seq = h->first_seq + i;
            slot->rx_flags = pending_flags;
            memcpy(&slot->plu, p, sizeof(deep_plu_t));
            rx_queue_push_commit(q);
            /* 커밋 이후 slot을 만지지 않는다 — 이 시점부터 소비자 소유 */

            pending_flags = 0;
            st->pushed++;
            p += mlen;
        }
    }
}
