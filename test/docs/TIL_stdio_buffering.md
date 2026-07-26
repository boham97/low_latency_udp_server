# TIL — stdio 버퍼링은 `isatty`로 정해진다 (`stdbuf`/`strace`) (2026-07-19)

## 발단

UDP 서버를 `> server.log`로 리다이렉트하고, "listen 배너가 파일에 뜨면 송신 시작"
하도록 `grep`으로 동기화했더니 **송신기가 죽은 서버로 쏴서 datagrams=0**.
원인은 UDP가 아니라 **배너가 stdio 버퍼에 갇혀 파일까지 못 간 것**.

## printf는 즉시 나가지 않는다 — 버퍼링 3모드

`printf`/`fwrite`는 바로 `write()` 하지 않고 stdio 버퍼에 모았다가 내보낸다.
언제 내보내나(flush)가 모드별로 다름:

| 모드 | flush 시점 | 자동 적용 대상 |
|------|-----------|---------------|
| unbuffered | 즉시 | stderr |
| line-buffered | `\n` 마다 | stdout이 **터미널**일 때 |
| full-buffered | 버퍼(4096B) 참 / 프로그램 종료 / `fflush()` | stdout이 **파일·파이프·`/dev/null`** |

- **결정 기준은 딱 하나: `isatty(fd)`** — glibc가 시작 시 "이 stdout이 터미널이냐"만 봄.
- full-buffered에선 **`\n`이 flush를 안 시킨다** (그건 line 규칙). 이게 함정.

## 왜 배너가 갇혔나

```c
printf("...listen...\n");   // 배너 40B → 버퍼에 복사 (4096 안 참)
for(;;) recvfrom(...);      // 여기서 오래 블록 → 프로그램 안 죽음
```
full-buffer flush 3조건(버퍼 참/종료/fflush)이 **전부 미달** → 배너가 메모리에만 있고
파일엔 0바이트 → `grep`이 못 찾음. 서버가 2초 후 종료하며 flush되자 그제야 파일에 찍힘.
→ **해결: 배너 뒤 `fflush(stdout)`** (또는 진단 로그는 항상 unbuffered인 stderr로).

## `/dev/null`도 full-buffered

버려지는 곳이라고 버퍼링이 사라지지 않음. `/dev/null`은 터미널이 아니라 `isatty=0`
→ full-buffered. **포매팅·버퍼복사·`write()`는 그대로 일어나고, 절약되는 건 실제 I/O뿐.**
→ 핫패스 로그를 `> /dev/null`로 껐다고 비용이 사라진다는 건 오해.

## `stdbuf` — 소스 수정 없이 밖에서 버퍼링 바꾸기

`LD_PRELOAD`로 `libstdbuf.so`를 주입해 프로그램 시작 시 `setvbuf`를 대신 걸어줌.

```bash
stdbuf -oL ./prog >log   # stdout 강제 line-buffered
stdbuf -o0 ./prog >log   # 강제 unbuffered
```

## 효과는 `strace`로만 보인다 (측정)

`>/dev/null`이면 화면 출력은 둘 다 똑같음 → 차이는 **시스템콜 레벨**에만 존재.
`strace -f -e trace=write`로 write() 횟수를 세야 드러남 (`-f`=exec된 자식 추적):

```bash
strace -f -e trace=write ./buftest >/dev/null              # 기본
strace -f -e trace=write stdbuf -oL ./buftest >/dev/null   # 강제 line
```

buftest = 1000줄 출력. 관측:

| 실행 | 버퍼링 | write() 횟수 | 한 번에 |
|------|--------|-------------|---------|
| `./buftest >/dev/null` | full | **3** | 4096B씩 |
| `stdbuf -oL ./buftest >/dev/null` | line | **1001** | 줄마다 7B |

→ line-buffer 즉시성의 대가 = **시스템콜 약 300배**.

## 이 프로젝트(저지연)와의 연결

- 3 vs 1001이 곧 **"핫패스 시스템콜 최소화"** 가 왜 중요한지의 실측.
- 핫패스에서 `printf`/`fwrite` 금지 이유: ① flush 순간 `write()` 스파이크가 p99 꼬리를
  튀김 ② 포매팅 비용 ③ stdio 내부 락(`flockfile`) ④ 블로킹 I/O로 파이프라인 정지.
  → 원시값만 SPSC 링버퍼에 남기고 **별도 스레드가 포매팅·flush**(deferred formatting).
- `strace -e trace=write`로 시스템콜 세기 = `perf` 가기 전, 최적화 before/after를
  검증하는 가장 간단한 측정 도구.
- 진단·상태 로그는 **stderr(항상 unbuffered)** 로 → 리다이렉트해도 안 갇힘.

## 참고: 데모 코드

`test/buftest.c` — `isatty`로 버퍼링 모드가 갈리는 걸 `strace`로 관찰하는 데모.
