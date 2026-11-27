#!/bin/bash
# mark25 (25 Nov 2026) MSC_VGX_HUDCULL: the HUD run of MSC_VGX_HUDNATIVE is drawn on the display with the viewport of a framebuffer object (a negative height, vgx_backend_gl.cpp ApplyRenderState),
# so its y_scale has the sign an FBO's has, while vitaGL's change_cull_mode (misc.c) picks the cull mode for the DISPLAY (is_rendering_display adds 1). The two tables differ by exactly that
# inversion because the two y_scales differ in sign, so a draw with culling on is culled on the wrong side in the HUD run: the player indicators (glSetDefaultState: cull on) vanish, the score
# digits (GLS_Culling 0) do not. Both vitaGL functions are taken from dependencies/vitaGL/source/misc.c (not copies). The model of the hardware is calibrated on vitaGL's own two cases (a display
# with y_scale < 0 and an FBO with y_scale > 0 both keep GL's meaning): SCE_GXM_CULL_CW culls a triangle whose window-space area (NDC area times the sign of y_scale) is positive, CULL_CCW one whose
# area is negative; the calibration itself is asserted first (display and FBO agree with GL for every front face / cull side / winding).
# Invariant: for every GL state and winding, the HUD run's decision (what the renderer asks for after the fix) equals the FBO path's (the picture the EFB path makes). FIX=0: the renderer asks for
# the same face in the HUD run (the bug); the test must fail. BREAK=1: the fix inverts the wrong faces for CULL_FRONT only; must fail.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${MISC_C:-$VGX/../dependencies/vitaGL/source/misc.c}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$SRC" "$BUILD" <<'PY'
import sys
src = open(sys.argv[1]).read()
def func(sig):
    i = src.index(sig); j = src.index('{', i); d = 0; k = j
    while True:
        if src[k] == '{': d += 1
        elif src[k] == '}':
            d -= 1
            if d == 0: return src[i:k + 1] + '\n'
        k += 1
enum = src[src.index('static enum {'):src.index('} vglCullMode;') + len('} vglCullMode;')] + '\n'
open(sys.argv[2] + '/fixture.inc', 'w').write(enum + func('void vglSetViewportExact(') + func('void change_cull_mode(') + func('void glCullFace(') + func('void glFrontFace(') + func('void glViewport('))
PY
cat > "$BUILD/test.c" <<'C'
#include <stdio.h>
typedef unsigned char GLboolean; typedef int GLint, GLsizei; typedef unsigned GLenum;
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define THREAD_SAFE()
#define VGL_LC_SYNC()
#define DISPLAY_HEIGHT 544
#define SKIP_ERROR_HANDLING 1
#define SCE_GXM_CULL_NONE 0
#define SCE_GXM_CULL_CW 1
#define SCE_GXM_CULL_CCW 2
static float x_port, x_scale, y_port, y_scale, z_port, z_scale;
static GLboolean is_rendering_display, skip_viewport_override, vgl_viewport_exact = GL_TRUE, cull_face_state = GL_TRUE, no_polygons_mode;
static GLenum gl_front_face = GL_CCW, gl_cull_mode = GL_BACK;
static struct { int height; } fb, *in_use_framebuffer = &fb;
static struct { int x, y, w, h; } gl_viewport;
static void *gxm_context;
static int hw_cull; static float hw_ys;
static void sceGxmSetCullMode(void *c, int m) { (void)c; hw_cull = m; }
static void vglSetViewport(void *c, float xp, float xs, float yp, float ys, float zp, float zs) { (void)c; (void)xp; (void)xs; (void)yp; (void)zp; (void)zs; hw_ys = ys; }
#include "fixture.inc"
// window-space area sign of a triangle whose NDC area has sign `ndc` (+1: counter-clockwise in NDC, y up), drawn with the y_scale the last glViewport left
static int win_sign(int ndc) { return hw_ys < 0 ? -ndc : ndc; }
static int culled(int ndc) { const int s = win_sign(ndc); return no_polygons_mode ? 1 : hw_cull == SCE_GXM_CULL_CW ? s > 0 : hw_cull == SCE_GXM_CULL_CCW ? s < 0 : 0; }
// what GL says for this state: front = CCW triangle (NDC area > 0) when the front face is CCW
static int gl_culled(GLenum ff, GLenum cm, int ndc) { const int front = (ff == GL_CCW) == (ndc > 0); return (cm == GL_BACK) ? !front : front; }
// the renderer's request in the HUD run: FIX=1 asks for the opposite face (ApplyRenderState, g_hud.on)
static GLenum hud_face(GLenum cm) {
#ifdef FIX0
    return cm;
#elif defined(BREAK1)
    return cm == GL_FRONT ? GL_FRONT : GL_FRONT;
#else
    return cm == GL_BACK ? GL_FRONT : GL_BACK;
#endif
}
static void vp(int disp, int neg) { is_rendering_display = disp; fb.height = 448; glViewport(0, 0, 640, neg ? -(disp ? 544 : 448) : (disp ? 544 : 448)); }
int main(void) {
    int bad = 0;
    // calibration: the display (y_scale < 0) and an FBO (y_scale > 0) both give GL's decision for every face / side / winding
    for (int disp = 0; disp < 2; ++disp)
        for (int cm = 0; cm < 2; ++cm)
            for (int ndc = -1; ndc <= 1; ndc += 2) {
                const GLenum c = cm ? GL_FRONT : GL_BACK;
                vp(disp, 0); gl_front_face = GL_CCW; gl_cull_mode = c; change_cull_mode();
                if (culled(ndc) != gl_culled(GL_CCW, c, ndc)) { printf("calibration: display %d cull %s ndc %d: hardware %d, GL %d\n", disp, cm ? "front" : "back", ndc, culled(ndc), gl_culled(GL_CCW, c, ndc)); ++bad; }
            }
    if (bad) { printf("FAIL: the model does not reproduce vitaGL's own two cases\n"); return 2; }
    // the HUD run: display scene, FBO-sign viewport (negative height). It must decide as the FBO path does for the same game state (the EFB path's picture).
    for (int cm = 0; cm < 2; ++cm)
        for (int ndc = -1; ndc <= 1; ndc += 2) {
            const GLenum game = cm ? GL_FRONT : GL_BACK;
            vp(0, 0); gl_front_face = GL_CCW; gl_cull_mode = game; change_cull_mode(); const int want = culled(ndc);
            vp(1, 1); gl_front_face = GL_CCW; gl_cull_mode = hud_face(game); change_cull_mode(); const int got = culled(ndc);
            if (hw_ys <= 0) { printf("the HUD viewport has the y_scale of the display (%g): not the FBO case this test is about\n", hw_ys); ++bad; }
            if (got != want) { printf("HUD run: cull %s, triangle ndc %+d: EFB path culls %d, display run culls %d\n", cm ? "front" : "back", ndc, want, got); ++bad; }
        }
    puts(bad ? "FAIL" : "hud cull model: the display run culls what the EFB path culls (4 cases), calibration exact");
    return bad != 0;
}
C
cc -O1 -Wall -Wno-unused-variable -o "$BUILD/t" "$BUILD/test.c" -I"$BUILD" ${EXTRA:-}
"$BUILD/t"
