#!/bin/bash
# cache25 (vitaGL 0060): host test of the cached stream arena in scene mode (one write-back per scene submit) against a fake data cache, with mutants (each must fail).
# The arena text is the real text of vgl.c, the two macros are the real text of shared.h, the scene_end placement is checked in the real gxm.c. Usage: run_stream_scene_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_stream_scene_test}; rm -rf "$O"; mkdir -p "$O"
SRC="$H/../../dependencies/vitaGL/source"
cut_arena() { awk '/^\/\/ ---- GLB: cached stream arena/{p=1} /^void vglSetupCachedStream/{p=0} p' "$SRC/vgl.c" | sed 's/~63u/~(uintptr_t)63/g' > "$1/arena_under_test.inc"
              # the cached branch of shared.h's macros (the lines after the #if, before the #else)
              awk '/^#if !defined\(DISABLE_CIRCULAR_POOL\) && !defined\(CIRCULAR_POOL_SPEEDHACK\)$/{c++} c>=1&&/^#define VGL_STREAM_(FLUSH_IF_NEEDED|SCENE_SUBMIT)\(\) if/{print}' "$SRC/shared.h" | sed 's|//.*||' > "$1/macros.inc"; }
build() { nice -n 15 g++ -std=gnu++17 -O1 -fsanitize=address,undefined -DHAVE_SCRATCH_MEMORY -I"$1" "$H/stream_scene.cpp" -o "$2" ; }
mkdir -p "$O/ok"; cut_arena "$O/ok"
test -s "$O/ok/arena_under_test.inc" && test "$(grep -c '^#define' "$O/ok/macros.inc")" = 2 || { echo "markers or macros not found"; exit 2; }
grep -q "vgl_stream_scene_mode" "$O/ok/arena_under_test.inc" || { echo "vgl.c has no scene mode flag"; exit 2; }
build "$O/ok" "$O/ok/t"; "$O/ok/t" 2>&1 | tail -4
bad=0; n=0
mutant() { # name, file (arena_under_test.inc | macros.inc), sed expression
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; cp "$O/ok/arena_under_test.inc" "$O/ok/macros.inc" "$d/"; sed -i -e "$3" "$d/$2"
    if cmp -s "$O/ok/$2" "$d/$2"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t" 2>/dev/null
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1"; fi
}
mutant "scene submit does not write back"        macros.inc 's/VGL_STREAM_SCENE_SUBMIT() if (vgl_stream_unflushed) {.*$/VGL_STREAM_SCENE_SUBMIT() ;/'
mutant "scene submit forgets the dirty test"     macros.inc 's/VGL_STREAM_SCENE_SUBMIT() if (vgl_stream_unflushed)/VGL_STREAM_SCENE_SUBMIT() if (vgl_stream_cached)/'
mutant "draw entry still writes back in scene mode" macros.inc 's/ && !vgl_stream_scene_mode//'
mutant "flush stops one line short"              arena_under_test.inc 's/stream_flush_fn(from, to - from)/stream_flush_fn(from, to - from - 64)/'
mutant "flush stops one line short (sync)"       arena_under_test.inc 's/sceKernelSyncVMDomain(stream_uid, from, to - from)/sceKernelSyncVMDomain(stream_uid, from, to - from - 64)/'
mutant "flush starts one line late"              arena_under_test.inc 's/stream_flush_fn(from, to - from)/stream_flush_fn(from + 64, to - from - 64)/'
mutant "flush forgets what it flushed"           arena_under_test.inc 's/stream_flushed\[vgl_circular_idx\] = to;//'
mutant "touch does nothing"                      arena_under_test.inc 's/if (!vgl_stream_cached || stream_mem != 0)/if (1)/'
mutant "touch raises the mark instead of lowering it" arena_under_test.inc 's/if (a < stream_flushed\[vgl_circular_idx\])/if (a > stream_flushed[vgl_circular_idx])/'
mutant "reservation does not mark unflushed"     arena_under_test.inc '/if (stream_mem == 0)/{n;s/vgl_stream_unflushed = GL_TRUE;//}'
mutant "next slice keeps the old flushed mark"   arena_under_test.inc 's/stream_ptr\[vgl_circular_idx\] = stream_flushed\[vgl_circular_idx\] = stream_base\[vgl_circular_idx\]/stream_ptr[vgl_circular_idx] = stream_base[vgl_circular_idx]/'
mutant "next slice does not start empty (range reused while the GPU may read it)" arena_under_test.inc 's/stream_ptr\[vgl_circular_idx\] = stream_flushed\[vgl_circular_idx\] = stream_base\[vgl_circular_idx\]/(void)0/'
mutant "reservations padded to 4 bytes only" arena_under_test.inc 's/size = (size + 63u) \& ~(uintptr_t)63;/size = (size + 3u) \& ~(uintptr_t)3;/'
# source placement: the submit macro sits in gxm.c scene_end before sceGxmEndScene; no other path submits a scene
placed() { awk '/static inline __attribute__\(\(always_inline\)\) void scene_end\(void\)/{p=1} p{print} p&&/^}/{exit}' "$1" | awk '/VGL_STREAM_SCENE_SUBMIT\(\)/{a=NR} /sceGxmEndScene|vgl_rt_end_scene/{if(!b)b=NR} END{exit !(a&&b&&a<b)}'; }
placed "$SRC/gxm.c" && echo "scene_end: the write-back comes before the submit" || { echo "scene_end has no write-back before sceGxmEndScene"; bad=1; }
sed 's/VGL_STREAM_SCENE_SUBMIT() \/\/ 0060.*//' "$SRC/gxm.c" > "$O/gxm_mut.c"
if placed "$O/gxm_mut.c"; then echo "MUTANT SURVIVED: scene_end without the write-back"; bad=1; else echo "mutant killed: scene_end without the write-back"; fi
ends=$(grep -c 'sceGxmEndScene(' "$SRC/gxm.c"); rtc=$(grep -c 'vgl_rt_end_scene(' "$SRC/gxm.c")
[ "$ends" = 1 ] && [ "$rtc" = 1 ] && echo "gxm.c: one sceGxmEndScene and one vgl_rt_end_scene call, both in scene_end" || { echo "gxm.c has other scene submits ($ends, $rtc): each needs the write-back"; bad=1; }
grep -rn 'vgl_rt_end_scene(' "$SRC" --include=*.c | grep -v "^$SRC/rt.c\|^$SRC/gxm.c" && { echo "another caller of vgl_rt_end_scene"; bad=1; } || true
echo "checks done (bad=$bad)"
exit $bad
