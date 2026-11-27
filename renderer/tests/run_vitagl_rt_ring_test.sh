#!/bin/bash
# 1.2 vitaGL GPU timer ring guard: the driver's own GPU-timer code (extracted from dependencies/vitaGL/source/rt.c) over a model of sceGxm.
#   tests/run_vitagl_rt_ring_test.sh        (a second, no console, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
RT=${RT_C:-$VGX/../dependencies/vitaGL/source/rt.c}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
CC=${CC:-clang}
python3 - "$RT" "$BUILD/rt_gpu_fixture.inc" <<'PY'
import sys
src = open(sys.argv[1]).read()
def between(a, b):
    i = src.index(a)
    j = src.index(b, i)
    return src[i:j]
# the ring, the guard, the scene end, the frame end, the watcher, the set-up, the progress and the newest-frame read (not the depth/stencil half, not the stats copy)
text = between('#define RT_FRAMES 128', 'uint32_t vgl_rt_ds_bytes(') + between('uint32_t vgl_rt_now(void)', 'void vglRtGetStats(')
open(sys.argv[2], 'w').write(text)
PY
SAN=(); if [[ ${SANITIZE:-0} == 1 ]]; then SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CC" -std=c11 -O1 -g "${SAN[@]}" -w -I"$BUILD" -I"$VGX/../dependencies/vitaGL/source" "$VGX/tests/vitagl_rt_ring.c" -o "$BUILD/vitagl_rt_ring"
for n in 0 1 47 48 255 256 257 300 1024; do
    "$BUILD/vitagl_rt_ring" "$n" 1
    "$BUILD/vitagl_rt_ring" "$n" 0
done
echo "vitaGL timer ring tests: all passed"
