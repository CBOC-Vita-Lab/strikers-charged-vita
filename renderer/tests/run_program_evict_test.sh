#!/bin/bash
# W5 SEED: MSC_VGX_MAXPROGRAMS (program eviction, warm-up cap) on the real GL back end over the stub GL and a model of vitaGL's program table.
#   tests/run_program_evict_test.sh        (a few seconds, no GL driver, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/work/msc}
XXH="$MSC/deps/xxHash"; [[ -f "$XXH/xxhash.h" ]] || XXH="$(cd "$(dirname "$0")/.." && pwd)/third_party"   # (no deps/xxHash on this box: the repo copy)
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$XXH" -I"$VGX/third_party")
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
echo "--- 1.3 MSC_VGX_RECLAIM_QUIET: no housekeeping deletes in a match, on the Vita build"
for scene in 0 1 2 3; do env -u MSC_VGX_RECLAIM_QUIET "$BUILD/program_evict_vita" quiet $scene; done
MSC_VGX_RECLAIM_QUIET=0 "$BUILD/program_evict_vita" quiet 1
echo "--- 1.3: a warm-up that fills the table does not block the next frame's draws"
"$BUILD/program_evict_vita" full
echo "--- 1.3 (wave 8): the per-match ledger prints a start and an end line and the switches' DID NOTHING list"
"$BUILD/program_evict" ledger 2> "$BUILD/ledger.err"
grep -q '^\[match\] #1 start' "$BUILD/ledger.err" && grep -q '^\[match\] #1 end (back in the menus' "$BUILD/ledger.err" && grep -q '^\[acts-match\] #1 DID NOTHING.*MSC_VGX_ATLATE' "$BUILD/ledger.err" || { echo "FAIL: ledger lines"; cat "$BUILD/ledger.err"; exit 1; }
grep -E '^\[match\]|^\[acts-match\]' "$BUILD/ledger.err" | cut -c1-400
echo "program eviction tests: all passed"
