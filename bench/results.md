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
| p99 | 1735~1775 ns | 559~591 ns |
| p99.9 | 4225~4575 ns | 730~1450 ns |
| throughput | ~18~20 M msg/s | ~18~20 M msg/s |

(3회 반복 실행 결과 범위)

### 결론

- p99/p99.9 테일 레이턴시가 NEW에서 일관되게 3~5배 낮음 — OLD는 producer의
  `tail` release-store와 consumer의 `buf[0]` 읽기가 같은 캐시라인에서
  충돌해 주기적으로 큰 스톨 발생.
- p50은 NEW가 약간 높음 — `buf`가 별도 캐시라인에서 시작해 첫 접근 시
  캐시라인 하나를 더 가져오는 비용.
- throughput은 동일 (전체 작업량이 같으므로).
- 저지연 트레이딩 관점에서는 평균보다 p99/p99.9가 중요하므로 분리가 합리적인
  트레이드오프.
