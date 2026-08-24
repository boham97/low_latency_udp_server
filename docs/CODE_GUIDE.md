# CODE_GUIDE — `include/`·`src/` 전체 설명 + 오더북 용어 정리

`include/ll/`와 `src/`에 있는 코드를 파일 단위로 설명한다. 설계 이유(WHY)는
CLAUDE.md에 이미 정리돼 있으니, 여기서는 **각 파일이 정확히 뭘 하고 어떻게
쓰이는지**와 **용어**에 집중한다.

## 0. 한 장 요약 — 파이프라인

```
pcap 파일                                UDP 소켓(라이브)
   │                                          │
   ▼                                          ▼
parse_frame()                          rx_stage_run()
(book_offline.c)                       (rx_stage.c)
   │  Ethernet/IP/UDP 껍질 벗김                │  recvfrom
   └──────────────┬───────────────────────────┘
                   ▼
         rx_framer_segment()   ← rx_stage.c, 오프라인/라이브 공유
         IEX-TP 헤더 파싱 → seq 갭 검사 → [len][body] 순회
         PLU(0x38/0x35)만 골라 rx_queue에 push
                   │  rx_msg_t (64B, 와이어 그대로)
                   ▼
              rx_queue (SPSC)
                   │
                   ▼
            deep_decode()        ← deep_decode.h
            packed→정렬, 심볼→인덱스, 타입→side
                   │  deep_update_t (64B, 정렬 완료)
                   ▼
            book_apply()         ← book.c
            정렬 배열 갱신/삽입/삭제
                   │
                   ▼ (Event Processing Complete일 때만)
            book_bbo()  →  BBO 출력
```

지금(2026-08-25) `book_offline`은 이 전체를 **한 스레드**가 순서대로 수행한다
(`rx_queue`는 스레드 분리용이 아니라 세그먼트 단위 배치 버퍼로만 쓰인다).
라이브 데모(`rx_stage`/`rx_main.c`)는 Rx와 드레인(현재는 개수만 세는 자리채움)이
**2스레드 + SPSC 큐**로 분리돼 있다.

---

## 1. `include/ll/` — 공개 헤더

### 1.1 `iex_deep_wire.h` — 와이어 포맷 그대로

IEX-TP/DEEP 프로토콜의 바이트 레이아웃을 `#pragma pack(1)` 구조체로 그대로
얹은 것. 수신 버퍼를 이 구조체 포인터로 캐스팅하면 바로 필드가 읽힌다
(파싱 코드를 따로 안 짜도 됨 — zero-copy struct mapping).

- `iextp_header_t` (40B): IEX-TP 세그먼트(=UDP 페이로드 하나) 맨 앞의 전송 헤더.
  세션 ID, 첫 메시지 시퀀스 번호(`first_seq`), 메시지 개수(`message_count`).
- `deep_common_t`: 모든 DEEP 메시지의 공통 앞부분(타입 판별용, 실사용은 안 함).
- `deep_plu_t` (30B): **Price Level Update** — 이 프로젝트가 다루는 유일한
  메시지 타입. `type`(0x38=매수/0x35=매도), `flags`(bit0=Event Processing
  Complete), `timestamp`, `symbol[8]`(공백패딩), `size`(0=레벨 삭제),
  `price`(고정소수점 ×10000).
- 두 `_Static_assert`가 구조체 크기를 스펙과 강제로 맞춘다 — 스펙 버전이
  바뀌면 빌드가 먼저 깨진다.

### 1.2 `rx_msg.h` — Rx→디코드 큐에 실리는 단위

`rx_msg_t` (정확히 64B = 캐시라인 1개): `rx_tsc`(수신 시각, 데이터그램당 1회),
`seq`, `rx_flags`(`RX_FLAG_AFTER_GAP`), `plu`(와이어 그대로 30B 복사).
데이터그램 전체가 아니라 **PLU 메시지 하나**가 슬롯 하나다 — Rx가 이미
필터링을 끝냈으므로 큐를 건너는 건 전부 북에 반영될 메시지.

파일 끝의 `LL_SPSC_CACHED_DEFINE(rx_queue, rx_msg_t, 1024)`가 이 타입 전용
SPSC 큐(`rx_queue_t`)와 그 함수들(`rx_queue_push_begin` 등)을 매크로로
찍어낸다.

