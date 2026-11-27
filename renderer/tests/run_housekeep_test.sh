#!/bin/bash
# 1.3 (wave 8): the sweep back-off ladder and the trim-and-remake guard (vgx_housekeep.h). BREAK=1..4 must fail: 1 the gap never grows past two periods, 2 the gap never comes back,
# 3 any remake pins, 4 nothing is pinned.
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-housekeep-test}
mkdir -p "$BUILD"
CXX=${CXX:-clang++}
if [[ -n ${BREAK:-} ]]; then
    "$CXX" -std=c++20 -O1 -Wall -Wextra -DVGX_HK_BREAK="$BREAK" "$VGX/tests/housekeep.cpp" -o "$BUILD/housekeep_break$BREAK"
    if "$BUILD/housekeep_break$BREAK" > "$BUILD/break$BREAK.out" 2>&1; then echo "housekeep: FAILED (BREAK=$BREAK passed)"; exit 1; fi
    grep FAIL "$BUILD/break$BREAK.out" | head -3
    echo "housekeep: BREAK=$BREAK is caught"
    exit 0
fi
"$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/housekeep.cpp" -o "$BUILD/housekeep"
"$BUILD/housekeep"
