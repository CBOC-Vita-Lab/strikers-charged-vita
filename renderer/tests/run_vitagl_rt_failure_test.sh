#!/bin/bash
# 1.3 vitaGL 0019: a failed sceGxmCreateRenderTarget ends the scene entry. The block of scene_reset (dependencies/vitaGL/source/gxm.c) over a create that fails and leaves a
# pointer behind, as Vita3K does; no console. Deliberate break: BREAK=1 takes the patch's lines out, and the test must then fail.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${GXM_C:-$VGX/../dependencies/vitaGL/source/gxm.c}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$SRC" "$BUILD" "${BREAK:-0}" <<'PY'
import sys
src = open(sys.argv[1]).read()
i = src.index('// If a rendertarget is not bound to the in use framebuffer')
j = src.index('if (active_write_fb->depth_share_w && active_write_fb->depthbuffer_ptr')   # (0044 sits between the target block and the scene begin)
text = src[i:j]
if sys.argv[3] == '1':
    k = text.index('// 1.3: the result decides')
    text = text[:k] + text[text.index('#endif', k):]
assert 'setup_fbo_render_target_ms(&active_write_fb->target' in text
open(sys.argv[2] + '/block.inc', 'w').write(text)
PY
cat > "$BUILD/rt_failure.c" <<'C'
#include <stdio.h>
#include <stdint.h>
typedef void SceGxmRenderTarget;
typedef struct { SceGxmRenderTarget *target, *target2; int width, height; unsigned char dual_rt; } fb_t;
static void vgl_dual_make(fb_t *f) { (void)f; }   /* 0063: the pair's own test is run_dual_rt_test.sh */
#define GL_FALSE 0
#define GL_TRUE 1
#define FB_MSAA(f) 0
#define LOG_ERRORS 1
static fb_t fb, *active_write_fb = &fb;
static int vgl_robust_scenes = 1, gxm_fbo_rt_size = 8, failures, creates, logs, fail_next;
static char freed_block[64];
#define vgl_log(...) (++logs)
static const char *get_gxm_error_literal(int r) { (void)r; return "err"; }
static int setup_fbo_render_target_ms(SceGxmRenderTarget **rt, int w, int h, int scenes, int mode) {
    (void)w; (void)h; (void)scenes; (void)mode;
    ++creates;
    *rt = freed_block;   // what the emulator leaves when it fails, and the shape of a success
    return fail_next ? (int)0x805B0017 : 0;
}
static int scene_reset_failed(void) { ++failures; return GL_FALSE; }
static int enter(void) {
#include "block.inc"
    return GL_TRUE;
}
int main(void) {
    int bad = 0;
    fb.width = 512; fb.height = 256;
    fail_next = 1;
    if (enter() != GL_FALSE || failures != 1 || fb.target) { puts("FAIL: a failed create went on to the scene"); bad = 1; }
    fail_next = 0;
    if (enter() != GL_TRUE || fb.target != freed_block || creates != 2) { puts("FAIL: the next attempt must create again and go on"); bad = 1; }
    if (enter() != GL_TRUE || creates != 2) { puts("FAIL: a framebuffer with a target creates no other"); bad = 1; }
    if (logs < 1) { puts("FAIL: the failure was not logged"); bad = 1; }
    puts(bad ? "render target failure test FAILED" : "render target failure test passed");
    return bad;
}
C
${CC:-cc} -std=gnu11 -O1 -g -w -I"$BUILD" "$BUILD/rt_failure.c" -o "$BUILD/rt_failure"
"$BUILD/rt_failure"
