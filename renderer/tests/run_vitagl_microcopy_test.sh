#!/bin/bash
# vitaGL 0047 (vgl_fast_memcmp, upstream 6647b18) and 0048 (vgl_uniform_copy, upstream 53cc00f): the functions are taken from dependencies/vitaGL/source/utils/gxm_utils.c
# (not copies) and compared with memcmp / memcpy on random data, sizes 0..99, mixed alignment. BREAK=1 compares one byte too few for 8, BREAK=2 copies one byte too few for 16
# (the test must fail).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
UT=${GXM_UTILS_C:-$VGX/../dependencies/vitaGL/source/utils/gxm_utils.c}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$UT" "$BUILD" "${BREAK:-0}" <<'PY'
import sys
ut = open(sys.argv[1]).read(); brk = sys.argv[3]
def func(text, sig):
    i = text.index(sig); j = text.index('{', i); d = 0; k = j
    while True:
        if text[k] == '{': d += 1
        elif text[k] == '}':
            d -= 1
            if d == 0: return text[i:k + 1] + '\n'
        k += 1
a = func(ut, 'static inline __attribute__((always_inline)) GLboolean vgl_fast_memcmp(')
b = func(ut, 'static inline __attribute__((always_inline)) void vgl_uniform_copy(')
if brk == '1': a = a.replace('x[0] == y[0] && x[1] == y[1];', 'x[0] == y[0];', 1)
if brk == '2': b = b.replace('__builtin_memcpy(dst, src, 16);', '__builtin_memcpy(dst, src, 15);', 1)
open(sys.argv[2] + '/fixture.inc', 'w').write(a + b)
PY
${CC:-gcc} -std=gnu11 -O1 -g -w -fsanitize=address,undefined -I"$BUILD" "$VGX/tests/vitagl_microcopy.c" -o "$BUILD/vitagl_microcopy"
"$BUILD/vitagl_microcopy"