### 1.3 `deep_msg.h` — 디코드 출력 단위

`deep_update_t` (64B): 북이 바로 쓸 수 있게 **정렬**된 값. `rx_msg_t`(와이어
packed, 30B PLU)와 다른 점 셋:
1. 비정렬 필드를 자연 정렬 위치로 복사 (북 적용 루프가 비정렬 로드를 안 함)
2. 심볼 8바이트 → `symtab`이 발급한 조밀한 `uint32` 인덱스
3. 타입 바이트(`0x38`/`0x35`) → `side`(`DEEP_SIDE_BID`/`ASK`), Event Flags →
   `epc` 비트 1개로 접음

가격은 고정소수점 정수(`×10000`) 그대로 둔다 — 북은 가격을 비교/탐색만 하지
연산하지 않으므로, double로 바꾸면 오히려 정확한 정수 비교가 부동소수점
비교가 되고 디코드마다 변환 비용이 붙는다.

### 1.4 `deep_decode.h` — `rx_msg_t` → `deep_update_t`

`deep_decode()` 인라인 함수 하나. 하는 일은 1.3에 적은 세 가지 변환 + 심볼
인턴(`ll_symtab_intern`). 심볼 테이블이 꽉 차 인덱스를 못 받으면 0을 반환하고
`out`은 건드리지 않는다(`sym_dropped` 카운터만 증가).

### 1.5 `symtab.h` / `symtab.c` — 심볼 8바이트 → 조밀 인덱스

`ll_sym_key()`가 공백패딩 ASCII 심볼 8바이트를 `uint64` 키 하나로 읽는다
(strcmp/길이계산 불필요). `ll_symtab_intern()`이 오픈 어드레싱 + 선형 탐사로
그 키에 인덱스를 배정 — 처음 보는 심볼이면 새 인덱스 발급, 이미 있으면 기존
인덱스 반환. 해시는 Fibonacci hashing(황금비 곱셈 후 상위 비트 사용) — ASCII
대문자라 하위 비트가 몰려 있어 단순 마스킹은 슬롯 군집을 만든다.

슬롯 배열은 16,384개(256KB)로 크지만, 실제로 밟히는 건 활성 심볼 수뿐이라
워킹셋은 작다. **인덱스는 등장 순서로 발급되므로 세션 밖으로 들고 나가면 안
된다** (파일이 바뀌면 같은 심볼도 다른 인덱스를 받음).

### 1.6 `book.h` / `book.c` — L2 오더북 + BBO

- `book_level_t` (16B): `{price, size}` 한 레벨.
- `book_t`: 심볼 하나의 북. 사이드(bid/ask)별로 `level[]` 정렬 배열 +
  `n[]`(현재 레벨 수). **양쪽 다 `[0]`이 best**가 되도록 bid는 내림차순,
  ask는 오름차순으로 유지한다 — bid에 부호를 뒤집어 두 사이드를 "키
  오름차순" 한 벌의 코드로 처리한다(`book_apply`의 `sign` 변수).
- `book_set_t`: 전 심볼의 `book_t` 배열(사전 할당, 19MB) + 통계
  (`book_stats_t`: applied/inserted/updated/deleted/overflow/max_depth 등).
  `book_set_init()`은 이 19MB를 memset하지 않는다 — BSS가 이미 0인 것을
  전제하고 `n[]`만 신뢰한다(실사용 39개 심볼 때문에 19MB 전부를 물리 페이지로
  끌어오지 않으려는 선택).
- `book_apply(bs, u)`: PLU 하나를 반영. 정렬 배열 선형 탐색으로 삽입점을
  찾고, `size==0`이면 삭제(`memmove`로 당김), 있으면 갱신, 없으면 삽입
  (`memmove`로 밀고 끼움). `BOOK_MAX_LEVELS`(64) 초과 시 best에서 가장 먼
  레벨을 버린다(`overflow`로만 카운트, BBO엔 영향 없음).
- `book_bbo(bs, sym, out)`: `level[side][0]` 두 개를 읽어 `bbo_t`
  (`bid_px/bid_sz/ask_px/ask_sz/has_bid/has_ask`)로 채운다. 배열 첫 원소만
  보면 되므로 O(1).

### 1.7 SPSC 큐 3종 — `spsc_queue_nopad.h` / `_padded.h` / `_cached.h`

