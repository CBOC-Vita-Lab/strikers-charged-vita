#!/bin/bash
# 1.2 vitaGL scene entry: init_depth_stencil_buffer / scene_reset / scene_reset_failed from dependencies/vitaGL/source/gxm.c over a model of sceGxm.
#   tests/run_vitagl_scene_entry_test.sh        (a second, no console, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
GXM=${GXM_C:-$VGX/../dependencies/vitaGL/source/gxm.c}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
CC=${CC:-clang}
python3 - "$GXM" "$BUILD/scene_fixture.inc" <<'PY'
import sys
src = open(sys.argv[1]).read()
def func(sig):
    i = src.index(sig)
    j = src.index('{', i)
    depth, k = 0, j
    while True:
        c = src[k]
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: break
        k += 1
    return src[i:k + 1]
parts = [func('GLboolean init_depth_stencil_buffer_ms('), func('GLboolean init_depth_stencil_buffer('),
         func('static GLboolean scene_reset_failed(void)'), func('GLboolean scene_reset(void)')]
# scene_end is the platform's end-of-scene call (sceGxmEndScene / the GPU timers' variant plus the vblank wait): the model counts it
text = '#define scene_end() (++ends)\n' + '\n\n'.join(parts)
open(sys.argv[2], 'w').write(text)
PY
SAN=(); if [[ ${SANITIZE:-0} == 1 ]]; then SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CC" -std=gnu11 -O1 -g "${SAN[@]}" -w -I"$BUILD" "$VGX/tests/vitagl_scene_entry.c" -o "$BUILD/vitagl_scene_entry"
"$BUILD/vitagl_scene_entry"
echo "vitaGL scene entry tests: all passed"
