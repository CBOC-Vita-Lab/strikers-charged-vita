#!/bin/bash
# Mutation check of the mpfix18 vlink changes: each mutant puts the 1.2 behaviour (or no limit) back into a copy of vlink.cpp and net_test MUST then fail.
#   tools/net/mpfix18_mutants.sh [outdir]      (host32; run from anywhere)
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/mpfix18-mut}; mkdir -p "$OUT"
FL="-m32 -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h \
 -std=gnu++11 -fno-exceptions -fno-rtti -fdeclspec -O1 -g1"
bad=0
run() {  # name, python edit (old -> new) applied to vlink.cpp
  python3 - "$1" "$2" "$3" "$OUT/vlink_mut.cpp" <<'PY' || { echo "mutant $1: pattern not found"; bad=1; return; }
import sys
name, old, new, out = sys.argv[1:5]
s = open('platform/sdk/net/vlink.cpp').read()
if old not in s: sys.exit(1)
open(out, 'w').write(s.replace(old, new, 1))
PY
  clang++ $FL "$OUT/vlink_mut.cpp" platform/sdk/net/vlink_udp.cpp platform/sdk/net/so_net.cpp tools/net/net_test.cpp platform/compat/np_compat.c platform/compat/np_gamefixes.cpp \
    -x c++ -o "$OUT/net_test_mut" -Wl,-z,muldefs -lpthread 2>"$OUT/build.err" || { head -20 "$OUT/build.err"; bad=1; return; }
  if "$OUT/net_test_mut" >"$OUT/run.txt" 2>&1; then echo "mutant $1: SURVIVED (test did not catch it)"; bad=1; else echo "mutant $1: caught ($(grep -a -c '^FAIL' "$OUT/run.txt") failed checks)"; fi
}
run queue-pop-front 'if (own >= kQueueLimit / 8) drop = firstOwn;' 'drop = 0;'
run queue-never-own 'if (own >= kQueueLimit / 8) drop = firstOwn;' 'if (own >= kQueueLimit / 8 + 200) drop = firstOwn;'
run reasm-oldest 'if (c > victimCount) { victimCount = c; victimFrom = &reasm[i].from; }' ';'
run reasm-ignore-sender 'const PeerAddr* victimFrom = mine ? &from : 0;' 'const PeerAddr* victimFrom = 0; victimCount = 0;'
run log-unlimited 'if (NpMpfix18::LogAllow(lb_, nowMs(nowCtx))) VLINK_LOG' 'if ((void)lb_, true) VLINK_LOG'
[ $bad = 0 ] && echo "all mutants caught"
exit $bad