같은 API(매크로로 `push`/`pop` 함수를 원하는 타입 전용으로 찍어냄)를 가진
링버퍼 세 가지 구현. 성능 비교용 대조군 3단:

| 파일 | 배치 | 매 push/pop마다 |
|---|---|---|
| `nopad` | head/tail/buf 인접(정렬 없음) | 상대측 캐시라인을 그대로 씀 → false sharing 최악 |
| `padded` | head/tail/buf 각각 `alignas(64)`로 분리 | false sharing은 없지만 상대측 인덱스를 매번 atomic load |
| `cached` | padded와 동일 배치 + 로컬 캐시(`cached_head`/`cached_tail`) | 평소엔 로컬 캐시만 보고, 가득/빔으로 보일 때만 상대측 라인을 확인 |

파이프라인은 **cached만** 실사용(`rx_msg.h`가 `LL_SPSC_CACHED_DEFINE`을 씀).
`cached.h`에만 있는 게 **슬롯 대여 API** — `push_begin`/`push_commit`/
`pop_begin`/`pop_end`. 값 복사(`buf[i]=*item`) 대신 슬롯 주소를 빌려주고
호출자가 직접 쓰게 해서, `push`/`pop`이 강제하는 임시 객체 복사를 없앤다.
**SPSC 전제라서만 성립**(생산자/소비자가 1개씩일 때만 begin~commit 사이의
"예약됐지만 미공개" 슬롯이 안전).

### 1.8 `mempool_freelist.h` / `_indexstack.h` / `_percore.h` — 메모리 풀 3종

**지금 파이프라인은 안 쓴다** (헤더만 있고 벤치 대상, 학습 산출물). 각 스테이지가
pop한 자리에서 처리를 끝내는 한 링 슬롯 자체가 곧 풀이라 별도 풀이 필요
없다는 게 현재 결론. 풀이 필요해지는 조건은 (a) 슬롯 참조를 다음 스테이지로
넘겨 `pop_end`를 미뤄야 할 때, (b) 반환이 FIFO 순서가 아닐 때, (c) 가변
길이라 슬롯 크기를 못 고정할 때.

- `freelist`: lock-free, 여러 스레드에서 CAS로 alloc/free (ABA 가능성 있음,
  produce-once/consume-once 전제).
- `indexstack`: 단일 스레드 전용, 배열 기반 LIFO 스택. atomic 없음.
- `percore`: 코어별로 완전히 독립된 `indexstack` — 코어 간 동기화 자체가 없지만,
  한 코어에서 alloc한 블록은 반드시 같은 코어에 free해야 함(마이그레이션 불가).

### 1.9 `rx_stage.h` — Rx 스테이지 API

- `rx_stats_t`: Rx가 뭘 얼마나 버렸는지 세는 카운터(datagrams/heartbeats/
  messages/filtered/pushed/dropped_full/truncated/gaps/gap_msgs/reorders).
- `rx_framer_t`: 세그먼트를 넘어가는 유일한 상태 — `expected_seq`(다음에
  기대하는 시퀀스 번호), `pending_flags`(갭 발생 시 다음 메시지에 붙일 플래그).
- `rx_framer_segment()`: IEX-TP 세그먼트 하나를 해체해 PLU만 큐에 push하는
  본체. **오프라인(pcap)과 라이브(recvfrom) 양쪽에서 공유**하는 한 벌 —
  오프라인에서 검증한 파싱/필터/갭 판정이 그대로 라이브에 적용되게 하려는
  구조.
- `rx_open_socket()` / `rx_stage_run()`: 라이브 전용. 소켓 열기(수신 버퍼
  16MB, idle 타임아웃)와 `recvfrom` 반복 루프.

### 1.10 `tsc.h` — RDTSC 읽기

`ll_rdtsc()` — `rdtsc` 인라인 어셈블리 한 줄. 직렬화 명령(`lfence`)을 일부러
안 넣는다 — 재정렬 오차(몇 사이클)보다 `lfence` 자체의 비용(수십 사이클)이
측정 대상(스테이지 핸드오프)에 비해 크기 때문. WSL2 환경이라 절대 ns 값은
안 믿고 **before/after 상대 비교**에만 쓴다.

---

## 2. `src/` — 구현부

### 2.1 `src/deep/symtab.c`

