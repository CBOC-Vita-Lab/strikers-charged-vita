#!/bin/bash
# 1.3 MSC_VGX_VP_EXACT (vitaGL patch 0025): glViewport is taken from dependencies/vitaGL/source/misc.c (not a copy) over a model of what sceGxmSetViewport does
# with its numbers (window x = x_port + x_scale * ndc_x, y likewise). For every size 1..600 and corner 0..3, framebuffer and display: the exact path must map NDC -1..1
# onto exactly [x, x + width] and [y, y + height] (OpenGL's rule); the old integer path must be off by one pixel exactly for odd sizes (that is what the patch fixes)
# and agree for even ones. BREAK=1 makes the exact path use the shift (the test must fail).
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
out = func('void vglSetViewportExact(') + func('void glViewport(')
open(sys.argv[2] + '/fixture.inc', 'w').write(out)
PY
cat > "$BUILD/test.c" <<'C'
#include <stdio.h>
#include <math.h>
typedef unsigned char GLboolean; typedef int GLint, GLsizei;
#define GL_TRUE 1
#define GL_FALSE 0
#define THREAD_SAFE()
#define VGL_LC_SYNC()
#define DISPLAY_HEIGHT 544
#define SKIP_ERROR_HANDLING 1
static float x_port, x_scale, y_port, y_scale, z_port, z_scale;
static GLboolean is_rendering_display, skip_viewport_override, vgl_viewport_exact = GL_TRUE;
static struct { int height; } fb, *in_use_framebuffer = &fb;
static struct { int x, y, w, h; } gl_viewport;
static void *gxm_context;
static float gx, gs, gy, gys;
static void vglSetViewport(void *c, float xp, float xs, float yp, float ys, float zp, float zs) { (void)c; (void)zp; (void)zs; gx = xp; gs = xs; gy = yp; gys = ys; }
#include "fixture.inc"
int main(void) {
    int bad = 0, oldOff = 0, oldEven = 0;
    for (int disp = 0; disp < 2; ++disp)
        for (int n = 1; n <= 600; ++n)
            for (int c = 0; c < 4; ++c) {
                is_rendering_display = disp; fb.height = 600;
                const int H = disp ? DISPLAY_HEIGHT : fb.height;
                for (int exact = 1; exact >= 0; --exact) {
                    vglSetViewportExact(exact ? GL_TRUE : GL_FALSE);
#ifdef BREAK
                    vgl_viewport_exact = GL_FALSE;
#endif
                    glViewport(c, c, n, n);
                    const float x0 = gx - gs, x1 = gx + gs;               // window x of NDC -1 and +1
                    const float ya = gy - gys, yb = gy + gys;             // window y (rows counted from the top on the display, from the bottom on an FBO)
                    const float y0 = disp ? H - (ya > yb ? ya : yb) : (ya < yb ? ya : yb), y1 = disp ? H - (ya < yb ? ya : yb) : (ya > yb ? ya : yb);
                    const int ok = fabsf(x0 - c) < 1e-3f && fabsf(x1 - (c + n)) < 1e-3f && fabsf(y0 - c) < 1e-3f && fabsf(y1 - (c + n)) < 1e-3f;
                    if (exact && !ok) { if (bad++ < 4) printf("exact: size %d corner %d display %d -> x %.1f..%.1f y %.1f..%.1f\n", n, c, disp, x0, x1, y0, y1); }
                    if (!exact) {
                        if (n & 1) oldOff += fabsf(x1 - (c + n - 1)) < 1e-3f && fabsf(x0 - c) < 1e-3f;
                        else oldEven += ok;
                    }
                }
            }
    if (bad) { printf("%d exact viewports wrong\n", bad); return 1; }
    if (oldOff != 2 * 300 * 4 || oldEven != 2 * 300 * 4) { printf("old path: %d odd one pixel short (want 2400), %d even exact (want 2400)\n", oldOff, oldEven); return 1; }
    puts("vitaGL viewport: exact halves cover [x, x+w] for every size; the old shift drew odd sizes one pixel short and even sizes the same");
    return 0;
}
C
${CC:-gcc} -std=gnu11 -O1 -g -w ${BREAK:+-DBREAK} -I"$BUILD" "$BUILD/test.c" -lm -o "$BUILD/vp_test"
"$BUILD/vp_test"
