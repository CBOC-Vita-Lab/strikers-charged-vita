#!/bin/bash
# 1.2 MSC_VGX_DLCACHE_BOUNDS_GUARD: the real display-list parser, cache and emitter (see dlcache_bounds.cpp).
#   tests/run_dlcache_bounds_test.sh        (a few seconds, no GL driver, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/work/msc}
XXH="$MSC/deps/xxHash"; [[ -f "$XXH/xxhash.h" ]] || XXH="$(cd "$(dirname "$0")/.." && pwd)/third_party"   # (no deps/xxHash on this box: the repo copy)
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$XXH")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
SRC=("$VGX/tests/dlcache_bounds.cpp")
for f in vgx_state vgx_dlcache vgx_draw vgx_rec vgx_play vgx_present vgx_guestmem vgx_texcache vgx_texdecode vgx_hdtex vgx_skin vgx_stats vgx_shadergen vgx_capture vgx_interp vgx_backend_null; do SRC+=("$VGX/src/$f.cpp"); done
"$CXX" "${FLAGS[@]}" "${SRC[@]}" -o "$BUILD/dlcache_bounds"
echo "--- guard on (default)"
"$BUILD/dlcache_bounds" on
echo "--- guard off (control)"
"$BUILD/dlcache_bounds" off
echo "display-list cache bounds tests: all passed"
