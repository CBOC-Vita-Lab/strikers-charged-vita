#!/bin/bash
# nxr4a: the render target pool (vitaGL 0039) and the collector (gxm.c) on a model of sceGxm, ASan + UBSan, with mutants of the pool text (each must fail). The text under test is cut out of the real
# gxm.c and utils/gc_recovery.h. Usage: run_rtpool_gc_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_rtpool_gc_test}; rm -rf "$O"; mkdir -p "$O"
SRC="$H/../../dependencies/vitaGL/source"
cut() {   # dir
    python3 - "$SRC/gxm.c" "$SRC/utils/mem_utils.h" "$1" <<'PY'
import sys
g = open(sys.argv[1]).read(); mu = open(sys.argv[2]).read(); d = sys.argv[3]
def between(s, a, b, incl=False):
    i = s.index(a); j = s.index(b, i); return s[i:j]
open(d + "/counters.inc", "w").write(between(g, "static uint32_t vgl_gcj_jobs", '#include "utils/gc_recovery.h"'))
open(d + "/pool.inc", "w").write(between(g, "#define VGL_RTP_SLOTS", "// 0063 (vglFramebufferDualRT)"))
open(d + "/collector.inc", "w").write(between(g, "// Garbage collector\n", "GLboolean start_shader_compiler"))
assert "frame_rt_purge_list[frame_purge_idx][frame_rt_purge_idx++] = x" in mu
PY
}
build() { nice -n 15 gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -w -I"$1" -I"$SRC" "$H/rtpool_gc_model.c" -o "$2"; }
mkdir -p "$O/ok"; cut "$O/ok"
grep -q "vgl_rtp_release" "$O/ok/pool.inc" && grep -q "vglGetGcJobStats" "$O/ok/pool.inc" && grep -q "gcj_t0" "$O/ok/collector.inc" && grep -q "gc_enqueue" "$SRC/gxm.c" || { echo "text under test not found"; exit 2; }
build "$O/ok" "$O/ok/t"; "$O/ok/t" all 2>&1 | tail -20
bad=0; n=0
mutant() {   # name, file (pool|collector), python-free sed expression
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; cp "$O/ok/"*.inc "$d/"
    sed -i -e "$3" "$d/$2.inc"
    if cmp -s "$O/ok/$2.inc" "$d/$2.inc"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t" 2>/dev/null || { echo "MUTANT $1 does not compile (fix the mutant)"; bad=1; return; }
    if "$d/t" all > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1"; fi
    rm -f "$d/t"
}
mutant "a hit ignores the scene count"                    pool 's/s->h == h \&\& s->refs == refs \&\& s->mode == mode/s->h == h \&\& s->mode == mode/'
mutant "a hit ignores the samples"                        pool 's/s->refs == refs \&\& s->mode == mode/s->refs == refs/'
mutant "a hit does not mark the target as handed out"     pool '0,/\t\t\ts->state = 1;/s//\t\t\t(void)0;/'
mutant "a let-go target is not kept"                      pool '0,/\t\t\t\ts->state = 2;/s//\t\t\t\ts->state = 0;/'
mutant "the count cap is ignored"                         pool 's/vgl_rtp_stat\[4\] < vgl_rtp_max_count \&\& //'
mutant "the KiB cap is ignored"                           pool 's/(!vgl_rtp_max_kib || vgl_rtp_stat\[5\] + s->kib <= vgl_rtp_max_kib)/1/'
mutant "switching the pool off keeps what it holds"       pool 's/for (int i = 0; i < n; i++)\n\t\tsceGxmDestroyRenderTarget(gone\[i\]);//; s/\t\tsceGxmDestroyRenderTarget(gone\[i\]);/\t\t(void)gone[i];/'
mutant "a let-go target over the cap is not destroyed"    pool '0,/^\tsceGxmDestroyRenderTarget(rt);/s//\t(void)rt;/'
mutant "a let-go target is kept and also destroyed"       pool 's/^\t\t\t\tVGL_RTP_UNLOCK();\n\t\t\t\treturn;//; 0,/\t\t\t\tvgl_rtp_stat\[5\] += s->kib;/s//&\n\t\t\t\tsceGxmDestroyRenderTarget(rt);/'
mutant "the collector job destroys nothing (leak)"        collector 's/vgl_rtp_release(frame_rt_purge_list\[frame_purge_clean_idx\]\[i\]);/(void)0;/'
mutant "the 0064 destroy timer reads nothing"             pool 's/vgl_gcj_max(&vgl_gcj_destroy_max_us, gcj_us);/(void)gcj_us;/'
[ $bad = 0 ] && echo "rtpool_gc model: all mutants killed" || { echo "rtpool_gc model: a mutant was not caught"; exit 1; }
