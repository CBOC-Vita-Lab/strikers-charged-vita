#!/bin/bash
# 1.3 MSC_VGX_VGL_MASKKEEP (vitaGL patch 0026, MASK_KEEP): the mask records and update_scissor_test are taken from dependencies/vitaGL/source/tests.c (not a
# copy) and run over a model of the GPU's mask bit: per depth/stencil surface a grid of mask bits in memory, a scene that loads it or starts from the background
# (0), stores it back or not, and the two mask quads drawn as the stencil function says (NEVER clears the bit under the quad, ALWAYS sets it). Things that are
# not the mask quads are hostile: a scene that does not store leaves random bits in memory (a partial render may have written some), a stencil clear and a new
# surface randomise the bits, so a record that survives one of them is caught. The scene starts follow gxm.c scene_reset (a framebuffer object rebuilds at
# every scene, the display when it was not the last target or did not load). Invariant: after every update_scissor_test the open scene's mask is exactly the
# scissor (the region when on, the whole target when off), with MASK_KEEP on and off; with it on, some rebuilds must have been kept; with it off, none.
# BREAK=1: the record survives a scene that does not load (both guards removed); BREAK=2: one that does not store; BREAK=3: a stencil clear. Each must fail.
# 1.3 MASK_BG (patch 0029, tests.c vgl_mb_scene_begin and the skip in update_scissor_test) runs in the same model: the surface has a background mask the scene that does not load
# starts from (what vglDepthStencilSurfaceSetBackgroundMask sets), the invariant is the same, and with MASK_BG on (with MASK_KEEP on and off) the quads drawn must be fewer.
# 1.3 MASK_CLIP (patch 0055, tests.c: the clear quad of a rebuild under an enabled scissor is drawn with the tile region clip set to the region the record says the mask holds): the stub of
# sceGxmSetRegionClip is a real clip here (tile-rounded outward, 8-pixel tiles in the model's small targets; a framebuffer object's clip rows are GL rows, the display's rows from the top, as
# update_scissor_test gives them), so a clear that does not reach every set bit leaves it set and the invariant fails. BREAK=7: the clip covers half of the old region; BREAK=8: a framebuffer
# object's clip is given in rows from the top (the other frame); BREAK=9: a record that is not valid is used. Each must fail.
# BREAK=4: the background is set with the scissor test on; BREAK=5: a background that was set is not cleared again for an unloaded scene that needs it clear; BREAK=6: the quad is
# left out although the scene was not begun with the background set.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${TESTS_C:-$VGX/../dependencies/vitaGL/source/tests.c}
SHARED=${SHARED_H:-$VGX/../dependencies/vitaGL/source/shared.h}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
# the call sites the model stands for (gxm.c scene_reset and init_depth_stencil_buffer_ms, misc.c glClear)
python3 - "$(dirname "$SRC")" <<'PY'
import sys
d = sys.argv[1]
g = open(d + '/gxm.c').read(); m = open(d + '/misc.c').read()
sr = g[g.index('GLboolean scene_reset(void) {'):]
b = sr.rindex('sceGxmBeginScene(', 0, sr.index('vgl_mk_scene_begin('))
assert sr.index('vgl_mk_scene_begin(') < sr.index('update_scissor_test();'), 'gxm.c: the scene hook must come before the scene-start rebuild'
init = g[g.index('GLboolean init_depth_stencil_buffer_ms('):g.index('GLboolean init_depth_stencil_buffer(')]
assert init.index('vgl_memset(depth_buffer, 0x80') < init.index('vgl_mk_forget(depth_buffer)'), 'gxm.c: a new surface must drop its record'
gc = m[m.index('void glClear(GLbitfield mask) {'):]
assert gc.count('vgl_mk_touch()') == 2, 'misc.c glClear: both paths must report a stencil clear'
PY
python3 - "$SRC" "$SHARED" "$BUILD" "${BREAK:-0}" <<'PY'
import sys
src = open(sys.argv[1]).read(); shared = open(sys.argv[2]).read(); brk = sys.argv[4]
def func(s, sig):
    i = s.index(sig); j = s.index('{', i); d = 0; k = j
    while True:
        if s[k] == '{': d += 1
        elif s[k] == '}':
            d -= 1
            if d == 0: return s[i:k + 1] + '\n'
        k += 1
a = src.index('GLboolean vgl_mask_keep = GL_FALSE;')
b = src.index('\n}\n', src.index('void vgl_mk_touch(void)')) + 3
block = src[a:b]
if brk == '1':
    t = 'if (!loads)\n\t\tr->valid = 0;'; assert t in block; block = block.replace(t, '(void)loads;')
