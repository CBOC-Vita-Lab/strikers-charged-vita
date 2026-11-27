#!/bin/bash
# 1.3 MSC_BG30: the picture decision (np_bg30.cpp) on synthetic camera streams with MSC_BG30=0, 1, 2; the mutants NP_BG30_BREAK=1..4 must FAIL. Seconds.
#   native/tools/bg30/run_bg30_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np_bg30_test}
mkdir -p "$OUT"
CXX=${CXX:-clang++}
$CXX -std=c++17 -O1 -Iinclude tools/bg30/bg30_unit.cpp -o "$OUT/bg30_unit"
for m in 0 1 2; do MSC_BG30=$m "$OUT/bg30_unit"; done
for b in 1 2 3 4 5; do
    $CXX -std=c++17 -O1 -DNP_BG30_BREAK=$b -Iinclude tools/bg30/bg30_unit.cpp -o "$OUT/bg30_break$b"
    if MSC_BG30=2 "$OUT/bg30_break$b" > "$OUT/break$b.out" 2>&1; then echo "bg30: FAILED (BREAK=$b passed)"; exit 1; fi
    echo "bg30: BREAK=$b is caught ($(grep -c '^FAIL' "$OUT/break$b.out") checks failed, first: $(grep -m1 '^FAIL' "$OUT/break$b.out" | cut -c1-110))"
done
echo "bg30 decision: passed"
