# Low Latency Market Data Study — IEX DEEP 오더북 빌더

## 프로젝트 목적

IEX DEEP 피드를 받아 **저지연 오더북을 재구성**하는 마켓 데이터 피드 핸들러 구현.

저지연 시장 데이터 처리 구조(UDP 수신 → 디코드 → 오더북 적용 → BBO 발행)를
직접 만들며 SPSC 파이프라인 / 메모리 풀 / 지연 측정 기법을 실습한다.

목표: 구간별 tick-to-update 지연을 측정하고 p99/p99.9 꼬리 지연을 줄이는
원리 이해 (절대 수치가 아니라 before/after 상대 개선으로 검증).

## 프로젝트: IEX DEEP 오더북 빌더

### 왜 DEEP인가

DEEP는 **price-aggregated (L2)** 피드 — 메시지 하나가 "이 가격 레벨의 집계
수량은 N"을 통째로 알려준다. ITCH(L3)처럼 order-by-order로 주문 ID를 해시맵에
추적할 필요가 없어 오더북 로직이 단순하다:

- `Price Level Update`: size > 0 → 레벨 갱신/삽입, size == 0 → 레벨 삭제
- order ID 추적 없음 → 주문 노드 풀 불필요. 오더북은 심볼당 **고정 크기
  구조체로 사전 할당**, 메모리 풀은 SPSC로 흐르는 in-flight 메시지 블록에만 사용.

### 파싱 계층

pcap → 이더넷/IP/UDP 제거 → **IEX-TP 헤더** → **DEEP 메시지**

- **IEX-TP**: Session ID, First Message Sequence Number, Message Count
  → 시퀀스 갭/드롭 감지 (백프레셔·드롭 정책의 실제 근거). 뒤에
  `[2B 길이][메시지]`가 Message Count개 반복.
- **DEEP 메시지** (오더북에 필요한 핵심 2종):
  - `Price Level Update (Buy)` = `0x38`, `(Sell)` = `0x35`
  - 필드: Event Flags(1) / Timestamp(8) / Symbol(8, 공백패딩) / Size(4)
    / Price(8, 고정소수점 ×10000)
  - ⚠️ 정확한 바이트 오프셋은 IEX DEEP 스펙 PDF로 대조 (버전 의존).
- **Event Flags = 배치 발행 신호**: "Event Processing Complete" 비트가 1일
  때만 책이 일관 상태 → **BBO 발행은 이때만** (배치 드레인과 연결).

### 파이프라인

```
[pcap replay ──UDP──▶]  Rx (IEX-TP, seq갭, PLU 필터) ─SPSC─▶ DEEP 디코드 ─SPSC─▶ 북 적용 + BBO 발행
                       recvfrom → 프레이밍 해체 →      슬롯 제자리 읽기    고정 book 배열 (사전 할당)
                       PLU 30B만 슬롯에 복사
       각 스테이지 경계에 TSC 타임스탬프 → 구간별 p99 히스토그램
```

**Rx가 필터 지점이다.** 실측(2018-01-27 pcap): 전체 105,068 메시지 중 PLU는
30,129개 — **71%는 큐를 안 건넌다.** 필터가 디코드 스테이지에 있으면 그 71%가
링 슬롯을 먹고, 캐시라인을 먹고, 소비자가 타입 바이트를 보고 버리는 데까지
코어를 쓴다. 프레이밍을 이미 벗겨 타입 바이트가 손에 있는 지점이 가장 싸다.

**큐 가득 참 = drop-newest** (`push_begin`이 NULL). 생산자를 멈춰 기다리면 소켓
버퍼가 넘쳐 어차피 커널이 버리는데, 그건 seq 갭으로만 보여 어디서 얼마나 잃었는지
알 수 없다. Rx에서 버리면 최소한 개수를 정확히 센다 (`rx_stats_t.dropped_full`).
갭 이후 첫 메시지에는 `RX_FLAG_AFTER_GAP`을 붙여 북 상태가 불완전함을 전달한다.

### 스테이지 간 데이터 전달 (2026-08-04 확정 / 08-08 일부 번복)

