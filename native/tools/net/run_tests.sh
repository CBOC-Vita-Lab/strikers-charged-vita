#!/bin/bash
# Builds and runs the tests of platform/sdk/net on host32 (clang -m32, the np build's flags). No game data; a few seconds.
#   tools/net/run_tests.sh [outdir]        default /work/msc/build/npm/net
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/work/msc/build/npm/net}
mkdir -p "$OUT"
FL="${M:--m32} -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h \
 -std=gnu++11 -fno-exceptions -fno-rtti -fdeclspec -O1 -g1"
clang++ $FL platform/sdk/net/vlink.cpp platform/sdk/net/vlink_udp.cpp platform/sdk/net/so_net.cpp tools/net/net_test.cpp \
  platform/compat/np_compat.c platform/compat/np_gamefixes.cpp -x c++ -o "$OUT/net_test" -Wl,-z,muldefs -lpthread 2>"$OUT/build.err" || { head -30 "$OUT/build.err"; exit 1; }
"$OUT/net_test"
# the rule fingerprint: rule options (NK fix, No Mega Strikes) change it, drawing-only options do not
fp() { env -u MSC_LINK_FINGERPRINT "$@" "$OUT/net_test" --fingerprint; }
f0=$(fp); fn=$(fp MSC_RULE_NOMEGA=1); fz=$(fp MSC_RULE_NOMEGA=0); fk=$(fp MSC_GAME_FIXES=nk); fd=$(fp MSC_GAME_FIXES=net,electro); fb=$(fp MSC_RULE_NOMEGA=1 MSC_GAME_FIXES=nk)
echo "fingerprints: default $f0, nomega $fn, nomega=0 $fz, nk $fk, drawing fixes $fd, nomega+nk $fb"
[ "$f0" = "$fz" ] && [ "$f0" = "$fd" ] && [ "$f0" != "$fn" ] && [ "$f0" != "$fk" ] && [ "$fn" != "$fk" ] && [ "$fb" != "$fn" ] && [ "$fb" != "$fk" ] \
  || { echo "FAIL: rule fingerprint"; exit 1; }
echo "rule fingerprint: ok"
# the build: another build id or version is another fingerprint; MSC_LAN_EXACT_BUILD=0 is the fingerprint without one
fx=$(fp MSC_LAN_EXACT_BUILD=0)
clang++ $FL -DNP_BUILD_ID='"other-build"' platform/sdk/net/vlink.cpp platform/sdk/net/vlink_udp.cpp platform/sdk/net/so_net.cpp tools/net/net_test.cpp \
  platform/compat/np_compat.c platform/compat/np_gamefixes.cpp -x c++ -o "$OUT/net_test_b" -Wl,-z,muldefs -lpthread 2>>"$OUT/build.err" || { head -30 "$OUT/build.err"; exit 1; }
fo=$(env -u MSC_LINK_FINGERPRINT "$OUT/net_test_b" --fingerprint); fox=$(env -u MSC_LINK_FINGERPRINT MSC_LAN_EXACT_BUILD=0 "$OUT/net_test_b" --fingerprint)
echo "build fingerprints: this build $f0, another build $fo, without the build $fx / $fox"
[ "$f0" != "$fo" ] && [ "$f0" != "$fx" ] && [ "$fx" = "$fox" ] || { echo "FAIL: build fingerprint"; exit 1; }
echo "build fingerprint: ok"
# (mpfix18) the keep-stack switch and the stranger-line limiter, and the vlink mutants (each 1.2 behaviour put back must fail net_test)
clang++ -std=gnu++11 -I. tools/net/mpfix18_test.cpp -o "$OUT/mpfix18_test" && "$OUT/mpfix18_test"
bash tools/net/mpfix18_mutants.sh "$OUT/mut"
# (onlogin23) the socket layer in the online screens before a session exists: each part taken away must fail net_test
bash tools/net/onlogin23_mutants.sh "$OUT/mut23"
clang++ -std=gnu++11 -I. tools/net/delay19_test.cpp -o "$OUT/delay19_test" && "$OUT/delay19_test"
bash tools/net/delay19_mutants.sh "$OUT/mut19"
# (wake14) the Wi-Fi keep-awake filler of vlink.cpp (MSC_NET_KEEPAWAKE_MS): the test and its mutants
clang++ -std=gnu++11 -I. platform/sdk/net/vlink.cpp tools/net/wake14_test.cpp -o "$OUT/wake14_test" && "$OUT/wake14_test" 2>/dev/null
bash tools/net/wake14_mutants.sh "$OUT/mut14" 2>&1 | grep -v '^\[vlink\]'
