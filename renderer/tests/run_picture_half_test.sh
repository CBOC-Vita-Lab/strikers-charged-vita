#!/bin/bash
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BASE=${VGX_BASE:-$VGX}
BUILD=${TEST_BUILD:-$BASE/build/tests/picture-half}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
awk '/^extern "C" int vgx_vita_load_packed_shader\(/,/^}/' "$VGX/vita/vgx_host_vita.cpp" > "$BUILD/picture_pack_loader.inc"
test -s "$BUILD/picture_pack_loader.inc"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$BASE/src" -I"$BASE/src/glstub" -I"$BASE/third_party" -I"$BUILD")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
SRC=("$VGX/tests/picture_half.cpp" "$BASE/src/vgx_shadergen.cpp")
"$CXX" "${FLAGS[@]}" "${SRC[@]}" -o "$BUILD/picture_half_host"
"$BUILD/picture_half_host"
"$CXX" "${FLAGS[@]}" -D__VGX_VITA__ "${SRC[@]}" -o "$BUILD/picture_half_vita"
"$BUILD/picture_half_vita"
