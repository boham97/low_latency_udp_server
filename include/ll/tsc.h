#ifndef LL_TSC_H
#define LL_TSC_H

#include <stdint.h>

/*
 * 스테이지 경계 타임스탬프용 TSC 읽기.
 *
 * 직렬화(lfence/cpuid)를 넣지 않는다. rdtsc는 주변 명령과 재정렬될 수 있어
 * 몇 사이클의 오차가 생기지만, lfence 자체가 수십 사이클이라 측정 대상(스테이지
 * 핸드오프)보다 비싸질 수 있다. 여기서 재는 것은 명령 단위가 아니라 구간이므로
 * 재정렬 오차는 히스토그램 폭 안에 묻힌다.
 *
 * WSL2/하이퍼바이저 환경이라 사이클→ns 변환의 절대값은 신뢰하지 않는다.
 * before/after 상대 비교에만 쓴다.
 */
static inline uint64_t ll_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif /* LL_TSC_H */
