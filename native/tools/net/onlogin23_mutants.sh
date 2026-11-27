#!/bin/bash
# Mutation check of the online-screens socket start (so_net.cpp, onlogin23): each mutant takes one part of it away in a copy of so_net.cpp and net_test MUST then fail.
# The first mutant is the 1.3 console before the fix (the login's SOStartup had no backend): the failure of the first console run of Online Central.
#   tools/net/onlogin23_mutants.sh [outdir]      (host32; run from anywhere)
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/onlogin23-mut}; mkdir -p "$OUT"
FL="-m32 -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h \
 -std=gnu++11 -fno-exceptions -fno-rtti -fdeclspec -O1 -g1"
bad=0
run() {  # name, python edit (old -> new) applied to so_net.cpp
  python3 - "$1" "$2" "$3" "$OUT/so_net_mut.cpp" <<'PY' || { echo "mutant $1: pattern not found"; bad=1; return; }
import sys
name, old, new, out = sys.argv[1:5]
s = open('platform/sdk/net/so_net.cpp').read()
if old not in s: sys.exit(1)
open(out, 'w').write(s.replace(old, new, 1))
PY
  clang++ $FL platform/sdk/net/vlink.cpp platform/sdk/net/vlink_udp.cpp "$OUT/so_net_mut.cpp" tools/net/net_test.cpp platform/compat/np_compat.c platform/compat/np_gamefixes.cpp \
    -x c++ -o "$OUT/net_test_mut" -Wl,-z,muldefs -lpthread 2>"$OUT/build.err" || { head -20 "$OUT/build.err"; bad=1; return; }
  if "$OUT/net_test_mut" >"$OUT/run.txt" 2>&1; then echo "mutant $1: SURVIVED (test did not catch it)"; bad=1; else echo "mutant $1: caught ($(grep -a -c '^FAIL' "$OUT/run.txt") failed checks)"; fi
}
run before-the-fix 'if (OnlineScreensOpen()) InstallPendingOnline();' ';'
run address-outside-the-screens 'return OnlineScreensOpen() ? kPendingOnlineVip : 0;' 'return kPendingOnlineVip;'
run always-installed 'if (OnlineScreensOpen()) InstallPendingOnline();' 'InstallPendingOnline();'
run left-installed 'if (L().GetBackend() == &kPendingOnline) L().SetBackend(g_replacedByPending);' ';'
run forgets-replaced 'L().SetBackend(g_replacedByPending);' 'L().SetBackend(0);'
run over-a-session 'if (now && NpNetActive && NpNetActive()) return;' ';'
run sends-succeed 'int PendingSend(void*, const VLink::PeerAddr*, bool, const void*, size_t) { return -1; }' 'int PendingSend(void*, const VLink::PeerAddr*, bool, const void*, size_t len) { return int(len); }'
[ $bad = 0 ] && echo "all mutants caught"
exit $bad
