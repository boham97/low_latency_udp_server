# 벤치마크 결과

환경: WSL2 + Docker (절대 수치 무의미, OLD/NEW 상대 비교만 참고)

## spsc_cacheline_bench — tail/buf 캐시라인 분리

`include/ll/spsc_queue.h`의 `buf` 배열에 `alignas(64)`를 추가해
`tail`과 `buf[0]`이 같은 캐시라인을 공유하던 문제를 분리.

- queue capacity = 8, items = 2,000,000
- OLD: `buf`가 `tail`과 같은 캐시라인 공유 (수정 전)
- NEW: `buf`에 `alignas(64)` 적용, `tail`과 분리 (현재)

| 지표 | OLD | NEW |
|---|---|---|
| p50 | 260~385 ns | 370~431 ns |
| p90 | 459~460 ns | 500~501 ns |
| p99 | 1735~1775 ns | 559~591 ns |
| p99.9 | 4225~4575 ns | 730~1450 ns |
| throughput | ~18~20 M msg/s | ~18~20 M msg/s |

(3회 반복 실행 결과 범위)

### 결론

- p99/p99.9 테일 레이턴시가 NEW에서 일관되게 3~5배 낮음 — OLD는 producer의
  `tail` release-store와 consumer의 `buf[0]` 읽기가 같은 캐시라인에서
  충돌해 주기적으로 큰 스톨 발생.
- p50/p90은 NEW가 약간 높음 — `buf`가 별도 캐시라인에서 시작해 첫 접근 시
  캐시라인 하나를 더 가져오는 비용.
- p50→p90 증가폭은 OLD/NEW가 비슷함 (분포 본체는 같은 모양) — 반면 p90→p99
  구간에서 OLD만 폭증(+1300ns대). 즉 false sharing 스톨은 매 호출이 아니라
  상위 ~10% 샘플에서 간헐적으로 누적되어 꼬리에서만 터짐.
- throughput은 동일 (전체 작업량이 같으므로).
- 저지연 트레이딩 관점에서는 평균보다 p99/p99.9가 중요하므로 분리가 합리적인
  트레이드오프.

### 참고: 큐 깊이(QUEUE_CAP)와 대기시간

QUEUE_CAP을 8에서 256으로 늘려서 같은 벤치마크를 돌려보면 모든 지표가
ns 단위에서 μs 단위로(수백~수천 배) 증가한다. push-to-pop 레이턴시는
"이 아이템이 큐 안에서 머문 시간"까지 포함하므로, 큐가 깊어질수록
producer가 consumer보다 앞서나가 백로그를 더 많이 쌓을 수 있고
(Little's Law: 대기시간 ≈ 큐 적재량 / throughput), 그 결과 대기시간이
같이 증가한다. 큐가 작을 때(capacity=8)는 백로그가 최대 8개로 제한되어
이 효과가 무시할 만하지만, 깊이가 커지면 캐시라인 분리 효과보다
큐잉 지연이 지배적이 되어 OLD/NEW 비교 자체가 무의미해진다.
