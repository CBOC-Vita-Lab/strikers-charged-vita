#!/bin/bash
# vitaGL 0043's per-kind slot table (rt_kinds.h): 17 sizes -> 16 slots + "other"; and the gpuscene20 header's lines.   native/tools/vitagl/run_rt_kinds_test.sh
set -e
D="$(cd "$(dirname "$0")" && pwd)"
OUT=${1:-$(mktemp -d)}
cc -O2 -Wall -I"$D/../../../dependencies/vitaGL/source" "$D/rt_kinds_test.c" -o "$OUT/rt_kinds_test"
"$OUT/rt_kinds_test"
c++ -O1 -Wall -std=c++17 "$D/gpuscene20_test.cpp" -o "$OUT/gpuscene20_test"
"$OUT/gpuscene20_test"
