#!/bin/bash
# 1.3 vglDrawElementsDirect: the driver's functions from dependencies/vitaGL/source/custom_shaders.c over a model of sceGxm.
#   tests/run_vitagl_direct_draw_test.sh        (a second, no console, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${CUSTOM_SHADERS_C:-$VGX/../dependencies/vitaGL/source/custom_shaders.c}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
CC=${CC:-clang}
python3 - "$SRC" "$BUILD" <<'PY'
import re, sys
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
# (a buffer's GL name is its address on the 32-bit console: the 64-bit model looks it up)
text = '\n\n'.join([func('static uint32_t vgl_collapse_streams('), func('static GLboolean direct_vertex_setup('), func('GLboolean vglDrawElementsDirect(')])
open(sys.argv[2] + '/direct_fixture.inc', 'w').write(re.sub(r'\(vbo \*\)([\w>\[\]-]+)', r'model_vbo(\1)', text))
impl = func('GLboolean _glDrawElements_CustomShadersIMPL(')
at = impl.index('if (direct_layout) {')
end = impl.index('\t}\n', at) + 3
open(sys.argv[2] + '/direct_impl.inc', 'w').write(impl[at:end])
# Order and resets that the fixture cannot see: the test comes before anything that reads the VAO's arrays, and a new or relinked program starts with no patched programs.
for later in ('vertex_layout_lookup_simple(', 'align_attributes(', 'handle_speedhack_attrib()'):
    assert at < impl.index(later), later
assert at > impl.index('setup_frag_program()') and at > impl.index('sceGxmSetFragmentTexture'), 'the program and textures are set first'
create, link = func('GLuint glCreateProgram('), func('static void link_program_untimed(')   # (glLinkProgram is a timing wrapper around it)
for body, name in ((create, 'glCreateProgram'), (link, 'link_program_untimed')):
    assert re.search(r'direct_vprog, 0,', body) and re.search(r'direct_key, 0,', body), name
PY
SAN=(); if [[ ${SANITIZE:-0} == 1 ]]; then SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CC" -std=gnu11 -O1 -g "${SAN[@]}" -w -I"$BUILD" "$VGX/tests/vitagl_direct_draw.c" -o "$BUILD/vitagl_direct_draw"
"$BUILD/vitagl_direct_draw"
echo "vitaGL direct draw tests: all passed"
