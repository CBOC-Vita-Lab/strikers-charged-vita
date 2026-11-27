#!/bin/bash
# 1.3 MSC_VGX_FOGVTX: the eye-depth form of GX fog against the per-pixel one (tests/fogvtx.cpp), the deliberate break, and the generated texts (tests/fogvtx_text.cpp).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/fogvtx}
CXX=${CXX:-g++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/fogvtx.cpp" -o "$BUILD/fogvtx"
for m in 1 2; do echo "== MSC_VGX_FOGVTX=$m"; "$BUILD/fogvtx" $m; ! "$BUILD/fogvtx" mutate $m >/dev/null 2>&1; done
"$CXX" "${FLAGS[@]}" "$VGX/tests/fogvtx_text.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/fogvtx_text"
"$BUILD/fogvtx_text"
echo "fogvtx: PASS"
