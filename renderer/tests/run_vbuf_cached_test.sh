#!/bin/bash
# Host test of the cached stream arena of vitaGL (vgl.c) against a fake data cache, with mutants (each must fail). Usage: run_vbuf_cached_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_vbuf_cached_test}; rm -rf "$O"; mkdir -p "$O"
VGL="$H/../../dependencies/vitaGL/source/vgl.c"
cut_arena() { # $1 = out file; the real text between the two markers
    # the one edit: `~63u` is 32 bit wide, which is the pointer width on the Vita; on a 64 bit host it would clear the pointer's high half
    awk '/^\/\/ ---- GLB: cached stream arena/{p=1} /^void vglSetupCachedStream/{p=0} p' "$VGL" | sed 's/~63u/~(uintptr_t)63/g' > "$1"
}
build() { nice -n 15 g++ -std=gnu++17 -O1 -fsanitize=address,undefined -DHAVE_SCRATCH_MEMORY $3 -I"$1" "$H/vbuf_cached.cpp" -o "$2"; }
mkdir -p "$O/ok"; cut_arena "$O/ok/arena_under_test.inc"
test -s "$O/ok/arena_under_test.inc" || { echo "markers not found in vgl.c"; exit 2; }
build "$O/ok" "$O/ok/t" ""; "$O/ok/t" 2>&1 | tail -3
bad=0; n=0
mutant() { # name sed-expression [extra g++ flag]
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; sed -e "$2" "$O/ok/arena_under_test.inc" > "$d/arena_under_test.inc"
    if [ -z "${3:-}" ] && cmp -s "$O/ok/arena_under_test.inc" "$d/arena_under_test.inc"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t" "${3:-}" 2>/dev/null
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1"; fi
}
mutant "draw entry does not flush"        's/XX_NOTHING_XX/&/' "-DVBUF_MUTANT_DRAW_NO_FLUSH"
mutant "touch does nothing"               's/if (!vgl_stream_cached || stream_mem != 0)/if (1)/'
mutant "flush stops one line short"       's/stream_flush_fn(from, to - from)/stream_flush_fn(from, to - from - 64)/'
mutant "flush stops one line short (sync)" 's/sceKernelSyncVMDomain(stream_uid, from, to - from)/sceKernelSyncVMDomain(stream_uid, from, to - from - 64)/'
mutant "flush forgets what it flushed"    's/stream_flushed\[vgl_circular_idx\] = to;//'
mutant "reservation does not mark unflushed" 's/vgl_stream_unflushed = GL_TRUE;\n\t\treturn p;/X/;/if (stream_mem == 0)/{n;s/vgl_stream_unflushed = GL_TRUE;//}'
mutant "next slice keeps the old flushed mark" 's/stream_ptr\[vgl_circular_idx\] = stream_flushed\[vgl_circular_idx\] = stream_base\[vgl_circular_idx\]/stream_ptr[vgl_circular_idx] = stream_base[vgl_circular_idx]/'
mutant "touch raises the mark instead of lowering it" 's/if (a < stream_flushed\[vgl_circular_idx\])/if (a > stream_flushed[vgl_circular_idx])/'
mutant "write-back is a no-op"            's/stream_flush_fn(from, to - from);/;/;s/sceKernelSyncVMDomain(stream_uid, from, to - from);/;/'
# patch 0054 (draw.c / buffers.c): every draw entry flushes the arena, every map call and the unmap mark the buffer written again (source check, with mutants)
SRC="$H/../../dependencies/vitaGL/source"
body() { awk -v f="$2" '$0 ~ "^(inline )?(void|GLboolean|void \\*) ?\\*?"f"\\(" {p=1} p{print} p&&/^}/{exit}' "$1"; }
covered() { # file, function, needle, [text file to read instead of the real one]
    local f=${4:-$SRC/$1}; local b; b=$(body "$f" "$2"); [ -n "$b" ] && echo "$b" | grep -q "$3"; }
for fn in glDrawArrays glMultiDrawArrays glDrawArraysInstanced glDrawElements glDrawElementsBaseVertex glDrawRangeElements glDrawRangeElementsBaseVertex glDrawElementsInstanced vglDrawObjects; do
    covered draw.c $fn VGL_STREAM_FLUSH_IF_NEEDED || { echo "draw entry without the arena write-back: $fn"; bad=1; }
done
for fn in glMapNamedBuffer glMapNamedBufferRange glUnmapNamedBuffer; do
    covered buffers.c $fn vgl_stream_touch || { echo "map call without vgl_stream_touch: $fn"; bad=1; }
done
sed '0,/VGL_STREAM_FLUSH_IF_NEEDED()/s///' "$SRC/draw.c" > "$O/draw_mut.c"
if covered draw.c glDrawArrays VGL_STREAM_FLUSH_IF_NEEDED "$O/draw_mut.c"; then echo "MUTANT SURVIVED: draw entry check"; bad=1; else echo "mutant killed: draw entry check"; fi
sed '0,/vgl_stream_touch(gpu_buf->ptr); \/\/ 0054: the writes/s///' "$SRC/buffers.c" > "$O/buf_mut.c"
if covered buffers.c glUnmapNamedBuffer vgl_stream_touch "$O/buf_mut.c"; then echo "MUTANT SURVIVED: unmap check"; bad=1; else echo "mutant killed: unmap check"; fi
echo "coverage checks done (bad=$bad)"
exit $bad