큐에 포인터나 인덱스를 싣지 않는다. 대신 **큐가 자기 슬롯 주소를 빌려주고,
호출자가 거기에 직접 쓴다.** (실측 근거: `test/bench/results.md`)

```c
TYPE *push_begin(q);   /* 빈 슬롯 주소, 가득 차면 NULL — tail은 아직 안 올림 */
void  push_commit(q);  /* tail release-store, 이 시점부터 소비자 소유 */
TYPE *pop_begin(q);    /* 다음 슬롯 주소, 비면 NULL */
void  pop_end(q);      /* head release-store */
```

- 기존 `push(q, const TYPE *item)`은 `buf[tail] = *item` 이라 호출자가 임시
  객체를 만들어 복사할 수밖에 없다. 슬롯 주소를 먼저 받으면 그 복사가 사라진다.
- 커밋 전이면 롤백이 공짜 — 실패 시 그냥 커밋 안 하면 그 자리가 재사용된다.

실측 (`test/bench/slot_vs_index.c`, cap=1024, 18회 중앙값 M msg/s.
생산자·소비자 모두 페이로드 전체를 만진다 — 디스어셈블리로 확인):

| payload | copy | borrow | index | scatter |
|---|---|---|---|---|
| 16B | 49.4 | **50.4** | 39.1 | 44.4 |
| 64B | **56.8** | 53.6 | 41.3 | 39.7 |
| 2048B | 5.8 | 17.9 | **23.4** | 15.0 |

- **작은 페이로드(16B/64B)에서 인덱스 전달이 진다** (borrow 대비 -13~-23%).
  파이프라인이 실제로 쓰는 크기(`rx_msg_t` 64B)가 이 영역이므로 이 API 결정은 유효하다.
  단 **원인은 "데이터를 큐 밖에 두는 것"도 "캐시라인 스트림이 2개로 느는 것"도
  아니다** — 데이터를 별도 풀에 두되 인덱스를 큐로 나르지 않는 변형(`implicit`)은
  borrow와 같은 성능이 나온다. 비용은 **소비자의 데이터 주소가 큐 로드에
  의존하면서 붙는 지연**이다 (가짜 의존성 주입 실험으로 확정, `test/bench/results.md` 5·6절).
- **값 복사는 슬롯이 커질 때만 아프다.** 16B/64B는 copy와 borrow가 겹치고
  (64B는 오히려 copy가 6% 앞선다), 2048B에서 5.8 vs 17.9 (borrow 3.1배).
  슬롯을 데이터 크기에 맞추면(지금 64B) 슬롯 대여의 복사 제거 효과는 사실상
  없다 — 남는 근거는 인덱스 전달 회피와 커밋 전 롤백이 공짜인 것.
- **2048B에서만 index가 앞서는 것은 "아무 일도 안 하는 소비자"가 만든 현상이다.**
  소비자 루프에 디코드 수준의 일감(~200사이클)만 넣어도 역전이 사라지고 borrow가
  앞선다. 놀고 있는 소비자는 생산자를 앞질러 읽어 생산자의 store queue를
  포화시키는데(스톨이 전체 사이클의 22~42%), 자기 일이 있으면 앞지르지 못한다.
  → **소비자가 일을 하는 한 두 크기 모두 borrow가 이긴다.** 자세한 경위는
  `test/bench/results.md` 5~8절.
- **벤치 교훈**: SPSC 벤치에서 소비자를 놀려두면 결론이 뒤집힌다. 스테이지 일감을
  파라미터로 두고 최소 한 점은 현실적인 값에서 재라.

#### recvfrom 제로카피는 채택하지 않았다 (2026-08-08 번복)

당초 "Rx가 `recvfrom(fd, slot->data, ...)`로 커널이 링 슬롯에 직접 쓰게 한다"로
확정했으나, 위 **PLU 필터와 동시에 성립하지 않는다.** 데이터그램 안에서 필요한
조각만 골라내는 이상 커널은 최종 목적지를 알 수 없다. 둘 중 필터를 택했다:

| | recvfrom 직접 | 필터 후 복사 (채택) |
|---|---|---|
| 슬롯 크기 | 2KB (최대 데이터그램) | **64B** (`rx_msg_t` = 캐시라인 1개) |
| 링 크기 (cap=1024) | 2MB | **64KB** — L2에 들어간다 |
| 복사 | 없음 | PLU당 30B 1회 |
| 다음 스테이지 일감 | 전체 메시지 | **29%** (PLU만) |

