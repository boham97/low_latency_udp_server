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
[pcap replay ──UDP──▶]  Rx (IEX-TP, seq갭) ─SPSC─▶ DEEP 디코드 ─SPSC─▶ 북 적용 + BBO 발행
                       recvfrom → 링 슬롯 직접     슬롯 제자리 읽기    고정 book 배열 (사전 할당)
       각 스테이지 경계에 TSC 타임스탬프 → 구간별 p99 히스토그램
```

### 스테이지 간 데이터 전달 (2026-08-04 확정)

큐에 포인터나 인덱스를 싣지 않는다. 대신 **큐가 자기 슬롯 주소를 빌려주고,
호출자가 거기에 직접 쓴다.**

```c
TYPE *push_begin(q);   /* 빈 슬롯 주소, 가득 차면 NULL — tail은 아직 안 올림 */
void  push_commit(q);  /* tail release-store, 이 시점부터 소비자 소유 */
TYPE *pop_begin(q);    /* 다음 슬롯 주소, 비면 NULL */
void  pop_end(q);      /* head release-store */
```

- 기존 `push(q, const TYPE *item)`은 `buf[tail] = *item` 이라 호출자가 임시
  객체를 만들어 복사할 수밖에 없다. 슬롯 주소를 먼저 받으면 그 복사가 사라진다.
- Rx는 `recvfrom(fd, slot->data, ...)`로 **커널이 링 슬롯에 직접 쓰게** 한다.
  커밋 전이면 롤백이 공짜 — 실패 시 그냥 커밋 안 하면 그 자리가 재사용된다.
- 대가: 슬롯이 최대 데이터그램 크기 고정 (cap=1024 × 2KB → 링 하나 2MB).

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

- 시작: 사이드별 `{price, size}` 정렬 배열(삽입정렬), best = 끝/앞.
- 최적화(측정 후): price-tick 직접 인덱싱 배열(O(1) 갱신) + best 포인터 캐싱.
- 심볼별 book은 사전 할당 배열(symbol → index), 동적 할당 금지.

### 진행 순서

1. **오프라인**: pcap 파일 읽어 IEX-TP→DEEP 파싱 → 단일 심볼 오더북 → BBO 출력
   (네트워크 없이 파싱/북 로직 정확성부터 확정).
2. pcap → UDP **replay**로 전환, Rx 스테이지를 실제 수신 경로에 태움.
3. SPSC 멀티스테이지 분리 + 레이턴시 하버스(구간별 TSC + 히스토그램) 탑재.

### 현재 위치 (2026-08-04)

| 영역 | 상태 |
|------|------|
| DEEP 파싱 (fread / libpcap / mmap) | 완료 — `test/parse/`, 전체 TSV 덤프까지 |
| UDP replay + Rx seq 갭 | 완료 — `test/udp/` (독립 실행, 큐와 미연결) |
| SPSC 큐 3종 + 벤치 | 완료 — `bench/results.md` (cached 우세) |
| mempool 3종 | 헤더만, 벤치 없음 |
| **오더북 / BBO** | **미착수** ← 진행순서 1단계의 남은 조각 |
| `src/` | 비어 있음 (디렉토리 뼈대만) |

다음 작업 순서:

1. SPSC 헤더 3종에 슬롯 대여 API 추가 → 값 복사 버전과 벤치 비교
   (payload 16B / 32B / 2KB로 "언제부터 복사가 아픈가" 측정)
2. `src/deep/` — `test/parse/`의 디코드 로직 승격 (지금 파일 3개에 복붙 계보)
3. `src/book/` — 사이드별 `{price,size}` 정렬배열, `size==0` 삭제,
   심볼→인덱스 사전 할당
4. Event Processing Complete 비트에서만 BBO 발행 →
   `test/out/deep_dump.tsv`를 정답지로 특정 심볼 BBO 추이 대조

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
├── src/             # net(수신/replay), spsc, mempool,
│                    #   deep(IEX-TP/DEEP 디코드), book(오더북) 등
├── bench/           # 벤치마크 (디코드/북 적용/지연 히스토그램)
└── tools/           # perf 스크립트, pcap replay 등
```

## 코딩 규칙

- 표준: C11
- 빌드: CMake
- 핫패스에 동적 할당 금지 (`new`, `malloc` 사용 금지)
- 핫패스에 시스템콜 최소화
- 핫패스 구조체 `alignas(64)` 정렬 (false sharing 방지)
- Lock-free 자료구조 우선 (mutex 사용 지양)
- 측정 없는 최적화 금지 — 반드시 before/after 벤치마크
- 주석은 WHY만, WHAT 설명 주석 금지