elif brk == '2':
    t = 'if (stores) {'; assert t in block; block = block.replace(t, 'if (1 || stores) {')
elif brk == '3':
    t = 'vgl_mk_cur->valid = 0;\n\t\tvgl_mk_stat[2]++;'; assert t in block; block = block.replace(t, 'vgl_mk_stat[2]++;')
bg = src[src.index('/* MASKBG BEGIN */'):src.index('/* MASKBG END */')]
if brk == '4':
    t = 'const GLboolean set = (vgl_mask_bg & (is_rendering_display ? 2 : 1)) && !scissor_test_state;'; assert t in bg; bg = bg.replace(t, 'const GLboolean set = (vgl_mask_bg & (is_rendering_display ? 2 : 1));')
elif brk == '5':
    t = 'vglDepthStencilSurfaceSetBackgroundMask(ds, set ? 1 : 0);'; assert t in bg; bg = bg.replace(t, 'if (set) vglDepthStencilSurfaceSetBackgroundMask(ds, 1);')
upd = src[src.index('void update_scissor_test() {'):src.index('void reset_scissor_test_region(void)')]
if brk == '6':
    t = '} else if (vgl_mb_ds) {'; assert t in upd; upd = upd.replace(t, '} else if (1) {')
if brk == '7':
    t = 'mc_w = vgl_mk_cur->rw;'; assert t in upd; upd = upd.replace(t, 'mc_w = vgl_mk_cur->rw / 2;')
elif brk == '8':
    t = 'mc_y = is_rendering_display ? vgl_mk_cur->y : vgl_mk_cur->gly;'; assert t in upd; upd = upd.replace(t, 'mc_y = vgl_mk_cur->y;')
elif brk == '9':
    t = 'vgl_mk_cur && vgl_mk_cur->valid && vgl_mk_cur->on && vgl_mk_cur->disp'; assert t in upd; upd = upd.replace(t, 'vgl_mk_cur && vgl_mk_cur->on && vgl_mk_cur->disp')
out = func(shared, 'static inline __attribute__((always_inline)) void vector4f_convert_to_local_space(') + block + '\n' + bg + '\n' + upd   # (the update's #ifdef branches open a brace each)   # (its #ifdef branches open a brace each)
if brk == '1':   # (and the second guard in update_scissor_test: the begun-from-background flag)
    t = ' && !vgl_rt_skip_invalidation) {'; assert t in out; out = out.replace(t, ') {')
open(sys.argv[3] + '/fixture.inc', 'w').write(out)
PY
cat > "$BUILD/test.c" <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char GLboolean;
#define GL_TRUE 1
#define GL_FALSE 0
#define STORE_DEPTH_STENCIL 1
#define DISPLAY_WIDTH 48
#define DISPLAY_HEIGHT 32
#define DISPLAY_WIDTH_FLOAT 48.0f
#define DISPLAY_HEIGHT_FLOAT 32.0f
typedef struct { float x, y, z, w; } vector4f;
typedef struct { int x, y, w, h, gl_x, gl_y, gl_w, gl_h; } scissor_region;
typedef enum { SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED } SceGxmDepthStencilForceLoadMode;
typedef enum { SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED } SceGxmDepthStencilForceStoreMode;
typedef struct { void *depthData; int load, store; int bg; } SceGxmDepthStencilSurface;
#define VGL_RT_NOLOAD 1
static void vglDepthStencilSurfaceSetBackgroundMask(SceGxmDepthStencilSurface *s, uint8_t e) { s->bg = e; }
static SceGxmDepthStencilForceLoadMode sceGxmDepthStencilSurfaceGetForceLoadMode(SceGxmDepthStencilSurface *s) { return s->load ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED; }
static SceGxmDepthStencilForceStoreMode sceGxmDepthStencilSurfaceGetForceStoreMode(SceGxmDepthStencilSurface *s) { return s->store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED; }
enum { SCE_GXM_STENCIL_FUNC_NEVER = 0, SCE_GXM_STENCIL_FUNC_ALWAYS = 7, SCE_GXM_STENCIL_OP_KEEP = 0, SCE_GXM_CULL_NONE = 0, SCE_GXM_REGION_CLIP_OUTSIDE = 1,
       SCE_GXM_PARAMETER_TYPE_F32 = 0 };
