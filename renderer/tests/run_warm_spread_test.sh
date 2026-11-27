#!/bin/bash
# wave 10 (warm10): the shader warm-up's heap rule and spread queue (tests/warm_spread.cpp, vgx_warmspread.h), then the real back end's guard over the stub GL (picture_half.cpp).
# BREAK=1|2: a rule broken on purpose (1 the explicit absolute figure ignored, 2 the floor halved): must fail.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/warm-spread}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -I"$VGX/src")
if [[ -n ${BREAK:-} ]]; then
    "$CXX" "${FLAGS[@]}" -DVGX_WARM_BREAK="$BREAK" "$VGX/tests/warm_spread.cpp" -o "$BUILD/warm_spread_break$BREAK"
    if "$BUILD/warm_spread_break$BREAK" > "$BUILD/break$BREAK.out" 2>&1; then echo "warm_spread: FAILED (BREAK=$BREAK passed)"; exit 1; fi
    grep FAIL "$BUILD/break$BREAK.out" | head -3
    echo "warm_spread: BREAK=$BREAK is caught"
    exit 0
fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/warm_spread.cpp" -o "$BUILD/warm_spread"
"$BUILD/warm_spread"
