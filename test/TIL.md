# TIL — `#pragma`와 구조체 패딩 (2026-07-18)

## `#pragma`란

- `#`으로 시작하는 **전처리기 지시문** 중 하나. (`#include`, `#define`과 같은 부류)
- `#include`/`#define`은 동작이 표준으로 정해져 있지만,
  **`#pragma`는 "표준 C 밖, 그 컴파일러만의 고유 기능을 켜는 창구"**.
- 표준이 정한 규칙은 딱 하나: **모르는 pragma는 조용히 무시**(에러 아님).
  → 특정 컴파일러 기능을 써도 다른 컴파일러에서 빌드가 깨지진 않음(그 기능만 안 먹음).

```c
#pragma pack(1)              // 구조체 패딩 제거
#pragma once                 // 이 헤더는 한 번만 include
#pragma GCC optimize("O3")   // GCC 전용 최적화
#pragma omp parallel for     // OpenMP 병렬화
```

## 구조체 패딩(padding)

- CPU는 4바이트 값을 4의 배수 주소에서 읽을 때 빠름(정렬, alignment).
- 그래서 컴파일러가 필드 사이에 **빈 바이트(패딩)를 몰래 삽입**해서 정렬을 맞춤.

```c
struct { uint16_t a; uint32_t b; uint16_t c; };
// 기본:   2+4+2 = 8이 아니라 12바이트  (a 뒤 2B 패딩 + 끝 2B 패딩)
//   [a 2][pad 2][   b 4   ][c 2][pad 2]
//    0         4           8
```

- 패딩의 크기·위치는 **컴파일러/CPU마다 다를 수 있음**.

## `#pragma pack(push, 1)` / `#pragma pack(pop)`

- `push, 1` = 현재 정렬 설정을 저장(push)하고 정렬을 1로 → **패딩 제거, 필드 딱 붙임**.
- `pop` = 저장해둔 설정으로 복원 → 아래 다른 코드엔 영향 안 줌.

```c
#pragma pack(push, 1)
struct { uint16_t a; uint32_t b; uint16_t c; };  // 8바이트, b는 offset 2
#pragma pack(pop)
```

## 네트워크 통신에서 왜 필수인가

- 패킷 버퍼를 구조체로 그대로 해석(제로카피)하려면 **송신·수신 양쪽의 메모리 배치가 정확히 같아야** 함.
- pack 안 하면 컴파일러/플랫폼마다 패딩이 달라져 → 필드 위치가 어긋나 값이 깨짐.
- 그래서 통신 구조체는 `pack(1)`로 배치를 못박음.

## 관련 메모

- `#pragma pack`은 **어떤 C 표준에도 없음** (MSVC 유래, GCC/Clang이 호환 지원하는 de-facto 표준).
- C11 `alignas`는 정렬을 **늘리는 것만** 되고 줄이는(pack) 건 안 됨 → 표준 packing 방법은 없음.
- GCC/Clang 전용이면 `struct {...} __attribute__((packed));`가 더 관용적.
- 배치 검증은 컴파일 타임에: `_Static_assert(sizeof(struct x) == 8, "layout mismatch");`

## 이 프로젝트(IEX DEEP)와의 연결

- IEX DEEP 메시지도 고정폭 바이너리라 **패딩 없는 배치**가 전제.
  (Event Flags 1 / Timestamp 8 / Symbol 8 / Size 4 / Price 8)
- 단, DEEP는 Timestamp가 offset 1 같은 **미정렬 위치**에 옴 → packed 구조체 오버레이 후
  값 읽기 or 필드 memcpy로 안전하게 접근(멤버 주소를 뽑아 넘기는 건 금지).
- IEX는 **리틀엔디언** → x86에선 바이트 스왑 없이 그대로 읽힘.
