#!/bin/bash
# Wave 9 (pacing): the MSC_PACE_LOG window figures (vgx_pacelog.h). BREAK=1..3 must fail: 1 the id gap is counted from the first id, 2 the fold bins by picture % 599, 3 an id shown twice is a gap.
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-pacelog-test}
mkdir -p "$BUILD"
CXX=${CXX:-clang++}
if [[ -n ${BREAK:-} ]]; then
    "$CXX" -std=c++17 -O1 -Wall -Wextra -DVGX_PACELOG_BREAK="$BREAK" "$VGX/tests/pacelog.cpp" -o "$BUILD/pacelog_break$BREAK"
    if "$BUILD/pacelog_break$BREAK" > "$BUILD/break$BREAK.out" 2>&1; then echo "pacelog: FAILED (BREAK=$BREAK passed)"; exit 1; fi
    grep FAIL "$BUILD/break$BREAK.out" | head -3
    echo "pacelog: BREAK=$BREAK is caught"
    exit 0
fi
"$CXX" -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/pacelog.cpp" -o "$BUILD/pacelog"
"$BUILD/pacelog"
