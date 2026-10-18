#!/bin/bash
# Codex's LA88 texture tests (decoder, cache and the GL back end over the stub GL), adapted to this tree.
#   tests/run_texture_tests.sh            plain build
#   SANITIZE=1 tests/run_texture_tests.sh with AddressSanitizer + UBSan
# Output goes to $TEST_BUILD (default build/tests of this checkout's parent build dir). xxhash.h comes from deps/xxHash.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/home/claude/msc}
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$MSC/deps/xxHash")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" -I"$VGX/src/glstub" "$VGX/tests/texture_gl.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/texture_gl"
"$BUILD/texture_gl"
"$CXX" "${FLAGS[@]}" -D__VGX_VITA__ -I"$VGX/src/glstub" "$VGX/tests/texture_gl.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/texture_gl_vita"
"$BUILD/texture_gl_vita"
"$CXX" "${FLAGS[@]}" "$VGX/tests/texture_la88.cpp" "$VGX/src/vgx_texdecode.cpp" "$VGX/src/vgx_texcache.cpp" "$VGX/src/vgx_guestmem.cpp" "$VGX/src/vgx_backend_null.cpp" -o "$BUILD/texture_la88"
env -u MSC_VGX_LA88 MSC_VGX_DXT1=0 "$BUILD/texture_la88" default
MSC_VGX_LA88=0 MSC_VGX_DXT1=0 "$BUILD/texture_la88" default
MSC_VGX_LA88=1 MSC_VGX_DXT1=0 "$BUILD/texture_la88" compact
MSC_VGX_LA88=1 MSC_VGX_DXT1=0 MSC_VGX_VITA=1 "$BUILD/texture_la88" compact
echo "texture tests: all passed"
