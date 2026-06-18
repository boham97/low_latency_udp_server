# 벤치마크 결과

환경: WSL2 + Docker (절대 수치 무의미, 변형 간 상대 비교만 참고)

## spsc_variants_bench — SPSC 레이아웃 3종 비교

`include/ll/`의 SPSC 큐 변형 3종을 같은 알고리즘/측정 코드로 비교한다.
producer(코어 0) / consumer(코어 1) 별도 스레드 고정, push-to-pop 지연을
`clock_gettime(CLOCK_MONOTONIC)`으로 측정.

| 변형 | 헤더 | 레이아웃 |
|---|---|---|
| nopad | `spsc_queue_nopad.h` | 정렬 없음 — head/tail/buf 한 라인 (false sharing) |
| padded | `spsc_queue_padded.h` | `alignas(64)` 분리만 (캐싱 없음) |
| cached | `spsc_queue_cached.h` | 분리 + `cached_head`/`cached_tail` |

- items = 2,000,000
- **각 조합을 별도 프로세스로 3회 실행** — 한 프로세스에서 연달아 돌리면 앞
  실행이 캐시/TLB/페이지를 예열해 뒤 실행이 부당하게 유리해지므로 격리.
  아래 표는 3회의 중앙값(범위).
  ```
  spsc_variants_bench <nopad|padded|cached> [capacity(8|1024)]
  ```

### cap = 1024 (한쪽이 앞서 달릴 수 있는 깊은 큐)

깊은 큐에선 지연이 큐잉 대기시간(적재량)에 지배되어 노이즈가 크다 —
레이아웃/캐싱 효과는 **throughput**으로 본다.

| 지표 | nopad | padded | cached |
|---|---|---|---|
| **throughput** | **21.65 M/s** (21.1–23.5) | **19.46 M/s** (18.5–21.4) | **23.67 M/s** (23.6–30.2) |
| p50 | ~50000 ns (49.2–50.8k) | 노이즈 (1.1–19.9k) | ~45000 ns (26.8–45.6k) |
| p99 | ~68000 ns (67.0–73.7k) | ~89000 ns (88.0–92.2k) | ~61000 ns (51.7–65.3k) |

→ cached가 3회 모두 throughput 최상위. nopad/padded는 범위가 겹쳐 둘 간 우열은
무의미. cached는 p99도 가장 낮고 안정적.

### cap = 8 (lockstep — 큐가 계속 가득/빔 경계)

얕은 큐는 큐잉 지연이 작아 **p50**이 핸드오프 비용 신호로 유효하다.

| 지표 | nopad | padded | cached |
|---|---|---|---|
| **throughput** | **19.62 M/s** (19.1–19.9) | **17.56 M/s** (16.2–18.1) | **20.51 M/s** (19.2–20.8) |
| p50 | 351 ns (351–369) | 415 ns (411–439) | 380 ns (369–380) |
| p99 | 1689 ns (1661–1709) | 1790 ns (1786–1799) | 1801 ns (1755–1860) |
| p99.9 | 4879 ns (4329–12142) | 6290 ns (5089–6745) | 4409 ns (4390–5241) |

→ throughput은 cached > nopad > padded로 일관. p50은 셋 다 351–415ns로 거의 붙어
lockstep에선 캐싱 이득이 작다는 걸 확인 (매 연산마다 cached_* refresh).

(WSL2 노이즈로 단일 실행은 흔들리므로 3회 중앙값/범위로 기록.)

### 결론

- **cached가 두 깊이 모두 throughput 최상위** (cap=1024: 23.7 vs 19.5~21.7,
  cap=8: 20.5 vs 17.6~19.6, 모두 3회 중앙값). producer가 consumer보다 앞서
  달리는 동안 `cached_head`/`cached_tail`로 통과하므로 상대 코어가 소유한
  인덱스 라인을 읽지 않아 코히런스 트래픽이 사라진 효과.
- **cap=8(lockstep)에서는 세 변형 차이 미미.** 큐가 매 연산마다 가득/빔 경계를
  밟아 `cached_*`가 거의 매번 refresh되니 캐싱 이득이 사라진다. 이 최적화는
  "한쪽이 여러 칸 앞서 달릴 수 있을 때" 빛난다.
- 지연(특히 cap=1024의 p50)은 큐잉 대기시간이 지배적이라 핸드오프 비용 지표로는
  부적합 — 아래 큐 깊이 노트 참고.

### 큐 깊이(capacity)와 대기시간

push-to-pop 지연은 "이 아이템이 큐 안에서 머문 시간"을 포함한다. 큐가 깊을수록
producer가 백로그를 더 쌓을 수 있어(Little's Law: 대기시간 ≈ 적재량 /
throughput) 지연이 같이 커진다. 그래서 cap=8은 수백 ns, cap=1024는 수만 ns대다.
깊은 큐에서 지연 비교는 큐잉 지연에 묻히므로, 레이아웃/캐싱 효과는 **throughput**
으로, 핸드오프 지연은 **얕은 큐(cap=8)**로 보는 것이 맞다.

### false sharing (nopad → padded)

`nopad`는 head/tail이 정렬 없이 인접해 같은 64바이트 라인을 공유한다.
producer의 `tail` release-store와 consumer의 `head` 갱신이 같은 라인을 두고
핑퐁하며 매 연산마다 코어 간 라인 invalidate가 발생한다. `padded`는
`alignas(64)`로 head/tail/buf를 각각 다른 라인에 놓아 이 묶임을 끊는다.
다만 padded는 여전히 매 push가 상대의 head를, 매 pop이 상대의 tail을 load
하므로 그 라인의 코히런스 트래픽은 남고 — 그걸 마저 없앤 것이 cached다.
