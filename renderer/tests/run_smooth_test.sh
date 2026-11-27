#!/bin/bash
# gxvita14 (wave 14): the smoothness score (vgx_smooth.h). BREAK=1..3 must fail: 1 judder weighs 2.5, 2 hold 0 counts as a picture, 3 only the lower cadence hold is ideal.
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-smooth-test}
mkdir -p "$BUILD"
CXX=${CXX:-clang++}
if [[ -n ${BREAK:-} ]]; then
    "$CXX" -std=c++17 -O1 -Wall -Wextra -DVGX_SMOOTH_BREAK="$BREAK" "$VGX/tests/smooth.cpp" -o "$BUILD/smooth_break$BREAK"
    if "$BUILD/smooth_break$BREAK" > "$BUILD/break$BREAK.out" 2>&1; then echo "smooth: FAILED (BREAK=$BREAK passed)"; exit 1; fi
    grep FAIL "$BUILD/break$BREAK.out" | head -3
    echo "smooth: BREAK=$BREAK is caught"
    exit 0
fi
"$CXX" -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/smooth.cpp" -o "$BUILD/smooth"
"$BUILD/smooth"
