#!/bin/bash
# nxgl25 (vitaGL 0061): host test of wrcopy.c's copy selection (MSC_VITA_WRCOPY=3, MSC_VITA_WRCOPY_FB), with mutants (each must fail). The text under test is cut out of the real wrcopy.c.
# Also checks that the start-up benchmark of vgx_host_vita.cpp still reads back every variant, mode 3 included. Usage: run_wrcopy_pick_test.sh [outdir]
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=${1:-/tmp/np_wrcopy_pick_test}; rm -rf "$O"; mkdir -p "$O"
SRC="$H/../../dependencies/vitaGL/source/utils/wrcopy.c"; HOST="$H/../vita/vgx_host_vita.cpp"
cut_text() { # the fallback switch + vglWriteCopyVariant (up to vglSetWriteCopy), then vgl_wr_copy (to the end of the file)
    { awk '/^static volatile int vgl_wr_fb/{p=1} /^void vglSetWriteCopy\(int mode\)/{p=0} p' "$SRC"; awk '/^void vgl_wr_copy\(/{p=1} p' "$SRC"; } | sed 's/^#ifndef vgl_fast_memcpy.*//' > "$1/under_test.inc"; }
build() { nice -n 15 g++ -std=gnu++17 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$1" "$H/wrcopy_pick.cpp" -o "$2"; }
mkdir -p "$O/ok"; cut_text "$O/ok"
grep -q "vgl_wr_fb" "$O/ok/under_test.inc" && grep -q "vgl_wr_copy" "$O/ok/under_test.inc" && grep -q "sceDmacMemcpy" "$O/ok/under_test.inc" || { echo "text under test not found"; exit 2; }
build "$O/ok" "$O/ok/t"; "$O/ok/t" 2>&1 | tail -3
bad=0; n=0
mutant() { # name, sed expression
    n=$((n+1)); local d="$O/m$n"; mkdir -p "$d"; cp "$O/ok/under_test.inc" "$d/"; sed -i -e "$2" "$d/under_test.inc"
    if cmp -s "$O/ok/under_test.inc" "$d/under_test.inc"; then echo "mutant $1: nothing changed"; bad=1; return; fi
    build "$d" "$d/t" 2>/dev/null || { echo "MUTANT $1 does not compile (fix the mutant)"; bad=1; return; }
    if "$d/t" > "$d/out" 2>&1; then echo "MUTANT SURVIVED: $1"; bad=1; else echo "mutant killed: $1"; fi
}
mutant "fallback always plain (0045)"                    's/^\t\tif (!vgl_wr_fb) {/\t\tif (1) {/'
mutant "fallback never plain (switch ignored)"           's/^\t\tif (!vgl_wr_fb) {/\t\tif (0) {/'
mutant "fallback goes to the 128 byte stores"            's/^\t\tmode = 1;/\t\tmode = 2;/'
mutant "DMA threshold 512"                               's/if (n >= 0x2000u) {/if (n >= 0x200u) {/'
mutant "DMA threshold 16 KiB"                            's/if (n >= 0x2000u) {/if (n >= 0x4000u) {/'
mutant "DMA without the source address test"             's/VGL_WR_ADDR(s) < 0x81000000u \&\& //'
mutant "DMA without the destination address test"        's/ \&\& VGL_WR_ADDR(d) < 0x81000000u//'
mutant "DMA ignores end of span"                         's/ \&\& n <= 0x81000000u - VGL_WR_ADDR(s) \&\& n <= 0x81000000u - VGL_WR_ADDR(d)//'
mutant "address limit is inclusive"                      's/n <= 0x81000000u - VGL_WR_ADDR(s)/n <= 0x81000001u - VGL_WR_ADDR(s)/'
mutant "address limit one MiB high"                      's/VGL_WR_ADDR(d) < 0x81000000u/VGL_WR_ADDR(d) < 0x81100000u/'
mutant "DMA error is not a fallback (copy lost)"         's/^\t\t\t\tvgl_wr_stat\[7\]++;/\t\t\t\tvgl_wr_stat[7]++; return;/'
mutant "DMA taken in mode 1 too"                         's/^\tif (mode == 3) {/\tif (mode >= 1 \&\& mode <= 3) {/'
mutant "DMA counter not counted"                         's/vgl_wr_stat\[4\]++;//'
mutant "DMA bytes counted short"                         's/vgl_wr_stat\[5\] += n;/vgl_wr_stat[5] += n - 1;/'
mutant "address refusal not counted"                     's/vgl_wr_stat\[6\]++;/vgl_wr_stat[6] += 0;/'
mutant "error not counted"                               's/vgl_wr_stat\[7\]++;//'
mutant "head aligned to 32 not 64"                       's/(-(intptr_t)d) \& 63u/(-(intptr_t)d) \& 31u/'
mutant "no head copy (unaligned stores)"                 's/^\tif (head) {/\tif (0) {/'
mutant "tail dropped"                                    's/^\tif (n)$/\tif (0)/'
mutant "vgl_wr_copy small limit 64"                      's/n < VGL_WRCOPY_MIN/n < 64u/'
# (nxr6a: the mutant "vgl_wr_copy mode 0 not plain" is gone: since the variant counts the branch it takes, mode 0 through the variant is observably the same plain copy -- an equivalent mutant)
mutant "vgl_wr_copy counts [0]/[1] before the dispatch (nxgl25)" 's/^\tvglWriteCopyVariant(m, dst, src, n);.*/\tvgl_wr_stat[0]++; vgl_wr_stat[1] += n;\n\tvglWriteCopyVariant(m, dst, src, n);/'
mutant "FB=0 plain fallback not counted as plain"        '/^\t\tif (!vgl_wr_fb) {/,/return;/ s/vgl_wr_stat\[3\] += n;//'
mutant "NEON copies not counted"                         's/^\tvgl_wr_stat\[0\]++; \/\/ 0061.*$//'
mutant "short NEON copy counted as NEON"                 '/^\tif (n < head + unit) {/,/return;/ s/vgl_wr_stat\[2\]++;/vgl_wr_stat[0]++;/'
# the start-up benchmark still reads back every variant (0..3) for the large and the 768 byte copies
b="$(awk '/static void WriteCopySetup\(\)/{p=1} p{print} /^}$/{if(p)exit}' "$HOST")"
echo "$b" | grep -q 'for (int v = 0; v < 4; ++v)' && echo "$b" | grep -q 'vglWriteCopyVariant(v, dst + 4096 + 32' && echo "$b" | grep -q 'std::memcmp(dst + 4096 + 32, src.data(), kBytes - 4096) != 0) ok\[v\] = false' \
  && echo "$b" | grep -q 'std::memcmp(dst + 512u \* 1024u' && echo "$b" | grep -q 'ok\[v\] && mbs\[v\]' && echo "start-up benchmark: every variant incl. mode 3 is compared and a mismatch disqualifies it" || { echo "start-up benchmark check FAILED"; bad=1; }
if echo "$b" | sed 's/for (int v = 0; v < 4; ++v)/for (int v = 0; v < 3; ++v)/' | grep -q 'for (int v = 0; v < 4; ++v)'; then echo "MUTANT SURVIVED: benchmark skips mode 3"; bad=1; else echo "mutant killed: benchmark skips mode 3"; fi
rm -rf "$O"/m* 2>/dev/null || true
[ $bad = 0 ] && echo "ALL OK ($n mutants killed)" || { echo "FAILED"; exit 1; }
