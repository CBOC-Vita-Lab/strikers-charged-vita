#!/bin/bash
# nxr4b (vitaGL 0063, MSC_VITA_FBO_DUALRT): model test of the ping-pong render target pair, ASan + UBSan, with mutants (each must fail). Texts under test are cut out of the real gxm.c (vgl_dual_make,
# vgl_dual_pick, vglGetDualRTStats, vglFramebufferDualRT), shared.h (VGL_FB_DROP_RT2, _glFramebufferTexture2D) and framebuffers.c (glDeleteFramebuffers); the scene_reset lines are checked by place.
# Usage: run_dual_rt_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_dual_rt_test}; rm -rf "$O"; mkdir -p "$O"
SRC="$H/../../dependencies/vitaGL/source"
cut_text() {
    awk '/^static inline __attribute__\(\(always_inline\)\) void _glFramebufferTexture2D\(/{p=1} p{print} p&&/^}/{exit}' "$SRC/shared.h" | sed 's/const int tiled_rt = /int tiled_rt; tiled_rt = /' > "$1/under_test_attach.inc"
    awk '/^void glDeleteFramebuffers\(/{p=1} p{print} p&&/^}/{exit}' "$SRC/framebuffers.c" | sed 's/(framebuffer \*)ids\[--n\]/FBFROM(ids[--n])/' > "$1/under_test_delete.inc"
    awk '/^#define VGL_FB_DROP_RT2/{p=1} p{print} p&&/while \(0\)/{exit}' "$SRC/shared.h" > "$1/under_test_drop.inc"
    awk '/^static uint32_t vgl_dual_made/{p=1} p{print} p&&/^void vglGetDualRTStats/{q=1} q&&/^}/{exit}' "$SRC/gxm.c" > "$1/under_test_dual.inc"
    awk '/^GLboolean vglFramebufferDualRT\(/{p=1} p{print} p&&/^}/{exit}' "$SRC/gxm.c" | sed 's/(framebuffer \*)fb_id/FBFROM(fb_id)/' > "$1/under_test_api.inc"; }