typedef int SceGxmParameterType;
typedef struct { int width, height; } framebuffer;
static void *gxm_context;
static int clear_vertex_program_obj, *clear_vertex_program_patched = &clear_vertex_program_obj, scissor_test_fragment_program_obj, scissor_clear_vertex_state, clear_draw_state;
static void *scissor_test_fragment_program = &scissor_test_fragment_program_obj;
static const void *cur_fp; static const void *cur_pvs; static int stencil_func = -1;
static framebuffer *in_use_framebuffer; static GLboolean is_rendering_display, scissor_test_state, dirty_scissor_state, vgl_rt_skip_invalidation;
static uint32_t vgl_rt_invalidations_skipped;
static scissor_region region;
static vector4f scissor_vertex_mem, *scissor_test_vertices = &scissor_vertex_mem, clear_vertex_mem = {-1.f, 1.f, 1.f, -1.f}, *clear_vertices = &clear_vertex_mem;
static const int clear_position_offset = 0, clear_depth_offset = 16;
static uint8_t vbuf[64];
static uint32_t quads_drawn;
// the open scene's mask: one byte per pixel of the target (rows from the top)
static uint8_t *scene_mask; static int tw, th;
static void sceGxmSetVertexProgram(void *c, void *p) { (void)c; (void)p; }
static void sceGxmSetFragmentProgram(void *c, const void *p) { (void)c; cur_fp = p; }
static void invalidate_viewport(void) {}
static void validate_viewport(void) {}
static void change_cull_mode(void) {}
static void refresh_stencil_settings(void) { stencil_func = -1; }
static void vglRestoreVertexUniformBuffer(void) {}
static void sceGxmSetCullMode(void *c, int m) { (void)c; (void)m; }
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = 1 << 20, clip_y1 = 1 << 20;   // the tile region clip in force (inclusive pixels, in the surface's own clip frame)
static void sceGxmSetRegionClip(void *c, int m, int a, int b, int d, int e) { (void)c; (void)m; clip_x0 = a; clip_y0 = b; clip_x1 = d; clip_y1 = e; }
static void sceGxmSetPrecomputedVertexState(void *c, const void *s) { (void)c; cur_pvs = s; }
static void sceGxmSetFrontStencilFunc(void *c, int f, int a, int b, int d, int m, int w) { (void)c; (void)a; (void)b; (void)d; (void)m; (void)w; stencil_func = f; }
static void sceGxmSetBackStencilFunc(void *c, int f, int a, int b, int d, int m, int w) { (void)c; (void)f; (void)a; (void)b; (void)d; (void)m; (void)w; }
static void sceGxmReserveVertexDefaultUniformBuffer(void *c, void **b) { (void)c; *b = vbuf; }
static void vglSetUniformData(uint8_t *b, SceGxmParameterType t, int off, uint32_t n, uint32_t comps, const void *src, SceGxmParameterType it) {
    (void)t; (void)off; (void)n; (void)it; memcpy(b, src, comps * 4); }
static void sceGxmDrawPrecomputed(void *c, const void *s) {
    (void)c; (void)s;
    if (cur_fp != scissor_test_fragment_program) { puts("draw with another program"); exit(1); }
    if (stencil_func != SCE_GXM_STENCIL_FUNC_NEVER && stencil_func != SCE_GXM_STENCIL_FUNC_ALWAYS) { puts("mask quad without its stencil function"); exit(1); }
    vector4f q = *clear_vertices;
    if (!cur_pvs) memcpy(&q, vbuf + clear_position_offset, 16);
    const float x0 = (q.x + 1.f) * 0.5f * tw, x1 = (q.y + 1.f) * 0.5f * tw, y0 = (1.f - q.z) * 0.5f * th, y1 = (1.f - q.w) * 0.5f * th;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const float cx = x + 0.5f, cy = y + 0.5f;
            // the tile region clip (OUTSIDE: what is outside is clipped), rounded outward to whole tiles; a framebuffer object's rows are GL rows (from the bottom)
            const int cy_row = is_rendering_display ? y : th - 1 - y;
            if (x < (clip_x0 & ~7) || x > (clip_x1 | 7) || cy_row < (clip_y0 & ~7) || cy_row > (clip_y1 | 7)) continue;
            if (cx > x0 && cx < x1 && cy > y0 && cy < y1) scene_mask[y * tw + x] = stencil_func == SCE_GXM_STENCIL_FUNC_ALWAYS;
        }
    ++quads_drawn;
}
#include "fixture.inc"

