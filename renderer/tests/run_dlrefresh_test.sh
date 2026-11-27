#!/bin/bash
# MSC_VGX_DLREFRESH: the real display-list parser, cache, decoder and emitter (see dlrefresh.cpp).
#   tests/run_dlrefresh_test.sh        (a minute to build, no GL driver, no game data; CXX=g++ where clang's sanitizer runtime is missing)
# For each scenario: `on` must draw the full decode of every call and the same frames as `off` (the bypass of before); the mutant
# (MSC_VGX_DLREFRESH_BREAK=1: refreshes one vertex too few) must fail the comparison with the full decode and differ from `off`.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/work/msc}
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
XXH="$MSC/deps/xxHash"
[[ -f "$XXH/xxhash.h" ]] || XXH="$VGX/third_party"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$XXH")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
SRC=("$VGX/tests/dlrefresh.cpp")
for f in vgx_state vgx_dlcache vgx_draw vgx_rec vgx_play vgx_present vgx_guestmem vgx_texcache vgx_texdecode vgx_hdtex vgx_skin vgx_stats vgx_shadergen vgx_capture vgx_interp vgx_backend_null; do SRC+=("$VGX/src/$f.cpp"); done
"$CXX" "${FLAGS[@]}" "${SRC[@]}" -o "$BUILD/dlrefresh"
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
for scene in skin rigid soft; do
    echo "--- $scene: refresh on (default)"
    "$BUILD/dlrefresh" $scene on "$D/$scene.on"
    echo "--- $scene: refresh off (the bypass of before)"
    "$BUILD/dlrefresh" $scene off "$D/$scene.off"
    cmp "$D/$scene.on" "$D/$scene.off" || { echo "FAIL: $scene: frames differ between MSC_VGX_DLREFRESH=1 and =0"; exit 1; }
    echo "$scene: $(wc -l < "$D/$scene.on") frames identical with the refresh on and off"
    echo "--- $scene: mutant (one vertex of every primitive not refreshed)"
    rc=0
    "$BUILD/dlrefresh" $scene mutant "$D/$scene.mut" > "$D/$scene.mut.out" 2>&1 || rc=$?
    tail -n 3 "$D/$scene.mut.out"
    if cmp -s "$D/$scene.mut" "$D/$scene.off"; then echo "FAIL: $scene: the mutant's frames equal the reference: the comparison proves nothing"; exit 1; fi
    if [[ $scene != soft && $rc != 1 ]]; then echo "FAIL: $scene: the mutant passed the comparison with the full decode (exit $rc)"; exit 1; fi
    if [[ $rc == 3 ]]; then echo "FAIL: $scene: the mutant never refreshed"; exit 1; fi
    echo "$scene: mutant caught ($(cmp "$D/$scene.mut" "$D/$scene.off" 2>&1 | head -n 1 || true))"
done
echo "display-list refresh tests: all passed"