2KB 슬롯을 흘려보내며 71%를 뒤에서 버리는 것보다, 30B 복사를 내고 링을 L2에
넣는 쪽이 낫다고 판단. 되돌릴 조건: 필터율이 낮아지거나(PLU 비중이 커지거나)
가변 길이 메시지를 통째로 넘겨야 할 때.

`rx_msg_t`는 정확히 64B다 (`_Static_assert`로 고정). 첫 멤버에 `alignas(64)`를
걸어 구조체 정렬을 올렸다 — 매크로의 `alignas(64) TYPE buf[N]`은 배열 시작만
맞추므로, 원소가 64B가 아니면 슬롯이 캐시라인을 걸친다.

**mempool은 파이프라인에 쓰지 않는다.** 각 스테이지가 pop한 자리에서 처리를
끝내는 한 링 슬롯이 곧 풀이고 수명 관리는 head/tail이 한다. 풀이 필요해지는 건
소유권이 링의 FIFO 수명과 어긋날 때뿐 — (a) 슬롯 참조를 다음 스테이지로 넘겨
`pop_end`를 미뤄야 할 때, (b) 반환이 FIFO 순서가 아닐 때, (c) 가변 길이라 슬롯
크기를 못 고정할 때. `include/ll/mempool_*.h` 3종은 학습 산출물로 남기고 벤치
대상으로만 쓴다.

**이 API는 SPSC에서만 성립한다.** begin~commit 사이 슬롯은 "예약됐지만 미공개"
상태인데, 생산자가 둘이면 tail이 안 올라간 사이 같은 슬롯을 둘 다 받는다.
`fetch_add`로 자리를 예약해도 커밋 순서가 뒤집혀 아직 안 쓴 슬롯이 노출된다 —
막으려면 슬롯별 시퀀스 번호가 필요하고(Vyukov bounded MPMC) 그건 다른 큐다.
소비자 쪽도 대칭. 디코드를 2스레드로 늘리는 건 튜닝이 아니라 큐 교체다.

호출 규칙 (SPSC 전제가 함수 안이 아니라 호출자 코드까지 늘어난 대가):

1. `push_commit` 전에 `push_begin`을 다시 부르지 않는다 — 같은 슬롯이 두 번 나온다
2. `push_commit` 이후 그 포인터를 만지지 않는다 — 그 순간부터 소비자 소유
3. `pop_begin` 포인터는 `pop_end` 전까지만 유효 — 붙잡고 있으면 링이 막힌다

### 오더북 자료구조

- 시작: 사이드별 `{price, size}` 정렬 배열(삽입정렬), **양쪽 다 `[0]`이 best**
  (bid 내림차순 / ask 오름차순). BBO 발행이 첫 원소 두 개 읽기로 끝난다.
  정렬 방향이 반대인 코드를 두 벌 두면 한쪽만 고치는 버그가 나므로,
  bid에 부호를 뒤집어 두 사이드를 "키 오름차순" 한 벌로 처리한다.
- 최적화(측정 후): price-tick 직접 인덱싱 배열(O(1) 갱신) + best 포인터 캐싱.
- 심볼별 book은 사전 할당 배열(symbol → index), 동적 할당 금지.
  `book_set_t`는 19MB라 **`book_set_init`이 레벨 배열을 memset하지 않는다** —
  BSS/calloc으로 이미 0인 것을 전제하고 `n[]`만 신뢰한다. memset하면 실제로
  쓰는 심볼이 39개일 때도 19MB 전부를 물리 페이지로 끌어온다.
- 깊이: `BOOK_MAX_LEVELS=64`. 실측 최대 깊이는 사이드당 **14**라 여유가 크다.
  초과분은 best에서 가장 먼 레벨을 떨어뜨리고 `overflow`로 센다 (BBO 영향 없음).

