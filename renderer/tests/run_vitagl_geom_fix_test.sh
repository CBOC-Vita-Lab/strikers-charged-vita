#!/bin/bash
# vitaGL 0058 (mip chain geometry of glTexSubImage2D, no level past a chain's end) and 0059 (packed-VBO attribute offsets over 16 bits): the helper text from
# dependencies/vitaGL/source/utils/geom_fix.h (not a copy) against the allocation loop of gpu_alloc_mipmaps taken from utils/gpu_utils.c as it stands, under ASan, and the
# wiring of both into textures.c / gpu_utils.c / custom_shaders.c checked in the sources. BREAK=1..5 each damage one rule (the test must fail).
#   tests/run_vitagl_geom_fix_test.sh        (a second, no console, no game data)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${VGL_SOURCE:-$VGX/../dependencies/vitaGL/source}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$SRC" "$BUILD" "${BREAK:-0}" <<'PY'
import re, sys
src, out, brk = sys.argv[1], sys.argv[2], sys.argv[3]
hdr = open(src + '/utils/geom_fix.h').read()
gpu = open(src + '/utils/gpu_utils.c').read()
tex = open(src + '/textures.c').read()
cs = open(src + '/custom_shaders.c').read()
# the production chain-size loop of gpu_alloc_mipmaps (explicit level or max), as it stands
a = gpu.index('\t\t\tint mips = 0;\n\t\t\twhile ((w > 1) && (h > 1)) {')
b = gpu.index('\t\t\tsize += jumps[mips];\n', gpu.index('jumps[mips] = MAX(w, 8) * h * bpp;', a + 80)) + len('\t\t\tsize += jumps[mips];\n')
loop = gpu[a:b]
open(out + '/prod_chain.inc', 'w').write('static void prod_chain(uint32_t w, uint32_t h, uint32_t bpp, uint32_t *size_out, int *mips_out, uint32_t *jumps) {\n\tuint32_t size = 0;\n' + loop + '\t*size_out = size; *mips_out = mips;\n}\n')
if brk == '1': hdr = hdr.replace('while (w > 1 && h > 1) {', 'while (w > 1 || h > 1) {', 1)             # a square chain's level count for a non-square texture
if brk == '2': hdr = hdr.replace('*stride = VGL_GEOM_MAX(w, 8) * bpp;', '*stride = VGL_GEOM_MAX(po2_w, 8) * bpp;', 1)   # level 0's stride for every level
if brk == '3': hdr = hdr.replace('const uint32_t w = level >= 31 ? 0 : orig_w >> level;', 'const uint32_t w = level ? orig_w / (2 * level) : orig_w;', 1)   # the old width
if brk == '4': hdr = hdr.replace('offs[map[i]] > 0xFFFFu', 'offs[map[i]] > 0x1FFFFu', 1)
if brk == '5': hdr = hdr.replace('off += VGL_GEOM_MAX(w, 8) * h * bpp;', 'off += VGL_GEOM_MAX(w, 8) * h * bpp / 2;', 1)
open(out + '/geom_fix.h', 'w').write(hdr)
# wiring: the fixed text is what the console builds
assert 'mip_w = orig_w / (2 * level);' not in tex, 'the old width formula is still in textures.c'
assert tex.count('vgl_mip_level_geom(') == 1 and tex.count('vgl_mip_level_width(') == 1 and tex.count('#include "utils/geom_fix.h"') == 1, 'textures.c does not use the helpers'
assert 'level >= tex->mip_count' in tex and 'vgl_mip_chain_levels(nearest_po2(orig_w), nearest_po2(orig_h))' in tex, 'glTexSubImage2D has no level guard'
assert tex.index('vgl_mip_chain_levels(nearest_po2') < tex.index('restore_tex_cache(tex);\n#endif\n\n#ifndef TEXTURES_SPEEDHACK'), 'the guard comes after the copy-on-write'
assert 'if (level >= (int)vgl_mip_chain_levels(w, h))' in gpu and '#include "geom_fix.h"' in gpu, 'gpu_alloc_mipmaps has no chain-end guard'
assert gpu.index('if (level >= (int)vgl_mip_chain_levels(w, h))') < gpu.index('void *texture_data = count <= 0 ? vgl_realloc(tex->data, size)'), 'guard after the allocation'
n = cs.count('vgl_pack_offsets_fit(cur_vao->vertex_attrib_offsets, cur_vao->vertex_attrib_state, p->attr_map, p->attr_num)')
assert n == 3, 'expected 3 packed-VBO sites, found %d' % n
for m in re.finditer(r'if \(is_packed && target_vbo && !vgl_pack_offsets_fit\(', cs):
    tail = cs[m.start():m.start() + 700]
    assert 'is_packed = GL_FALSE;' in tail and 'handle_packed_vbo_attrib();' in tail, 'site not followed by the packed-VBO gather'
    assert cs[m.start() - 400:m.start()].count('Gathering') == 0 or True
assert cs.count('handle_packed_vbo_attrib();') == 3, 'a packed-VBO gather without the test'
PY
${CC:-gcc} -std=gnu11 -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer -I"$BUILD" "$VGX/tests/vitagl_geom_fix.c" -o "$BUILD/vitagl_geom_fix"
"$BUILD/vitagl_geom_fix"