// the model
#define NFB 4
static framebuffer fbs[NFB] = {{40, 24}, {24, 16}, {40, 24}, {16, 16}};
static SceGxmDepthStencilSurface ds[NFB + 1]; // [NFB]: the display
static uint8_t mem[NFB + 1][64 * 64];        // each surface's memory (mask bits)
static uint8_t heap[8][64 * 64];             // the depth memory a surface can be given (addresses come back)
static uint8_t live[64 * 64];
static int cur = -1, last = -1;
static unsigned rnd(void) { static unsigned s = 12345; s = s * 1103515245u + 12345u; return s >> 8; }
static void dims(int k, int *w, int *h) { if (k == NFB) { *w = DISPLAY_WIDTH; *h = DISPLAY_HEIGHT; } else { *w = fbs[k].width; *h = fbs[k].height; } }
static void new_surface(int k) {
    void *a;
    for (;;) { // (memory no other live surface has; an address may come back after its surface went)
        a = heap[rnd() % 8];
        int used = 0;
        for (int j = 0; j <= NFB; ++j) used |= j != k && ds[j].depthData == a;
        if (!used) break;
    }
    ds[k].depthData = a;
    memset(mem[k], 1, sizeof mem[k]);   // (0x80: the bit set everywhere)
    vgl_mk_forget(ds[k].depthData);
}
static void end_scene(void) {
    if (cur < 0) return;
    if (ds[cur].store) memcpy(mem[cur], live, sizeof live);
    else for (size_t i = 0; i < sizeof live; ++i) if (rnd() & 1) mem[cur][i] = rnd() & 1; // (partial renders: anything)
    cur = -1;
}
static long checks, bad;
static void check(const char* what) {
    ++checks;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            int want = 1;
            if (scissor_test_state) want = x >= region.x && x < region.x + region.w && y >= region.y && y < region.y + region.h;
            if (live[y * tw + x] != want) {
                if (bad++ < 5) printf("%s: pixel %d,%d mask %d, scissor says %d (target %dx%d, on %d, region %d,%d %dx%d)\n", what, x, y, live[y * tw + x], want, tw, th,
                                      scissor_test_state, region.x, region.y, region.w, region.h);
                return;
            }
        }
}
static void begin_scene(int k) {
    end_scene();
    cur = k;
    ds[k].load = (rnd() % 5) != 0;
    ds[k].store = (rnd() % 6) != 0;
    is_rendering_display = k == NFB;
    in_use_framebuffer = k == NFB ? NULL : &fbs[k];
    dims(k, &tw, &th);
    scene_mask = live;
    vgl_mb_scene_begin(&ds[k], ds[k].load ? 0 : VGL_RT_NOLOAD);   // (before the begin, as gxm.c scene_reset does)
    if (ds[k].load) memcpy(live, mem[k], sizeof live); else memset(live, ds[k].bg ? 1 : 0, sizeof live);   // (what a scene that does not load starts from: the background mask)
    vgl_mk_scene_begin(&ds[k]);
    // gxm.c scene_reset under STORE_DEPTH_STENCIL
    if (!ds[k].load) { dirty_scissor_state = GL_TRUE; vgl_rt_skip_invalidation = GL_TRUE; }
    if (last != k) { dirty_scissor_state = GL_TRUE; last = k; }
    if (k != NFB) dirty_scissor_state = GL_TRUE;
    if (dirty_scissor_state) { update_scissor_test(); check("scene start"); }
}
static void set_region(void) {
    static const int R[4][4] = {{0, 0, 16, 16}, {4, 2, 20, 10}, {0, 0, 64, 64}, {8, 8, 8, 8}};
    const int* r = R[rnd() % 4];
    region.gl_x = r[0]; region.gl_y = r[1]; region.gl_w = r[2]; region.gl_h = r[3];
}
static long run(GLboolean keep, int bgmask, int mclip, uint32_t *kept) {
    vglSetMaskKeep(keep);
    vglSetMaskBackground(bgmask);
    vglSetMaskClip(mclip);
    memset(ds, 0, sizeof ds);
    for (int k = 0; k <= NFB; ++k) new_surface(k);
    cur = last = -1; scissor_test_state = GL_FALSE; set_region();
    const uint32_t k0 = vgl_mk_stat[1];
    for (int i = 0; i < 200000; ++i) {
        const unsigned op = rnd() % 16;
        if (op < 5 || cur < 0) begin_scene(rnd() % (NFB + 1));
        else if (op < 8) { scissor_test_state = !scissor_test_state; dirty_scissor_state = GL_TRUE; }
        else if (op < 11) { set_region(); if (scissor_test_state) dirty_scissor_state = GL_TRUE; }
        else if (op == 11) { for (size_t j = 0; j < sizeof live; ++j) if (rnd() & 1) live[j] = rnd() & 1; vgl_mk_touch(); } // a stencil clear
        else if (op == 12 && (rnd() % 8) == 0) { end_scene(); new_surface(rnd() % (NFB + 1)); }
        else if (op == 13) dirty_scissor_state = GL_TRUE; // glPopAttrib, the same state again
        if (cur >= 0 && dirty_scissor_state) { update_scissor_test(); check("scissor update"); }
    }
    end_scene();
    *kept = vgl_mk_stat[1] - k0;
    return bad;
}
int main(void) {
    uint32_t keptOn = 0, keptOff = 0, k2 = 0, k3 = 0;
    uint32_t q[10]; uint32_t armed[10], bgkept[10];
    const int cfg[10][3] = {{0, 0, 0}, {1, 0, 0}, {0, 3, 0}, {1, 3, 0}, {1, 1, 0}, {1, 2, 0}, {0, 0, 1}, {1, 0, 1}, {1, 3, 1}, {0, 3, 1}};   // MASK_KEEP, MASK_BG mode, MASK_CLIP
    uint32_t* kp[10] = {&keptOff, &keptOn, &k2, &k3, &k3, &k3, &k3, &k3, &k3, &k3};
    uint32_t clipNarrowed[10] = {0};
    for (int c = 0; c < 10; ++c) {
        const uint32_t q0 = quads_drawn, a0 = vgl_mb_stat[0], b0 = vgl_mb_stat[1], n0 = vgl_mc_stat[0];
        bad = 0; checks = 0;
        if (run(cfg[c][0], cfg[c][1], cfg[c][2], kp[c])) { printf("MASK_KEEP %s, MASK_BG mode %d, MASK_CLIP %s: %ld of %ld checks wrong%s\n", cfg[c][0] ? "on" : "off", cfg[c][1], cfg[c][2] ? "on" : "off", bad, checks, c == 0 ? " (the model itself)" : ""); return 1; }
        clipNarrowed[c] = vgl_mc_stat[0] - n0;
        q[c] = quads_drawn - q0; armed[c] = vgl_mb_stat[0] - a0; bgkept[c] = vgl_mb_stat[1] - b0;
    }
    if (keptOff || k2) { printf("MASK_KEEP off kept %u / %u rebuilds\n", keptOff, k2); return 1; }
    if (!keptOn) { puts("MASK_KEEP on kept nothing"); return 1; }
    if (armed[0] || armed[1] || bgkept[0] || bgkept[1]) { puts("MASK_BG off still armed a scene or left a quad out"); return 1; }
    if (!armed[2] || !bgkept[2] || !armed[3] || !bgkept[3]) { puts("MASK_BG on armed nothing or left nothing out"); return 1; }
    if (!armed[4] || !armed[5]) { printf("MASK_BG modes 1 and 2 armed %u and %u scenes, mode 3 %u\n", armed[4], armed[5], armed[3]); return 1; }
    if (clipNarrowed[0] || clipNarrowed[1] || clipNarrowed[2] || clipNarrowed[3]) { puts("MASK_CLIP off still narrowed a clear"); return 1; }
    if (!clipNarrowed[6] || !clipNarrowed[7] || !clipNarrowed[8] || !clipNarrowed[9]) { printf("MASK_CLIP on narrowed nothing in a configuration (%u %u %u %u)\n", clipNarrowed[6], clipNarrowed[7], clipNarrowed[8], clipNarrowed[9]); return 1; }
    if (q[2] >= q[0] || q[3] >= q[1]) { printf("MASK_BG on did not cut the quads (%u -> %u, %u -> %u)\n", q[0], q[2], q[1], q[3]); return 1; }
    printf("vitaGL MASK_CLIP: exact in all ten configurations (MASK_KEEP on/off x MASK_BG 0-3 x MASK_CLIP on/off); clears narrowed to the old region %u / %u / %u / %u (per MASK_CLIP configuration), kilopixels of clear fill left out %u\n",
           clipNarrowed[6], clipNarrowed[7], clipNarrowed[8], clipNarrowed[9], vgl_mc_stat[1]);
    printf("vitaGL MASK_KEEP: %ld mask checks per configuration exact in all ten configurations (MASK_BG modes 0-3); MASK_KEEP rebuilds kept %u, mask quads %u -> %u; MASK_BG: scenes started with the mask set %u, quads left out %u, mask quads %u -> %u (with MASK_KEEP on: %u -> %u)\n",
           checks, keptOn, q[0], q[1], armed[2], bgkept[2], q[0], q[2], q[1], q[3]);
    return 0;
}
C
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable ${SAN:-} -I"$BUILD" "$BUILD/test.c" -o "$BUILD/mk_test"
"$BUILD/mk_test"
