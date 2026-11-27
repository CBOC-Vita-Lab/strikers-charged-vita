#!/bin/bash
# 1.3 MSC_VGX_PALMERGE: palette runs on the real front end (tests/palmerge.cpp) and the 60-row palette vertex shader (tests/palbucket.cpp).
# The draws of four frames executed on the CPU as the palette vertex shader reads its matrices must be the same, byte for byte, with the switch off,
# on, on with 30 rows only, on with no room for buffers (every run drawn member by member), and auto with and without 60-row keys in the warm-up list;
# with it on there must be fewer records.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/palmerge}
CXX=${CXX:-g++}
mkdir -p "$BUILD/obj"
SAN=()
if [[ ${SANITIZE:-0} == 1 ]]; then SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
FLAGS=(-std=c++20 -O0 -g -w -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party" "${SAN[@]}")
"$CXX" -std=c++20 -O1 -DHAVE_PALROWS -I"$VGX/src" -I"$VGX/third_party" "$VGX/tests/palbucket.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/palbucket"
"$BUILD/palbucket"
# (the front end's files without the display-list cache, which the test replaces, and the two that need the game's headers)
for f in vgx_rec vgx_play vgx_guestmem vgx_state vgx_shadergen vgx_texdecode vgx_texcache vgx_stats vgx_interp vgx_present vgx_capture vgx_skin vgx_hdtex vgx_backend_null; do
    "$CXX" "${FLAGS[@]}" -c "$VGX/src/$f.cpp" -o "$BUILD/obj/$f.o"
done
"$CXX" "${FLAGS[@]}" "$VGX/tests/palmerge.cpp" "$BUILD"/obj/*.o -x c "$VGX/src/vgx_host_stubs.c" -o "$BUILD/palmerge" -lpthread -ldl
T=$BUILD/palmerge
MSC_VGX_PALMERGE=0 "$T" "$BUILD/off.bin" | tee "$BUILD/off.log"
MSC_VGX_PALMERGE=1 MSC_VGX_PALMERGE_CHECK=1 "$T" "$BUILD/on.bin" 2>&1 | tee "$BUILD/on.log"
MSC_VGX_PALMERGE=1 MSC_VGX_PALMERGE_ROWS=30 "$T" "$BUILD/r30.bin" | tee "$BUILD/r30.log"
MSC_VGX_PALMERGE=1 MSC_VGX_PALMERGE_MB=0 "$T" "$BUILD/cap.bin" | tee "$BUILD/cap.log"
# auto (the default): off while the warm-up list has no 60-row key; with the list a run with the switch on wrote, the same runs as on
rm -f "$BUILD/list.bin"
MSC_VGX_PALMERGE=1 MSC_VGX_SHADERLIST="$BUILD/list.bin" "$T" "$BUILD/onlist.bin" > /dev/null
"$T" "$BUILD/auto0.bin" 2>&1 | tee "$BUILD/auto0.log"
MSC_VGX_SHADERLIST="$BUILD/list.bin" "$T" "$BUILD/auto1.bin" 2>&1 | tee "$BUILD/auto1.log"
cmp "$BUILD/off.bin" "$BUILD/on.bin"
cmp "$BUILD/off.bin" "$BUILD/auto0.bin"
cmp "$BUILD/off.bin" "$BUILD/auto1.bin"
grep -q 'records per frame: 11 11 11 11' "$BUILD/auto0.log"
grep -q 'palette runs off' "$BUILD/auto0.log"
grep -q 'records per frame: 5 5 5 5' "$BUILD/auto1.log"
grep -q 'uploads 7' "$BUILD/auto1.log"
cmp "$BUILD/off.bin" "$BUILD/r30.bin"
cmp "$BUILD/off.bin" "$BUILD/cap.bin"
grep -q 'records per frame: 11 11 11 11' "$BUILD/off.log"
grep -q 'records per frame: 5 5 5 5' "$BUILD/on.log"          # five runs a frame
grep -q 'uploads 7' "$BUILD/on.log"                           # four buffers made in frame 1, found in frame 2, three made again after their member went
! grep -q 'CHECK FAILED' "$BUILD/on.log"
grep -q 'records per frame: 8 8 8 8' "$BUILD/r30.log"
grep -q 'records per frame: 11 11 11 11' "$BUILD/cap.log"     # no room: every member drawn on its own, as without the switch
echo "palmerge tests: all passed"