`symtab.h`의 3개 함수(`ll_symtab_init` / `_intern` / `_name`) 구현. 내용은
1.5절 참고. `deep_decode()`는 인라인 헤더라 별도 `.c`가 없다.

### 2.2 `src/book/book.c`

`book.h`의 `book_set_init` / `book_apply` / `book_bbo` 구현. 내용은 1.6절 참고.

### 2.3 `src/net/rx_stage.c`

`rx_stage.h`의 구현부. 핵심은 `rx_framer_segment()`:

1. IEX-TP 헤더(40B) 확인 + `protocol_id == 0x8004`(DEEP)인지 검사 — TOPS 등
   다른 프로토콜이 섞여 있으면 여기서 걸러야 오파싱을 안 함.
2. `message_count == 0`이면 heartbeat — seq 전진 없이 그냥 리턴.
3. `first_seq`가 `expected_seq`와 다르면 갭(누락, `gaps`/`gap_msgs` 증가) 또는
   역전(`reorders` 증가) 기록. 갭이면 다음 메시지에 `RX_FLAG_AFTER_GAP`을 예약.
4. `[2B length][body]` 블록을 `message_count`만큼(단, 실제 수신 바이트
   `end`를 넘지 않게) 순회. 타입이 PLU(0x38/0x35)가 아니거나 블록이 30B보다
   짧으면 필터(`filtered++`)하고 다음 블록으로.
5. PLU면 `rx_queue_push_begin()`으로 슬롯을 받아 그 자리에 직접
   `rx_tsc`/`seq`/`rx_flags`/`plu`(30B memcpy)를 채우고 `push_commit()`.
   큐가 가득 차 슬롯을 못 받으면 **drop-newest**(`dropped_full++`) — 생산자를
   멈추면 결국 커널 소켓 버퍼가 넘쳐 seq 갭으로만 보이는 손실이 나므로,
   여기서 명시적으로 세면서 버리는 쪽을 택함.

`rx_stage_run()`은 이 위에 `recvfrom` 무한루프를 얹은 것 — 데이터그램 하나
받을 때마다 `ll_rdtsc()`로 기준 시각을 찍고 `rx_framer_segment()` 호출. idle
타임아웃(EAGAIN)이 나면 리플레이가 끝난 것으로 보고 루프를 빠져나온다.

### 2.4 `src/net/rx_main.c`

`rx_stage` 실행 파일의 `main()`. **2스레드** 데모:

- 메인 스레드(코어 0 고정): `rx_stage_run()` 호출 — 위 2.3의 Rx 본체.
- `drain_thread`(코어 1 고정): `rx_queue_pop_begin/pop_end`로 큐를 비우며
  타입이 진짜 PLU인지, push→pop 사이 TSC 사이클(지연의 첫 기준선)을 재는
  **자리채움 소비자**(아직 디코드/북 없음 — 개수만 셈).

종료 시 push==pop 여부, PLU 아닌 게 섞였는지, p50/p99/p99.9 지연을 출력한다.

### 2.5 `src/book/book_offline.c`

**오프라인 전체 파이프라인** 한 파일 — pcap → 프레이밍 → 디코드 → 북 →
BBO를 **단일 스레드**로 수행. CLAUDE.md "진행순서 1단계"의 결과물이고,
정확성 검증(`test/book/verify_bbo.sh`)이 이 바이너리를 대상으로 한다.

- `parse_frame()`: pcapng 안에 든 프레임 하나에서 Ethernet(+VLAN)/IPv4/UDP
  헤더를 벗겨 UDP 페이로드(=IEX-TP 세그먼트)만 뽑고, `rx_framer_segment()`
  → `drain()` 순으로 호출.
- `drain()`: 그 세그먼트가 큐에 채운 PLU를 전부 소진. 메시지 하나마다
  `pop_begin` → `deep_decode()` → `pop_end`(슬롯 반환) → 성공 시
  `on_update()`. **슬롯 참조가 이 스코프를 벗어나지 않으므로** 별도 mempool도
  인덱스 전달도 필요 없다는 게 CLAUDE.md의 판단을 그대로 코드로 보여줌.
  스테이지별(decode/book_apply) TSC 구간도 여기서 찍어 `g_lat_decode[]`/
  `g_lat_book[]`에 쌓는다.