**가격은 고정소수점 `x10000` 정수 그대로 둔다** (원래 "가격 스케일 해제"로
적었던 것에서 번복). 북의 핫패스는 가격을 *비교*하고 *찾을* 뿐 산술을 안 한다.
double로 바꾸면 정확한 정수 비교가 부동소수점 비교가 되고(틱 경계에서 `==`가
어긋날 수 있다) 디코드마다 변환이 붙는다. 십진 변환은 출력 지점에서만.

**심볼 → 인덱스**: 공백패딩 8바이트를 `uint64` 하나로 읽어 키로 쓴다 (strcmp도
길이 계산도 없음). 오픈 어드레싱 + 선형 탐사, 슬롯 16K개 사전 할당.
해시는 곱셈-시프트(Fibonacci) — ASCII 대문자라 하위 비트가 몰려 있어 마스킹만
하면 군집이 슬롯 군집이 된다. 슬롯 배열이 256KB지만 **워킹셋은 테이블 크기가
아니라 활성 심볼 수**다 (실측 39개). 인덱스는 등장 순서 발급 → 세션 밖으로
들고 나가지 말 것.

`delete_missing`(size==0인데 그 레벨이 없음)은 **갭 지표가 아니다.** 실측 pcap은
seq 1부터 시작해 갭이 0인데도 16건 나오고, 파이썬 독립 재생도 같은 16건이다 —
피드 자체가 없는 레벨에 삭제를 보낸다. 갭 판정은 `RX_FLAG_AFTER_GAP`이 한다.

### 진행 순서

1. **오프라인**: pcap 파일 읽어 IEX-TP→DEEP 파싱 → 단일 심볼 오더북 → BBO 출력
   (네트워크 없이 파싱/북 로직 정확성부터 확정).
2. pcap → UDP **replay**로 전환, Rx 스테이지를 실제 수신 경로에 태움.
3. SPSC 멀티스테이지 분리 + 레이턴시 하버스(구간별 TSC + 히스토그램) 탑재.

### 현재 위치 (2026-08-11)

| 영역 | 상태 |
|------|------|
| DEEP 파싱 (fread / libpcap / mmap) | 완료 — `test/parse/`, 전체 TSV 덤프까지 |
| UDP replay | 완료 — `test/udp/udp_replay_send.c` |
| SPSC 큐 3종 + 벤치 | 완료 — `bench/results.md` (cached 우세) |
| 슬롯 대여 API | `spsc_queue_cached.h`만 추가. 벤치 완료 — `test/bench/results.md` |
| **Rx 스테이지 → SPSC** | **완료** — `src/net/`, PLU만 필터해 큐로 |
| mempool 3종 | 헤더만, 벤치 없음 |
| **디코드 / 오더북 / BBO (오프라인)** | **완료 — 정답지 대조 일치** (아래) |
| 라이브 경로 소비자 | `rx_main.c`는 아직 개수만 세는 자리채움 |
| 디코드 스테이지 분리 (2번째 SPSC) | 미착수 — 나눌지 여부부터 측정 대상 |

#### 진행순서 1단계 완료 (2026-08-11)

`src/book/book_offline`: pcap → 프레이밍 → 디코드 → 북 → BBO, **단일 스레드**.

스테이지를 안 나눈 게 의도다. 디코드 일감이 필드 재배치 몇 개라 큐 핸드오프
비용보다 작을 수 있는데, 나눠놓고 시작하면 "나누는 게 이득인가"를 영영 못 잰다.
정확성을 먼저 고정하고 분리는 before/after가 있는 변경으로 따로 한다.
그래도 `rx_queue`는 통과시킨다 — 오프라인에서 확정한 것이 라이브와 같은
코드여야 검증이 옮겨간다 (`rx_framer_segment` 한 벌을 공유, 큐는 세그먼트
단위 배치 버퍼로 쓰임).

검증: `test/book/verify_bbo.sh`. `test/book/bbo_reference.py`가 `deep_dump.tsv`를
읽어 **공유 코드 없이**(파이썬, dict + max/min) BBO를 재생하고 diff한다.
같은 버그를 양쪽이 동시에 낼 확률이 낮아야 대조가 의미를 갖는다.

실측: 105,068 메시지 / PLU 30,129 / 심볼 39 / insert 3,157·update 23,954·delete
3,002 / 최대 깊이 14 / **BBO 23,065줄 전부 일치**. 크로스 북 0건(락 20건은
전부 테스트 심볼 ZWZZT의 bid==ask).

