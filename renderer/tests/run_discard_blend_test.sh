#!/bin/bash
# MSC_VGX_DISCARD_BLEND: the blend algebra over every GX blend / z / destination-alpha combination, and the text (tests/discard_blend.cpp).
# BREAK=1: a generator that loses the alpha test in the no-discard form must fail.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/discard-blend}
CXX=${CXX:-g++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
if [[ ${BREAK:-0} == 1 ]]; then
    "$CXX" "${FLAGS[@]}" -DVGX_DISCARD_BLEND_BREAK "$VGX/tests/discard_blend.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/discard_blend_break"
    if "$BUILD/discard_blend_break" > "$BUILD/break.out" 2>&1; then echo "discard_blend: FAILED (the generator without the test passed)"; exit 1; fi
    tail -2 "$BUILD/break.out"
    echo "discard_blend: the break is caught"
    exit 0
fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/discard_blend.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/discard_blend"
"$BUILD/discard_blend" 2>/dev/null
echo "discard_blend: PASS"
