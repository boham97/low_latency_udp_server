#!/usr/bin/env python3
"""
bbo_reference.py — deep_dump.tsv를 정답지로 BBO를 독립 재생한다.

src/book/ 과 공유하는 코드가 하나도 없다는 게 이 스크립트의 유일한 존재 이유다.
입력은 이미 검증된 덤프(test/out/deep_dump.tsv), 언어도 자료구조도 다르다
(정렬 배열 대신 dict + max/min). 같은 버그를 양쪽이 동시에 낼 확률이 낮아야
대조가 의미를 갖는다.

usage: bbo_reference.py <deep_dump.tsv>   > reference.tsv
출력: seq \t symbol \t bid_px \t bid_sz \t ask_px \t ask_sz   (EPC 메시지마다 1줄)
"""
import sys


def parse_px(s):
    """'12.3400' -> 123400 (고정소수점 x10000). float를 거치면 반올림 오차가 난다."""
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    whole, _, frac = s.partition(".")
    v = int(whole) * 10000 + int((frac + "0000")[:4])
    return -v if neg else v


def fmt_px(v):
    sign = "-" if v < 0 else ""
    a = abs(v)
    return f"{sign}{a // 10000}.{a % 10000:04d}"


def main(path):
    # book[symbol] = ({bid price: size}, {ask price: size})
    book = {}
    out = sys.stdout

    with open(path) as f:
        next(f)  # 헤더
        for line in f:
            p = line.split()
            typ = p[3]
            if typ not in ("8", "5"):
                continue
            # PLU는 타임스탬프가 항상 있어 컬럼 위치가 고정이다
            # seq pkt midx type name flags ts_ns date time symbol size price ...
            seq, flags, sym = p[0], int(p[5], 16), p[9]
            size, px = int(p[10]), parse_px(p[11])

            sides = book.setdefault(sym, ({}, {}))
            side = sides[0] if typ == "8" else sides[1]

            if size == 0:
                side.pop(px, None)
            else:
                side[px] = size

            if flags & 0x01:  # Event Processing Complete
                bids, asks = sides
                bp = max(bids) if bids else None
                ap = min(asks) if asks else None
                out.write(
                    "%s\t%s\t%s\t%d\t%s\t%d\n"
                    % (
                        seq,
                        sym,
                        fmt_px(bp) if bp is not None else "-",
                        bids[bp] if bp is not None else 0,
                        fmt_px(ap) if ap is not None else "-",
                        asks[ap] if ap is not None else 0,
                    )
                )


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "test/out/deep_dump.tsv")
