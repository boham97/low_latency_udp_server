/*
 * buftest.c — stdio 버퍼링 모드가 "출력 대상(터미널이냐 아니냐)"으로 정해짐을 보이는 데모
 *
 * 같은 코드라도 stdout 이 어디로 향하느냐에 따라 glibc 가 버퍼링을 다르게 건다:
 *   - 터미널(isatty=1)  → line-buffered : '\n' 마다 flush → write() 자주
 *   - 파일/파이프/디바이스(isatty=0) → full-buffered : 버퍼(보통 4096B)가 찰 때,
 *     또는 프로그램 종료 시, 또는 fflush() 때만 flush → write() 드물게
 *
 * /dev/null 도 터미널이 아니라 full-buffered 로 동작한다 — 데이터는 버려져도
 * 포매팅/버퍼복사/write() 시스템콜은 그대로 일어나고, 절약되는 건 실제 I/O 뿐.
 *
 * Build: gcc -O2 -Wall -o buftest buftest.c
 * 관찰:
 *   ./buftest                          # 터미널: 줄마다 바로 뜸(line-buffered)
 *   strace -e write ./buftest >/dev/null   # write() 몇 번인지 세보기(full-buffered)
 *   strace -e write ./buftest | cat        # 파이프도 동일하게 full-buffered
 *   strace -e write stdbuf -oL ./buftest >/dev/null    # 밖에서 강제 line-buffered 로 바꿔보기
 */
#include <stdio.h>
#include <unistd.h>

int main(void) {
    /* stdout 이 터미널인지 여부 = glibc 가 버퍼링 모드를 고르는 바로 그 기준 */
    printf("stdout isatty = %d  (%s)\n",
           isatty(STDOUT_FILENO),
           isatty(STDOUT_FILENO) ? "line-buffered" : "full-buffered");

    /* 약 8~9KB 출력. full-buffered 면 4096B 씩 모여 write() 가 3번 정도,
       line-buffered 면 줄마다 나가 write() 가 1000번대로 늘어난다.
       strace -e write 로 그 횟수 차이를 직접 확인하는 게 이 데모의 핵심. */
    for (int i = 0; i < 1000; ++i)
        printf("line %d\n", i);

    return 0;
}
