#!/bin/bash
# Wave 9 (xcheck): the game-speed meter (MSC_SPEED_LOG, np_speed.h) on synthetic frame streams, and the mutants (NP_SPEED_BREAK 1..4 must FAIL). No game data, seconds.
#   native/tools/fps/run_speed_meter_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np_speed_meter_test}
mkdir -p "$OUT"
CXX=${CXX:-clang++}
$CXX -std=c++11 -O1 -Iplatform/compat tools/fps/speed_meter_unit.cpp -o "$OUT/speed_meter_unit"
"$OUT/speed_meter_unit"
for b in 1 2 3 4; do
    $CXX -std=c++11 -O1 -DNP_SPEED_BREAK=$b -Iplatform/compat tools/fps/speed_meter_unit.cpp -o "$OUT/speed_meter_break$b"
    if "$OUT/speed_meter_break$b" > "$OUT/break$b.out" 2>&1; then echo "speed meter: FAILED (BREAK=$b passed)"; exit 1; fi
    echo "speed meter: BREAK=$b is caught ($(grep -c '^FAIL' "$OUT/break$b.out") checks failed, first: $(grep -m1 '^FAIL' "$OUT/break$b.out" | cut -c1-110))"
done
# the real np_fps.cpp: the [speed] lines of a synthetic match (100 % then 90 % play, a replay, a load), and the switch off
$CXX -std=c++11 -O1 -DNP_NATIVE=1 -Iinclude -Iplatform/compat tools/fps/speed_log_unit.cpp -o "$OUT/speed_log_unit"
"$OUT/speed_log_unit" on 2> "$OUT/on.err" || true
grep '^\[speed\]' "$OUT/on.err" > "$OUT/on.speed" || true
n=$(wc -l < "$OUT/on.speed")
[ "$n" -ge 5 ] || { echo "speed log: FAILED (only $n [speed] lines)"; cat "$OUT/on.speed"; exit 1; }
grep -q '^\[speed\] play 5.0 s: 100.0 % ' "$OUT/on.speed" || grep -q '^\[speed\] play 5.0 s: 99.[5-9] % ' "$OUT/on.speed" || { echo "speed log: FAILED (no full-speed window)"; cat "$OUT/on.speed"; exit 1; }
grep -q '^\[speed\] play 5.0 s: 90.[0-9] % ' "$OUT/on.speed" || { echo "speed log: FAILED (no 90 % window)"; cat "$OUT/on.speed"; exit 1; }
grep -q '^\[speed\] match #1 end: ' "$OUT/on.speed" || { echo "speed log: FAILED (no match end line)"; cat "$OUT/on.speed"; exit 1; }
"$OUT/speed_log_unit" off 2> "$OUT/off.err" || true
if grep -q '^\[speed\]' "$OUT/off.err"; then echo "speed log: FAILED (MSC_SPEED_LOG=0 still prints)"; exit 1; fi
echo "speed log: $n lines with the switch on, none with MSC_SPEED_LOG=0"
echo "speed meter: passed"