`rx_stage.c`는 `rx_framer_segment()`로 프레이밍을 분리했다 (recvfrom 루프와
pcap 리플레이가 같은 함수를 쓴다). 세그먼트를 넘는 상태는 `rx_framer_t`
하나뿐 — `expected_seq`, `pending_flags`.

`src/net/` (실행 파일 `rx_stage`): `rx_stage_run()`이 recvfrom → IEX-TP 프레이밍 →
seq 갭 검사 → PLU만 `rx_queue`로. `rx_main.c`는 개수만 세는 드레인 소비자
(디코드 스테이지 자리채움). 검증: `test/out/deep_dump.tsv` 대조 —
105,068 메시지 중 PLU 30,129개, push == pop == 30,129, 비-PLU 유입 0.
와이어 구조체는 `include/ll/iex_deep_wire.h`로 승격 (`test/common/` 삭제).

다음 작업 순서:

1. **라이브 경로에 북 태우기** — `rx_main.c`의 개수 세는 드레인을
   `deep_decode` + `book_apply` + BBO로 교체. UDP replay 결과가 오프라인
   BBO와 일치하는지 확인 (drop-newest 때문에 완전 일치는 아닐 수 있음 —
   `dropped_full`만큼 어긋나는지가 확인 대상). 진행순서 2단계.
2. **디코드 스테이지를 뗄지 측정** — 지금 단일 스레드인 디코드+북을 두 번째
   SPSC로 가르고 before/after. 디코드 일감이 작아 핸드오프 비용에 질 수
   있다는 게 가설. 지는 쪽으로 나오면 안 나누는 것도 결론이다.
3. 레이턴시 하버스 — 구간별 TSC + p50/p99/p99.9 히스토그램. 메시지별
   타임스탬프가 없어 지금 push→pop에 같은 세그먼트 앞쪽 메시지의 파싱 시간이
   섞이는 문제(아래 측정 메모)를 여기서 정리.
4. 북 최적화 — price-tick 직접 인덱싱 + best 캐싱. 1~3에서 나온 기준선이
   있어야 비교 대상이 생긴다.
5. (보류) 얕은 큐(cap=8)에서 슬롯 대여 vs 인덱스 전달 재측정. `test/bench/`는
   cap=1024만 재서 핸드오프 비용이 큐잉 대기시간에 묻혀 있다. nopad/padded에
   슬롯 대여 API를 추가하는 것도 여기서 함께 (파이프라인은 cached로 굳어 급하지 않음).

측정 메모: `rx_msg_t.rx_tsc`는 **데이터그램당 1회** 찍힌다. 그래서 push→pop
구간에 같은 세그먼트 앞쪽 메시지의 파싱 시간이 섞인다 (첫 실측 p50 385 /
p99 17.5k / p99.9 108k 사이클). 큐 핸드오프만 분리하려면 메시지별 타임스탬프가
하나 더 필요 — 레이턴시 하버스 작업에서 정리.

### 테스트 데이터

- IEX HIST: DEEP pcap T+1 무료 다운로드 (가용성은 사용 시점에 확인).
- 초기 로직 검증은 합성 DEEP 패킷으로도 가능.

## 환경

- **호스트**: Windows + WSL2
- **실행**: Docker 컨테이너 (Linux)
- **언어**: C11

### 환경 한계 (알고 진행)

| 항목 | 상태 |
|------|------|
| 코드 구현 / 빌드 | 정상 |
| `perf`, `bpftrace` | WSL2 커널 맞는 버전으로 사용 가능 |
| 상대적 성능 비교 | 가능 (절대 수치는 신뢰 불가) |
| `isolcpus`, C-states | 불가 (Windows 호스트가 제어) |
| DPDK / 커널 바이패스 | 불가 (가상 NIC) |
| 하드웨어 타임스탬프 | 불가 |
| NUMA 실험 | 불가 |
| Huge pages | 가능 (`MAP_HUGETLB`) |
| AF_XDP | 제한적 (가상 NIC 드라이버 의존) |
| 절대 지연 수치 | 의미 없음 (하이퍼바이저 노이즈) |

