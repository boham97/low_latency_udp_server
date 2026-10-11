#!/usr/bin/env python3
"""
level_dist.py — PLU가 북의 어느 위치를 건드리는지 분포를 낸다.

북 레이아웃을 고르기 전에 "탐색이 몇 칸에서 끝나는가, memmove가 몇 레벨을
옮기는가, best에서 얼마나 떨어진 갱신인가"를 먼저 본다. 결과와 해석은
bench/results.md의 book_layout_bench 절.

i는 best(0)부터 센 위치. bisect를 쓰지만 재는 건 위치이지 탐색 방식이 아니다.

usage: level_dist.py <deep_dump.tsv>
"""
import sys, bisect
from collections import Counter

def parse_px(s):
    whole, _, frac = s.partition(".")
    return int(whole) * 10000 + int((frac + "0000")[:4])

book = {}
pos = {"insert": Counter(), "update": Counter(), "delete": Counter()}
depth_at = Counter()
tick_dist = Counter()  # |price - best| in cents for updates
moved = Counter()
with open(sys.argv[1]) as f:
    next(f)
    for line in f:
        p = line.split()
        if p[3] not in ("8", "5"):
            continue
        sym, size, px = p[9], int(p[10]), parse_px(p[11])
        bid = p[3] == "8"
        lv = book.setdefault((sym, bid), [])  # sorted keys, best first
        key = -px if bid else px
        i = bisect.bisect_left(lv, key)
        found = i < len(lv) and lv[i] == key
        n = len(lv)
        depth_at[n] += 1
        if size == 0:
            if found:
                pos["delete"][i] += 1; moved[n - i - 1] += 1; lv.pop(i)
        elif found:
            pos["update"][i] += 1
        else:
            pos["insert"][i] += 1; moved[n - i] += 1; lv.insert(i, key)
        if lv:
            tick_dist[min(abs(key - lv[0]) // 100, 50)] += 1

def show(name, c):
    tot = sum(c.values()); acc = 0
    print(f"{name} (n={tot})")
    for k in sorted(c):
        acc += c[k]
        print(f"  {k:3d}: {c[k]:6d}  cum {100*acc/tot:5.1f}%")
for k, c in pos.items():
    show(f"i ({k})", c)
show("levels moved by memmove", moved)
show("depth at apply", depth_at)
show("distance from best (cents, capped 50)", tick_dist)
