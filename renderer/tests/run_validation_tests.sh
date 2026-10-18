#!/bin/bash
# Codex's tests of MSC_VGX_VALIDATE (the CPU-side draw validator): the pure validator (normal and -ffast-math) and the real
# GL back end over the stub GL (desktop and Vita preprocessing; off / valid / bad / missing / late-selected fixtures).
#   tests/run_validation_tests.sh     SANITIZE=1 CXX=g++ tests/run_validation_tests.sh
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/home/claude/msc}
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$MSC/deps/xxHash")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/validate.cpp" -o "$BUILD/validate"
"$BUILD/validate"
"$CXX" "${FLAGS[@]}" -ffast-math "$VGX/tests/validate.cpp" -o "$BUILD/validate_fastmath"
"$BUILD/validate_fastmath"
for platform in desktop vita; do
    BUILD_FLAGS=("${FLAGS[@]}")
    if [[ $platform == vita ]]; then BUILD_FLAGS+=(-D__VGX_VITA__); fi
    "${CXX}" "${BUILD_FLAGS[@]}" -I"$VGX/src/glstub" \
        "$VGX/tests/validate_gl.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/validate_gl_$platform"
    for mode in off valid bad missing late; do "$BUILD/validate_gl_$platform" "$mode"; done
done
echo "validation tests: all passed"
