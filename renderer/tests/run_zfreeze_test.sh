#!/bin/bash
# Z-freeze (MSC_VGX_ZFREEZE): the plane arithmetic (src/vgx_zfreeze.h, no GL, a few seconds) and the generated shader on the real
# GL back end (tests/zfreeze_gl.cpp: Mesa llvmpipe through EGL, at three render-target sizes: 1:1, the Vita's 960x544, 2x).
#   tests/run_zfreeze_test.sh            both        ZFREEZE_GL=0 tests/run_zfreeze_test.sh   arithmetic only
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/work/msc}
XXH="$MSC/deps/xxHash"; [[ -f "$XXH/xxhash.h" ]] || XXH="$(cd "$(dirname "$0")/.." && pwd)/third_party"   # (no deps/xxHash on this box: the repo copy)
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
mkdir -p "$BUILD"
CXX=${CXX:-clang++}
$CXX -std=c++20 -O2 -Wall -Wextra -I"$VGX/src" "$VGX/tests/zfreeze.cpp" -o "$BUILD/zfreeze"
"$BUILD/zfreeze"
if [ "${ZFREEZE_GL:-1}" = 1 ]; then
    GLLIBS=(-lEGL -lGLESv2)   # (a box with the runtime libraries but no -dev symlinks: link the sonames)
    echo 'int main(){}' | $CXX -x c++ - -lEGL -lGLESv2 -o "$BUILD/gl_probe" 2>/dev/null || GLLIBS=(-l:libEGL.so.1 -l:libGLESv2.so.2)
    $CXX -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$XXH" \
        "$VGX/tests/zfreeze_gl.cpp" "$VGX/src/vgx_shadergen.cpp" "$VGX/src/vgx_host_egl.cpp" -o "$BUILD/zfreeze_gl" "${GLLIBS[@]}"
    for size in 640x528 960x544 1280x1056; do "$BUILD/zfreeze_gl" $size; done
fi
echo "zfreeze tests: all passed"
