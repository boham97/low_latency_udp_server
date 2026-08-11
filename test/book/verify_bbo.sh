#!/bin/sh
# verify_bbo.sh — 오프라인 파이프라인의 BBO를 정답지와 대조한다.
#
#   build/src/book_offline   pcap → 프레이밍 → 디코드 → 정렬배열 북 → BBO
#   test/book/bbo_reference.py  deep_dump.tsv → dict 북 → BBO   (독립 구현)
#
# 두 경로가 공유하는 코드가 없다는 게 요점이다. 일치하면 파싱/심볼매핑/북적용/
# EPC 발행시점이 전부 맞다는 뜻이고, 어긋나면 어느 seq에서 갈라졌는지 바로 나온다.
#
# 프로젝트 루트에서 실행.
set -e

PCAP=${1:-test/data/20180127_IEXTP1_DEEP1.0.pcap}
DUMP=${2:-test/out/deep_dump.tsv}
OUT=test/out

[ -x build/src/book_offline ] || { echo "build/src/book_offline 없음 — cmake --build build" >&2; exit 1; }
[ -f "$DUMP" ] || { echo "정답지 $DUMP 없음 — test/parse/deep_dump으로 생성" >&2; exit 1; }

mkdir -p "$OUT"
./build/src/book_offline "$PCAP" --bbo > "$OUT/bbo_impl.tsv" 2> "$OUT/bbo_impl_summary.txt"
python3 test/book/bbo_reference.py "$DUMP" > "$OUT/bbo_ref.tsv"

cat "$OUT/bbo_impl_summary.txt"

if diff -q "$OUT/bbo_ref.tsv" "$OUT/bbo_impl.tsv" > /dev/null; then
    echo
    echo "=> BBO $(wc -l < "$OUT/bbo_impl.tsv") 줄 전부 일치. 파싱/북/발행시점 확정."
else
    echo
    echo "=> 불일치. 첫 10줄:"
    diff "$OUT/bbo_ref.tsv" "$OUT/bbo_impl.tsv" | head -10
    exit 1
fi
