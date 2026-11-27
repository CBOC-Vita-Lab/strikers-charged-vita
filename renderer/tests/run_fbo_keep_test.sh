#!/bin/bash
# nxgl25 (vitaGL 0062, MSC_VITA_FBO_KEEP): life-cycle model test of a framebuffer object's colour attach / detach / delete, ASan + UBSan, with mutants (each must fail).
# The texts under test are cut out of the real shared.h (_glFramebufferTexture2D) and framebuffers.c (glDeleteFramebuffers); the guard in gxm.c scene_reset is checked by its place. Usage: run_fbo_keep_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_fbo_keep_test}; rm -rf "$O"; mkdir -p "$O"
SRC="$H/../../dependencies/vitaGL/source"
cut_text() {
    awk '/^static inline __attribute__\(\(always_inline\)\) void _glFramebufferTexture2D\(/{p=1} p{print} p&&/^}/{exit}' "$SRC/shared.h" | sed 's/const int tiled_rt = /int tiled_rt; tiled_rt = /' > "$1/under_test_attach.inc"   # (C++ refuses a jump over an initialization; C does not)
    awk '/^void glDeleteFramebuffers\(/{p=1} p{print} p&&/^}/{exit}' "$SRC/framebuffers.c" | sed 's/(framebuffer \*)ids\[--n\]/FBFROM(ids[--n])/' > "$1/under_test_delete.inc"; }
