#!/bin/bash
# Wave 9 (pacing): the slip budget (MSC_SLIP_BUDGET_MS) on synthetic frame times around the real np_fps.cpp: the table of game time against real time per pacing state, the bound, and the
# mutants (NP_SLIP_BREAK 1..4 must FAIL). No game data, seconds.   native/tools/fps/run_slip_budget_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-${BUILD:-/tmp/np_slip_budget_test}}
mkdir -p "$OUT"
CXX=${CXX:-clang++}
$CXX -std=c++11 -O1 -DNP_NATIVE=1 -Iinclude -Iplatform/compat tools/fps/slip_budget_unit.cpp -o "$OUT/slip_budget_unit"
"$OUT/slip_budget_unit" 2>&1 | grep -v '^\[fps\]\|^\[slip\]'
for b in 1 2 3 4; do
    $CXX -std=c++11 -O1 -DNP_NATIVE=1 -DNP_SLIP_BREAK=$b -Iinclude -Iplatform/compat tools/fps/slip_budget_unit.cpp -o "$OUT/slip_budget_break$b"
    if "$OUT/slip_budget_break$b" > "$OUT/break$b.out" 2>&1; then echo "slip budget: FAILED (BREAK=$b passed)"; exit 1; fi
    echo "slip budget: BREAK=$b is caught ($(grep -c '^FAIL' "$OUT/break$b.out") checks failed, first: $(grep -m1 '^FAIL' "$OUT/break$b.out" | cut -c1-110))"
done
echo "slip budget: passed"
