#!/bin/bash
# 1.3 vglSetDrawCaches: the macros of dependencies/vitaGL/source/custom_shaders.c over a model of sceGxm (a second, no console).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${CUSTOM_SHADERS_C:-$VGX/../dependencies/vitaGL/source/custom_shaders.c}
GXM=$(dirname "$SRC")/gxm.c
UT=$(dirname "$SRC")/utils/gxm_utils.c
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$SRC" "$BUILD" "$GXM" "$UT" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
def macro(start):
    i = src.index(start)
    j = i
    while True:
        e = src.index('\n', j)
        if src[e - 1] != '\\': return src[i:e + 1]
        j = e + 1
def func(sig):
    i = src.index(sig); j = src.index('{', i); d = 0; k = j
    while True:
        if src[k] == '{': d += 1
        elif src[k] == '}':
            d -= 1
            if d == 0: return src[i:k + 1] + '\n'
        k += 1
up = src[src.index('#ifndef HAVE_FFP_SHADER_SUPPORT\n'):]
up = up[up.index('#define upload_uniforms()'):]
e = up.index('#else')
text = up[:e] + macro('#define setup_frag_program_tx(failed)') + macro('#define setup_frag_program()') + macro('#define vgl_fill_uniform_data(p, type, size, count)')
text += func('static inline __attribute__((always_inline)) int uni_hist_eq(') + func('static inline __attribute__((always_inline)) void *uni_hist_find(') + func('static inline __attribute__((always_inline)) void uni_hist_add(') + func('static inline __attribute__((always_inline)) void uni_hist_free(')
text += func('static inline __attribute__((always_inline)) SceGxmFragmentProgram *frag_memo_find(') + func('static inline __attribute__((always_inline)) void frag_memo_add(') + func('static inline __attribute__((always_inline)) void program_caches_reset(')
assert '#define VGL_UNI_HIST 4\n' in src and '#define VGL_UNI_HIST_BUDGET (1536u * 1024u)' in src, 'the model\'s constants differ from the source'
open(sys.argv[2] + '/fixture.inc', 'w').write(text)
# the mutant: the history compares only half the buffer (the test must see a different picture)
assert 'uint32_t w = n >> 2;' in text
open(sys.argv[2] + '/fixture_mutant.inc', 'w').write(text.replace('uint32_t w = n >> 2;', 'uint32_t w = n >> 3;'))
# what the model cannot see: every program start resets the caches, every value write marks, the generation moves at scene end and at a wrap
for name in ('GLuint glCreateProgram(', 'static void link_program_untimed('):
    assert 'program_caches_reset(' in func(name), name
fill = macro('#define vgl_fill_uniform_data(p, type, size, count)')
assert fill.count('u->changed[0] = 1') == 3 and fill.count('u->changed[1] = 1') == 3
assert len(re.findall(r'__builtin_memcpy\(u->|vglSetUniformData\(u->', src)) == 6, 'a write of a value outside vgl_fill_uniform_data'
assert src.count('u->changed = p->unif_changed') == 2
gxm = open(sys.argv[3]).read()
se = gxm[gxm.index('void scene_end(void)'):]; se = se[:se.index('\n}\n')]
assert 'vgl_unif_gen++' in se
ut = open(sys.argv[4]).read()
fn = ut[ut.index('void *vglReserveUniformCircularPoolBuffer('):]; fn = fn[:fn.index('\n}\n')]
assert 'vgl_unif_gen++' in fn
PY
${CC:-clang} -std=gnu11 -O1 -g -w -fsanitize=address,undefined -I"$BUILD" "$VGX/tests/vitagl_draw_caches.c" -o "$BUILD/vitagl_draw_caches"
"$BUILD/vitagl_draw_caches"
# the mutant must be caught (an equality test that ignores half of the bytes names a wrong copy)
mkdir -p "$BUILD/m" && cp "$BUILD/fixture_mutant.inc" "$BUILD/m/fixture.inc"
${CC:-clang} -std=gnu11 -O1 -g -w -fsanitize=address,undefined -I"$BUILD/m" "$VGX/tests/vitagl_draw_caches.c" -o "$BUILD/vitagl_draw_caches_mutant"
if "$BUILD/vitagl_draw_caches_mutant" >/dev/null 2>&1; then echo "MUTANT NOT CAUGHT: a half-buffer compare went unseen"; exit 1; fi
echo "vitaGL draw caches: the half-buffer-compare mutant is caught"
