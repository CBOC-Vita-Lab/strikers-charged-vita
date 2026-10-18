#!/bin/bash
# W5 SEED: MSC_VGX_MAXPROGRAMS (program eviction, warm-up cap) on the real GL back end over the stub GL and a model of vitaGL's program table.
#   tests/run_program_evict_test.sh        (a few seconds, no GL driver, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/home/claude/msc}
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$MSC/deps/xxHash")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
SRC=("$VGX/tests/program_evict.cpp" "$VGX/src/vgx_shadergen.cpp")
"$CXX" "${FLAGS[@]}" -I"$VGX/src/glstub" "${SRC[@]}" -o "$BUILD/program_evict"
"$CXX" "${FLAGS[@]}" -D__VGX_VITA__ -I"$VGX/src/glstub" "${SRC[@]}" -o "$BUILD/program_evict_vita"
echo "--- no cap: the whole catalogue (1054 programs) at once runs the table out; the no-free-slot net takes over"
"$BUILD/program_evict" 0
echo "--- cap 200 (a low cap, the catalogue warm-up and six working sets)"
"$BUILD/program_evict" 200
echo "--- cap 100 (below one working set of 176: draws still never see a dead program)"
"$BUILD/program_evict" 100 1054 4400
echo "--- Vita build, default cap (900), catalogue warm-up"
env -u MSC_VGX_MAXPROGRAMS "$BUILD/program_evict_vita" default
echo "program eviction tests: all passed"