- `on_update()`: `book_apply()` 호출 후, **EPC(Event Processing Complete)
  비트가 선 메시지에서만** `book_bbo()`를 읽어 TSV 한 줄(`seq, symbol, bid_px,
  bid_sz, ask_px, ask_sz`)을 stdout에 출력. `--symbol SYM`이면 그 심볼만.
- `main()`: pcapng 파일을 통째로 읽어(mmap 대신 `fread`) Enhanced/Simple
  Packet Block을 순회하며 `parse_frame()` 호출, 끝나면 입력/디코드/오더북/
  BBO/레이턴시 통계를 stderr로 출력(stdout은 BBO TSV 전용이라 그대로
  diff 가능).

옵션: `--bbo`(BBO 출력 켬), `--symbol SYM`(그 심볼만, `--bbo` 자동 켜짐).
환경변수 `LL_PROFILE_REPS`로 같은 pcap을 N회 반복해 perf 표본을 늘릴 수
있음(정확성 검증은 항상 1회 경로).

---

## 3. 오더북 / 마켓데이터 용어 정리

### 3.1 오더북 기본 개념

| 용어 | 뜻 |
|---|---|
| **오더북 (Order Book)** | 한 종목의 매수/매도 주문을 가격별로 쌓아둔 장부. 이 프로젝트에서는 심볼당 `book_t` 하나. |
| **사이드 (Side)** | 매수(bid) / 매도(ask) 중 어느 쪽인지. `DEEP_SIDE_BID` / `DEEP_SIDE_ASK`. |
| **레벨 (Price Level)** | "이 가격에 총 몇 주가 걸려 있다"는 한 줄. `book_level_t{price, size}`. |
| **Bid** | 매수 호가. "이 가격에 사겠다." 값이 높을수록 유리(사려는 사람이 더 쳐준 가격). |
| **Ask (=Offer)** | 매도 호가. "이 가격에 팔겠다." 값이 낮을수록 유리. |
| **Best Bid / Best Ask** | 그 순간 가장 유리한(가장 비싼 bid / 가장 싼 ask) 레벨. 이 프로젝트에서는 항상 `level[side][0]`. |
| **BBO (Best Bid and Offer)** | 최우선 매수·매도 호가 한 쌍 — best bid의 (가격,수량) + best ask의 (가격,수량). 시세창에 뜨는 "현재가" 주변 숫자가 이것. `bbo_t`가 그 스냅샷. |
| **스프레드 (Spread)** | best ask − best bid. 좁을수록 유동성이 좋다는 신호(이 프로젝트에서 별도로 계산하진 않음). |
| **락 (Locked / Crossed Book)** | 정상이면 bid < ask인데, bid == ask(locked) 또는 bid > ask(crossed)가 되는 비정상 상태. `book_offline` 검증 로그의 "락 20건"이 여기 해당(전부 테스트 심볼 ZWZZT의 bid==ask). |
| **깊이 (Depth)** | 한 사이드에 쌓인 레벨 개수. `BOOK_MAX_LEVELS=64`가 상한, 실측 최대는 사이드당 14. |
| **틱 (Tick)** | 허용되는 최소 가격 단위. "price-tick 인덱싱"은 가격을 그대로 배열 첨자로 써서 O(1) 갱신하는 로드맵상의 다음 최적화(아직 미착수). |

### 3.2 피드/프로토콜 레벨