## 학습 범위

### 다룰 것

1. **시장 데이터 처리 파이프라인** (이 프로젝트의 메인)
   - UDP 수신 → IEX-TP/DEEP 디코드 → 오더북 적용 → BBO 발행
   - 시퀀스 갭 감지 / 백프레셔 / 드롭 정책

2. **메모리 관리**
   - 사전 할당 메모리 풀 (런타임 malloc 제거)
   - Huge pages 사전 할당 (`MAP_HUGETLB`)
   - `alignas(64)` 기반 캐시 라인 정렬 / false sharing 방지

3. **Lock-free 자료구조**
   - SPSC 큐 구현

4. **시간 측정**
   - RDTSC 기반 타이머 / TSC 드리프트 보정
   - `SO_TIMESTAMPING`으로 수신 시점 타임스탬프
   - p50 / p99 히스토그램

5. **네트워크**
   - UDP 수신 / pcap → UDP replay 로 수신 경로 재현
   - IEX-TP 시퀀스 번호 기반 패킷 손실/갭 감지
   - DEEP 바이너리 디코드 지연 벤치마크 (고정폭 파싱)

6. **프로파일링**
   - `perf stat`, `perf record` / `perf report`

7. **프로세스 간 통신**
   - 공유메모리 IPC (소켓/메시지 큐 대비 지연 최소화)
   - AF_XDP 경량 커널 바이패스 실험 (환경 의존적)

### 다루지 않을 것 (베어메탈 필요)

- DPDK, 커널 바이패스
- 하드웨어 타임스탬프
- `isolcpus`, `nohz_full` 커널 파라미터
- BIOS C-states 비활성화
- NUMA 최적화

## 프로젝트 구조

```
├── include/ll/      # 공개 헤더
│   ├── iex_deep_wire.h    # IEX-TP / DEEP 와이어 packed 구조체
│   ├── spsc_queue_*.h     # SPSC 큐 3종 (nopad / padded / cached)
│   ├── mempool_*.h        # 메모리 풀 3종 (학습 산출물, 벤치 대상)
│   ├── rx_msg.h           # 큐 원소 rx_msg_t + rx_queue 인스턴스화
│   ├── rx_stage.h         # Rx 스테이지 API + rx_framer_t + rx_stats_t
│   ├── deep_msg.h         # 디코드 출력 deep_update_t (정렬 완료, 64B)
│   ├── deep_decode.h      # rx_msg_t → deep_update_t (인라인)
│   ├── symtab.h           # 심볼 8B → 조밀 인덱스
│   ├── book.h             # L2 오더북 + BBO
│   └── tsc.h              # ll_rdtsc()
├── src/
│   ├── net/         # Rx 스테이지 (rx_stage.c) + 드레인 데모 (rx_main.c)
│   ├── deep/        # symtab.c (디코드 본체는 deep_decode.h 인라인)
│   ├── book/        # book.c + book_offline.c (오프라인 파이프라인)
│   ├── spsc/        # (비어 있음 — 큐는 헤더 온리)
│   └── mempool/     # (비어 있음)
├── bench/           # 벤치마크 (디코드/북 적용/지연 히스토그램)
├── test/            # 학습·실험 코드 (test/README.md 참고)
└── tools/           # perf 스크립트 등
```

빌드: `cmake -S . -B build && cmake --build build -j`

- 오프라인 파이프라인: `./build/src/book_offline test/data/*.pcap --bbo`
  (`--symbol AYI`로 한 심볼만). 검증: `./test/book/verify_bbo.sh`
- 라이브: `./build/src/rx_stage 9004 &` → `test/udp/udp_replay_send`

## 코딩 규칙

- 표준: C11
- 빌드: CMake
- 핫패스에 동적 할당 금지 (`new`, `malloc` 사용 금지)
- 핫패스에 시스템콜 최소화
- 핫패스 구조체 `alignas(64)` 정렬 (false sharing 방지)
- Lock-free 자료구조 우선 (mutex 사용 지양)
- 측정 없는 최적화 금지 — 반드시 before/after 벤치마크
- 주석은 WHY만, WHAT 설명 주석 금지
