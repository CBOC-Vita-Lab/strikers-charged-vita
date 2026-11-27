#!/bin/bash
# Bounded slip (MSC_SLIP, np_fps.cpp NpSlipDelta) on a model of the 30 fps frame loop around the real np_fps.cpp: off changes nothing, on never lets the
# game fall below 83 % of real time, never costs pictures, and does nothing in a LAN session.   tools/fps/run_slip_test.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-${BUILD:-/tmp/np_slip_test}}
mkdir -p "$OUT"
clang++ -std=c++11 -O1 -DNP_NATIVE=1 -Iinclude -Iplatform/compat tools/fps/slip_unit.cpp -o "$OUT/slip_unit"
"$OUT/slip_unit" 2>&1 | grep -v '^\[fps\]'
clang++ -std=c++11 -O1 -DNP_NATIVE=1 -Iinclude -Iplatform/compat tools/fps/load_fps_unit.cpp -o "$OUT/load_fps_unit"
"$OUT/load_fps_unit" 2>&1 | grep -v '^\[fps\]'
"$OUT/load_fps_unit" on 2>&1 | grep -v '^\[fps\]'
# nxpace25: the [load] line's clock and its mutants (each must fail)
for m in 1 2 3; do
    clang++ -std=c++11 -O1 -DNP_NATIVE=1 -DNP_LOADTR_BREAK=$m -Iinclude -Iplatform/compat tools/fps/load_fps_unit.cpp -o "$OUT/load_fps_unit_break"
    if "$OUT/load_fps_unit_break" >/dev/null 2>&1; then echo "load clock mutant $m NOT caught"; exit 1; fi
    echo "load_fps_unit: load clock mutant $m is caught"
done