build() { nice -n 15 g++ -std=gnu++17 -O1 -g0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fpermissive -w -I"$1" "$H/fbo_keep_model.cpp" -o "$2"; }
mkdir -p "$O/ok"; cut_text "$O/ok"
grep -q "vgl_fb_detach_keep" "$O/ok/under_test_attach.inc" && grep -q "mark_rt_as_dirty" "$O/ok/under_test_delete.inc" && grep -q "FBFROM" "$O/ok/under_test_delete.inc" || { echo "text under test not found"; exit 2; }
build "$O/ok" "$O/ok/t"; "$O/ok/t" 2>&1 | tail -4
bad=0; n=0
mutant() { # name, file (attach|delete), sed expression
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; cp "$O/ok/"*.inc "$d/"; case "$3" in perl:*) perl -0pi -e "${3#perl:}" "$d/under_test_$2.inc";; *) sed -i -e "$3" "$d/under_test_$2.inc";; esac
    if cmp -s "$O/ok/under_test_$2.inc" "$d/under_test_$2.inc"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t" 2>/dev/null || { echo "MUTANT $1 does not compile (fix the mutant)"; bad=1; return; }
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1"; fi
    rm -f "$d/t"
}
# the kept detach is the first block after "if (tex_id == 0 && vgl_fb_detach_keep) {"
mutant "kept detach falls through (reads slot 0's size)"   attach 'perl:s/(vgl_fb_detach_keep\) \{.*?fb->tex = NULL;\n)\t\t\treturn;/$1/s'
mutant "kept detach drops the target"                      attach 's/^\t\tif (tex_id == 0 \&\& vgl_fb_detach_keep) {/&\n\t\t\tif (fb->target) { mark_rt_as_dirty(fb->target); fb->target = NULL; }/'
mutant "kept detach drops the depth surface"               attach 's/^\t\tif (tex_id == 0 \&\& vgl_fb_detach_keep) {/&\n\t\t\tif (fb->depthbuffer_ptr) { mark_depth_stencil_as_dirty(fb->depthbuffer_ptr); fb->depthbuffer_ptr = NULL; fb->depthbuffer_state \&= ~DEPTHBUFFER_READY; }/'
mutant "kept detach leaves the scene in use (no dirty flag)" attach '/dirty_framebuffer = GL_TRUE;/d'
mutant "kept detach marks dirty only for another object" attach 's/if (in_use_framebuffer == fb) dirty_framebuffer/if (in_use_framebuffer != fb) dirty_framebuffer/'
mutant "kept detach forgets the reference"                 attach '0,/fb->tex->ref_counter--;/s//(void)0;/'
mutant "kept detach keeps the texture pointer"             attach '0,/^\t\t\tfb->tex = NULL;/s//\t\t\t(void)0;/'
mutant "kept detach never frees a dirty texture"           attach '0,/gpu_free_texture(fb->tex);/s//(void)0;/'
mutant "switch ignored (always kept)"                      attach 's/tex_id == 0 \&\& vgl_fb_detach_keep/tex_id == 0/'
mutant "switch ignored (never kept)"                       attach 's/tex_id == 0 \&\& vgl_fb_detach_keep/0/'
mutant "a size change keeps the target"                    attach 'perl:s/(old_w != fb->width.*?\{\n\t\t\tif \(fb->target\) \{\n)\t\t\t\tmark_rt_as_dirty\(fb->target\);\n\t\t\t\tfb->target = NULL;/$1/s'
mutant "only a width change counts"                        attach 's/old_w != fb->width || old_h != fb->height/old_w != fb->width/'
mutant "only a height change counts"                       attach 's/old_w != fb->width || old_h != fb->height/old_h != fb->height/'
mutant "a size change keeps the depth surface"             attach 's/^\t\t\t\tfb->depthbuffer_ptr = NULL;/\t\t\t\t(void)0;/'
mutant "a size change forgets the depth-ready flag"        attach 's/fb->depthbuffer_state \&= ~DEPTHBUFFER_READY;/(void)0;/'
mutant "delete leaves the target when no texture is attached" delete 's/^\t\t\tif (fb->target) {/\t\t\tif (fb->target \&\& fb->tex) {/'
mutant "delete leaves the depth surface"                   delete 's/^\t\t\tif (fb->depthbuffer_ptr) {/\t\t\tif (0) {/'
mutant "delete forgets the texture reference"              delete 's/fb->tex->ref_counter--;/(void)0;/'
mutant "delete does not clear the target pointer"          delete 's/^\t\t\tfb->target = NULL;/\t\t\t(void)0;/'
# the guard in gxm.c scene_reset sits before the first read of ->tex->data, and without it the read comes first
placed() { awk '/^GLboolean scene_reset\(void\)/{p=1} p&&/!active_write_fb->tex\)/{a=NR} p&&/active_write_fb->tex->data/{if(!b)b=NR} p&&/^}/{exit} END{exit !(a&&b&&a<b)}' "$1"; }
placed "$SRC/gxm.c" && echo "gxm.c scene_reset: the null-texture guard comes before the first read of ->tex->data" || { echo "gxm.c scene_reset has no guard before ->tex->data"; bad=1; }
sed 's/if (!active_write_fb->tex) {/if (0) {/' "$SRC/gxm.c" > "$O/gxm_mut.c"
if placed "$O/gxm_mut.c"; then echo "MUTANT SURVIVED: scene_reset without the guard"; bad=1; else echo "mutant killed: scene_reset without the guard"; fi
# nxr6b: with the robust scenes OFF the guard must still refuse the scene (it logged and fell through to sceGxmBeginScene with the freed texture's colour surface)
unguarded() { awk '/^GLboolean scene_reset\(void\)/{p=1} p&&/if \(!active_write_fb->tex\) \{/{g=1} g&&/vgl_robust_scenes/{bad=1} g&&/return scene_reset_failed\(\);/{ret=1} g&&/^\t\t\t\}/{exit} END{exit !(ret&&!bad)}' "$1"; }
unguarded "$SRC/gxm.c" && echo "gxm.c scene_reset: the null-texture guard refuses the scene whether or not the robust scenes are on" || { echo "gxm.c scene_reset: the null-texture guard depends on vgl_robust_scenes"; bad=1; }
perl -0pe 's/(if \(!active_write_fb->tex\) \{.*?)\t\t\t\treturn scene_reset_failed\(\);/$1\t\t\t\tif (vgl_robust_scenes) return scene_reset_failed();/s' "$SRC/gxm.c" > "$O/gxm_mut2.c"
if unguarded "$O/gxm_mut2.c"; then echo "MUTANT SURVIVED: the guard back under vgl_robust_scenes"; bad=1; else echo "mutant killed: the guard back under vgl_robust_scenes"; fi
rm -rf "$O"/m[0-9]* 2>/dev/null || true
[ $bad = 0 ] && echo "ALL OK ($n mutants killed)" || { echo "FAILED"; exit 1; }
