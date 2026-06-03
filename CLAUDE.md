# Low Latency Server Study

## 프로젝트 목적

저지연 거래 시스템 구조 공부.

목표: wire-to-wire 50μs 이하 달성 원리 이해 및 구현 실습

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

1. **시장 데이터 처리 파이프라인**
   - UDP 수신 → 필터링 → 가격 결정 → 전략 → 주문 송신

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
   - UDP 유니캐스트 / 멀티캐스트 수신
   - 시퀀스 번호 기반 패킷 손실 감지
   - 직렬화 포맷 비교 (JSON vs 바이너리) 파싱 지연 벤치마크

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

## 코딩 규칙

- 표준: C11
- 빌드: CMake
- 핫패스에 동적 할당 금지 (`new`, `malloc` 사용 금지)
- 핫패스에 시스템콜 최소화
- 핫패스 구조체 `alignas(64)` 정렬 (false sharing 방지)
- Lock-free 자료구조 우선 (mutex 사용 지양)
- 측정 없는 최적화 금지 — 반드시 before/after 벤치마크
- 주석은 WHY만, WHAT 설명 주석 금지
