#!/bin/bash
# 1.3 MSC_VGX_GXM_DIRECT and MSC_FX_LOWRES_RUNS on the real GL back end (Vita build) over the stub GL.
#   tests/run_gxm_direct_test.sh        (a few seconds, no GL driver, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$(mktemp -d)}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -D__VGX_VITA__ -I"$VGX/src" -I"$VGX/third_party" -I"$VGX/src/glstub")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/gxm_direct.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/gxm_direct"
MSC_VGX_GXM_DIRECT=1 "$BUILD/gxm_direct" on 2>/dev/null
MSC_VGX_GXM_DIRECT=0 "$BUILD/gxm_direct" off 2>/dev/null
FX_WANT=1111 "$BUILD/gxm_direct" fxruns 2>/dev/null
FX_WANT=0101 MSC_FX_LOWRES_RUNS=2 "$BUILD/gxm_direct" fxruns 2>/dev/null
FX_WANT=0100 MSC_FX_LOWRES_RUNS=1 "$BUILD/gxm_direct" fxruns 2>/dev/null
echo "GXM direct tests: all passed"