build() { nice -n 15 g++ -std=gnu++17 -O1 -g0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fpermissive -w -I"$1" "$H/dual_rt_model.cpp" -o "$2"; }
mkdir -p "$O/ok"; cut_text "$O/ok"
for f in attach delete drop dual api; do [ -s "$O/ok/under_test_$f.inc" ] || { echo "text under test not found: $f"; exit 2; }; done
grep -q "vgl_dual_pick" "$O/ok/under_test_dual.inc" && grep -q "VGL_FB_DROP_RT2" "$O/ok/under_test_delete.inc" && grep -q "VGL_FB_DROP_RT2" "$O/ok/under_test_attach.inc" || { echo "text under test incomplete"; exit 2; }
build "$O/ok" "$O/ok/t"; "$O/ok/t" > "$O/ok/out" 2>&1 || { tail -12 "$O/ok/out"; echo "the model test fails on the real texts"; exit 1; }; tail -6 "$O/ok/out"
bad=0; n=0
mutant() { # name, file (attach|delete|drop|dual|api), sed expression
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; cp "$O/ok/"*.inc "$d/"; case "$3" in perl:*) perl -0pi -e "${3#perl:}" "$d/under_test_$2.inc";; *) sed -i -e "$3" "$d/under_test_$2.inc";; esac
    if cmp -s "$O/ok/under_test_$2.inc" "$d/under_test_$2.inc"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t" 2>/dev/null || { echo "MUTANT $1 does not compile (fix the mutant)"; bad=1; return; }
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1"; fi
    rm -f "$d/t"
}
mutant "parity ignored (always the first target)"          dual 's/fb->dual_rt \&\& (frame \& 1)/0/'
mutant "parity changes every second picture"               dual 's/(frame \& 1)/(frame \& 2)/'
mutant "second target used on every picture"               dual 's/fb->dual_rt \&\& (frame \& 1)/fb->dual_rt/'
mutant "pair always on (switch ignored)"                   dual 's/fb->dual_rt \&\& (frame \& 1)/(frame \& 1)/'
mutant "odd picture takes a target2 that was never made"   dual 's/^\t\tif (!fb->target2)$/\t\tif (0)/'
mutant "make without a first target"                       dual 's/if (fb->target2 || !fb->target)/if (fb->target2)/'
mutant "make always makes a new one (leak)"                dual 's/if (fb->target2 || !fb->target)/if (!fb->target)/'
mutant "failed make leaves rubbish in target2"             dual 'perl:s/fb->target2 = NULL;\n\t\tvgl_dual_fail\+\+;/vgl_dual_fail++;/'
mutant "make uses the wrong size"                          dual 's/fb->width, fb->height, gxm_fbo_rt_size, FB_MSAA(fb))) {/fb->height, fb->width, gxm_fbo_rt_size, FB_MSAA(fb))) {/'
mutant "make uses the wrong scenes per frame"              dual 's/fb->width, fb->height, gxm_fbo_rt_size, FB_MSAA(fb))) {/fb->width, fb->height, 1, FB_MSAA(fb))) {/'
mutant "odd scenes never counted"                          dual 's/vgl_dual_odd++;/(void)0;/'
mutant "a failed pick does not fall back to the target"    dual 's/^\treturn fb->target;$/\treturn fb->target2;/'
mutant "drop leaves the second target"                     drop 's/mark_rt_as_dirty((fb)->target2);/(void)0;/'
mutant "drop does not clear the pointer (double mark)"     drop 's/(fb)->target2 = NULL;/(void)0;/'
mutant "drop marks the first target"                       drop 's/mark_rt_as_dirty((fb)->target2);/mark_rt_as_dirty((fb)->target);/'
mutant "delete leaves the second target"                   delete 's/^\t\t\tVGL_FB_DROP_RT2(fb);.*/\t\t\t(void)0;/'
mutant "attach: a size change keeps the second target"     attach 'perl:s/(old_w != fb->width.*?VGL_FB_DROP_RT2\(fb\);)/$1 (void)0;/s; s/(old_w != fb->width[^\n]*\n\t\t\tif \(fb->target\) \{\n[^\n]*\n[^\n]*\n\t\t\t\}\n)\t\t\tVGL_FB_DROP_RT2\(fb\);/$1/s'
mutant "attach: the plain detach keeps the second target"  attach 'perl:s/(Detaching attached texture if passed texture ID is 0\n\t\tif \(tex_id == 0\) \{\n\t\t\tif \(fb->target\) \{\n[^\n]*\n[^\n]*\n\t\t\t\}\n)\t\t\tVGL_FB_DROP_RT2\(fb\);\n/$1/s'
mutant "api: switching off keeps the second target"        api 's/^\t\tVGL_FB_DROP_RT2(fb);/\t\t(void)0;/'
mutant "api: switch sets dual_rt to 2 (any non-zero)"      api 's/fb->dual_rt = on ? 1 : 0;/fb->dual_rt = on ? 1 : 0; if (on) fb->dual_rt = 3;/'
mutant "api: unknown object accepted"                      api 's/if (!fb || !fb->active)/if (0)/'
# scene_reset's lines, by place: the pair is made with the first target, the target of the picture is picked once, and both the census and BeginScene use it
G="$SRC/gxm.c"
placed() { awk '/^GLboolean scene_reset\(void\)/{p=1} p&&/vgl_dual_make\(active_write_fb\)/{a=NR} p&&/scene_rt = vgl_dual_pick\(active_write_fb, vgl_framecount\)/{b=NR} p&&/scn_begin\(scene_rt, 0\)/{c=NR} p&&/sceGxmBeginScene\(gxm_context, 0, scene_rt,/{d=NR} p&&/sceGxmBeginScene\(gxm_context, 0, active_write_fb->target,/{e=NR} p&&/^}/{exit} END{exit !(a&&b&&c&&d&&!e&&a<b&&b<c&&c<d)}' "$1"; }
placed "$G" && echo "gxm.c scene_reset: the pair is made with the target, picked once (frame parity), used by the census and by sceGxmBeginScene" || { echo "gxm.c scene_reset does not use the picked target"; bad=1; }
for m in 's/scene_rt = vgl_dual_pick(active_write_fb, vgl_framecount)/scene_rt = active_write_fb->target/' 's/sceGxmBeginScene(gxm_context, 0, scene_rt,/sceGxmBeginScene(gxm_context, 0, active_write_fb->target,/' 's/scn_begin(scene_rt, 0)/scn_begin(active_write_fb->target, 0)/' 's/vgl_dual_make(active_write_fb); \/\/ 0063/(void)0; \/\/ 0063/'; do
    sed "$m" "$G" > "$O/gxm_mut.c"; cmp -s "$G" "$O/gxm_mut.c" && { echo "scene_reset mutant changed nothing: $m"; bad=1; continue; }
    if placed "$O/gxm_mut.c"; then echo "MUTANT SURVIVED: scene_reset $m"; bad=1; else echo "mutant killed: scene_reset ${m:0:60}"; fi; n=$((n+1)); done
# the library default is off: gen sets dual_rt 0 and target2 NULL
grep -q "framebuffers\[i\].dual_rt = 0;" "$SRC/framebuffers.c" && grep -q "framebuffers\[i\].target2 = NULL;" "$SRC/framebuffers.c" && echo "framebuffers.c: a new object starts with the pair off" || { echo "glGenFramebuffers does not reset the pair"; bad=1; }
rm -rf "$O"/m[0-9]* 2>/dev/null || true
[ $bad = 0 ] && echo "ALL OK ($n mutants killed)" || { echo "FAILED"; exit 1; }
