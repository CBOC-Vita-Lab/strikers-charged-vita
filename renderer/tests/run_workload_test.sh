#!/bin/bash
# 1.3 the [lane] line, MSC_VGX_PROMOSOLO, MSC_VGX_DL_TINY. A few seconds, no GL driver, no game data.   SANITIZE=1: ASan + UBSan.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/msc_workload_test}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread -I"$VGX/src" -I"$VGX/third_party")
if [[ ${SANITIZE:-0} == 1 ]]; then
  FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  export ASAN_OPTIONS=detect_leaks=0   # (the cache's entries live until a stadium flush; these runs end without one, dlcache_bounds covers the teardown)
fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/workload.cpp" -o "$BUILD/workload"
"$CXX" "${FLAGS[@]}" -DNP_LOGGING=0 "$VGX/tests/workload.cpp" -o "$BUILD/workload-nolog"
"$BUILD/workload" thread "$BUILD/lane.log"
"$BUILD/workload" same "$BUILD/lane-same.log"
"$BUILD/workload" off "$BUILD/lane-off.log"
"$BUILD/workload-nolog" thread "$BUILD/lane-nolog.log"
SRC=("$VGX/tests/dl_promosolo.cpp")
for f in vgx_state vgx_dlcache vgx_draw vgx_rec vgx_play vgx_present vgx_guestmem vgx_texcache vgx_texdecode vgx_hdtex vgx_skin vgx_stats vgx_shadergen vgx_capture vgx_interp vgx_backend_null; do SRC+=("$VGX/src/$f.cpp"); done
"$CXX" "${FLAGS[@]}" "${SRC[@]}" -o "$BUILD/dl_promosolo"
for m in off on budget tiny; do "$BUILD/dl_promosolo" "$m"; done
echo "workload / promosolo tests: all passed"
