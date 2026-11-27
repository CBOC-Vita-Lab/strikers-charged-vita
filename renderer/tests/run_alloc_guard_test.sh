#!/bin/bash
# 1.2 MSC_VGX_ALLOC_GUARD: the GL back end over the stub GL with a driver that runs out of names and memory (see alloc_guard.cpp).
#   tests/run_alloc_guard_test.sh        (a few seconds, no GL driver, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/work/msc}
XXH="$MSC/deps/xxHash"; [[ -f "$XXH/xxhash.h" ]] || XXH="$(cd "$(dirname "$0")/.." && pwd)/third_party"   # (no deps/xxHash on this box: the repo copy)
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$XXH")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
SRC=("$VGX/tests/alloc_guard.cpp" "$VGX/src/vgx_shadergen.cpp")
"$CXX" "${FLAGS[@]}" -I"$VGX/src/glstub" "${SRC[@]}" -o "$BUILD/alloc_guard"
"$CXX" "${FLAGS[@]}" -D__VGX_VITA__ -I"$VGX/src/glstub" "${SRC[@]}" -o "$BUILD/alloc_guard_vita"
for g in 1 0; do
    echo "--- host build, guard $g"
    "$BUILD/alloc_guard" "$g"
    echo "--- Vita build, guard $g"
    "$BUILD/alloc_guard_vita" "$g"
done
echo "allocation guard tests: all passed"
