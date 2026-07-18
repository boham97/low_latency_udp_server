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

---

# TIL — 파일 읽기: `malloc`+`fread` vs `mmap` (2026-07-19)

## 뭘 바꿨나

파서가 pcap 파일을 읽는 방식만 교체(파싱 로직은 한 줄도 안 건드림).

```c
// before: 빈 메모리 잡고 파일 전체를 복사
uint8_t *buf = malloc(fsz);
fread(buf, 1, fsz, fp);

// after: 파일을 주소 공간에 직접 매핑, 복사 없음
uint8_t *buf = mmap(NULL, fsz, PROT_READ, MAP_PRIVATE, fd, 0);
```

- `buf[i]`로 접근하는 결과는 동일 → 출력 완전 일치.
- 차이는 "어떻게 메모리로 가져오나"에 있음.

## 세 방식의 read 전략

| 지표 | 손파싱(fread) | mmap | libpcap(read 4KB) |
|------|---------------|------|-------------------|
| read syscall | 1회(대신 큰 복사) | **0회** | 2,576회 |
| 전체 복사 | 있음(10.5MB) | **없음** | 없음 |
| page-fault | 2,634 | **223** | 179 |
| IPC | 0.70 | **2.26** | 2.01 |
| 실행 시간 | 10.5 ms | **2.3 ms** | 7.9 ms |

## 왜 mmap이 빠른가

1. **전체 복사가 사라짐** → 복사에 쓰던 명령 자체가 없어짐(instructions 30.5M→19.4M).
2. **page-fault 12배 감소** — mmap은 파일 매핑이라 fault 시 커널이 여러 페이지를
   미리 당겨옴(readahead/fault-around). 반면 `malloc`+`fread`는 익명 메모리라
   4KB 페이지마다 minor fault → 10.5MB ÷ 4KB ≈ 2,570개(관측 2,634와 일치).
3. **캐시 압박 절반** — fread는 "커널버퍼→내버퍼"로 메모리를 두 번 훑지만
   mmap은 파싱하며 한 번만 훑음(cache-refs 2.06M→0.52M).

## 메모

- `open`→`fstat`(크기)→`mmap` 순서. 매핑 후엔 `close(fd)` 해도 매핑은 유효.
- 정리는 `free`가 아니라 `munmap(buf, fsz)`.
- `read` 4KB 반복(libpcap)은 syscall 폭탄, `fread` 전체 복사는 page-fault 폭탄
  → 파일 기반 파싱에선 둘 다 피하는 mmap이 정답.
- WSL2라 절대 수치는 노이즈지만, page-fault(2634 vs 223)·IPC(0.70 vs 2.26) 차이는
  노이즈로 안 뒤집히는 크기 → 결론 유효.