| 용어 | 뜻 |
|---|---|
| **L1 / L2 / L3 (피드 레벨)** | L1=최우선호가만, **L2=가격대별 집계 수량**(IEX DEEP이 이거), L3=주문 단위(주문 ID까지). L2라서 주문 ID를 추적할 필요가 없고, 이 프로젝트 오더북 로직이 단순해지는 근거. |
| **IEX** | Investors Exchange — 미국 주식거래소. 이 프로젝트가 쓰는 피드의 발행사. |
| **DEEP** | IEX의 L2 마켓데이터 피드 이름. 이 프로젝트가 파싱하는 대상. |
| **IEX-TP** | IEX Transport Protocol — DEEP/TOPS 등 여러 피드를 얹어 나르는 공통 전송 헤더(세션ID, 시퀀스, 메시지 개수). `iextp_header_t`. |
| **세그먼트 (Segment)** | IEX-TP 헤더 + 그 뒤 메시지 블록들 = UDP 페이로드 하나. `rx_framer_segment()`가 처리하는 단위. |
| **Price Level Update (PLU)** | DEEP의 핵심 메시지. "이 가격 레벨의 집계 수량이 N으로 바뀌었다"(N=0이면 그 레벨 삭제)를 통째로 알려줌. Buy=`0x38`, Sell=`0x35`. |
| **Event Flags / EPC (Event Processing Complete)** | PLU의 flags 필드 bit0. 이 비트가 설 때까지는 여러 PLU가 배치로 도착 중이라 책이 "찢어진" 중간 상태일 수 있다. **BBO는 EPC가 선 시점에만 발행** — 그래야 일관된 스냅샷을 내보낸다. |
| **시퀀스 번호 (Sequence Number)** | IEX-TP 헤더의 `first_seq`. 세그먼트 안 메시지들이 순서대로 이 값부터 번호를 받는다. 다음 세그먼트의 `first_seq`가 기대값과 다르면 **갭(gap)**(중간에 유실) 또는 **역전(reorder)**. |
| **하트비트 (Heartbeat)** | `message_count == 0`인 세그먼트. 데이터가 없어도 "연결 살아있다"를 알리는 빈 세그먼트. |
| **백프레셔 (Backpressure)** | 소비자가 생산자를 못 따라가 큐가 가득 찬 상태. 이 프로젝트 정책은 **drop-newest**(새로 들어오는 것을 버림) — CLAUDE.md·rx_stage.c 주석 참고. |
| **고정소수점 (Fixed-point) ×10000** | DEEP의 가격 필드는 실수를 정수로 인코딩한 것 — 정수값을 10000으로 나누면 실제 가격(예: `1234500` → `123.4500`). 북 내부는 이 정수 그대로 다루고, 사람이 읽을 십진 문자열 변환은 출력 시점(`fmt_px`)에서만 한다. |

### 3.3 북 적용 로직에서 쓰는 표현

| 용어 | 뜻 |
|---|---|
| **삽입 (Insert)** | 처음 보는 가격에 size>0 PLU가 와서 새 레벨이 생기는 것. `book_stats_t.inserted`. |
| **갱신 (Update)** | 이미 있는 가격 레벨의 수량이 바뀌는 것. `.updated`. |
| **삭제 (Delete)** | size==0 PLU로 레벨이 없어지는 것. `.deleted`. |
| **delete_missing** | size==0인데 그 가격 레벨이 애초에 북에 없던 경우. **갭 지표가 아니다** — 실측상 시퀀스 갭이 0인 구간에서도 나타나며, 피드 자체가 존재하지 않는 레벨에 삭제를 보내는 경우로 확인됨(book.h 주석 / CLAUDE.md 참고). |
| **오버플로 (Overflow)** | 그 사이드 레벨 수가 `BOOK_MAX_LEVELS`(64)를 넘어서려 할 때, best에서 가장 먼 레벨을 버리는 것. BBO(=best)에는 영향이 없다. |
| **AFTER_GAP** | 시퀀스 갭 직후 첫 메시지에 붙는 플래그(`RX_FLAG_AFTER_GAP`). 이 메시지가 반영된 시점부터는 북 상태가 유실분만큼 불완전할 수 있음을 소비자에게 알림. |

### 3.4 이 프로젝트의 파이프라인 용어

| 용어 | 뜻 |
|---|---|
| **Rx 스테이지** | UDP 수신 + IEX-TP 프레이밍 해체 + PLU 필터까지 담당하는 첫 스테이지(`rx_stage.c`). |
| **디코드 스테이지** | `rx_msg_t`(와이어 packed) → `deep_update_t`(정렬 완료)로 바꾸는 단계(`deep_decode.h`). 지금은 별도 스레드가 아니라 북 적용과 한 스레드에서 순차 실행. |
| **SPSC** | Single-Producer Single-Consumer — 생산자 1개, 소비자 1개 전제의 락프리 큐. 이 프로젝트의 모든 큐가 이 전제. |
| **슬롯 대여 (Slot Borrowing)** | 큐가 원소를 값으로 복사받는 대신, 빈 슬롯의 주소를 내주고 호출자가 직접 채우게 하는 API(`push_begin`/`push_commit`/`pop_begin`/`pop_end`). |
| **드레인 (Drain)** | 큐에 쌓인 원소를 빌 때까지 다 소비하는 것. `book_offline.c`의 `drain()`, `rx_main.c`의 `drain_thread`. |
