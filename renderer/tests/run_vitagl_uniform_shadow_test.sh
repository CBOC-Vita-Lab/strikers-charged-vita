#!/bin/bash
# 1.3 MSC_VGX_UNI_SHADOW (vitaGL patch 0024): vglGetUniformShadow / vglUniformShadowWritten and the code they stand in for (vgl_fill_uniform_data,
# vglSetUniformData, upload_uniforms with the uni_reuse cache of 0018) are taken from dependencies/vitaGL/source (not copies), over a model of the program
# tables and sceGxm. A random stream of program switches, F32 vec4 / vec4-array / mat4 writes, scene ends and ring wraps is played twice: through glUniform4fv's
# path, and through the shadow addresses plus one vglUniformShadowWritten per draw. At every draw the bytes the GPU would read from both default uniform buffers
# must be the same. BREAK=1|2|3 makes the test fail on purpose (no changed flag; no dirty flag; a fragment write flagged as vertex).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${CUSTOM_SHADERS_C:-$VGX/../dependencies/vitaGL/source/custom_shaders.c}
UT=$(dirname "$SRC")/utils/gxm_utils.c
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$SRC" "$UT" "$BUILD" <<'PY'
import sys
src = open(sys.argv[1]).read()
ut = open(sys.argv[2]).read()
def macro(text, start):
    i = text.index(start); j = i
    while True:
        e = text.index('\n', j)
        if text[e - 1] != '\\': return text[i:e + 1]
        j = e + 1
def func(text, sig):
    i = text.index(sig); j = text.index('{', i); d = 0; k = j
    while True:
        if text[k] == '{': d += 1
        elif text[k] == '}':
            d -= 1
            if d == 0: return text[i:k + 1] + '\n'
        k += 1
up = src[src.index('#ifndef HAVE_FFP_SHADER_SUPPORT\n'):]
up = up[up.index('#define upload_uniforms()'):]
up = up[:up.index('#else')]
out = func(ut, 'static inline __attribute__((always_inline)) void vgl_uniform_copy(')
out += func(ut, 'void vglSetUniformData(')
out += up + macro(src, '#define vgl_fill_uniform_data(p, type, size, count)')
out += func(src, 'GLboolean vglGetUniformShadow(') + func(src, 'void vglUniformShadowWritten(')
open(sys.argv[3] + '/fixture.inc', 'w').write(out)
PY
${CC:-gcc} -std=gnu11 -O1 -g -w -fsanitize=address,undefined ${BREAK:+-DBREAK=$BREAK} -I"$BUILD" "$VGX/tests/vitagl_uniform_shadow.c" -o "$BUILD/vitagl_uniform_shadow"
"$BUILD/vitagl_uniform_shadow"
