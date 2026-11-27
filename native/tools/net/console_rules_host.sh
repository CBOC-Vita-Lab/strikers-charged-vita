#!/bin/bash
# A host game under the CONSOLE's online rules, from an existing host32 glnull build (no full rebuild: the online objects are compiled again and the
# game is linked again). What the console build has and a plain host build does not, and how this build gets it:
#   - HTTPS through np_https + Mbed TLS for every request (the plain desktop client is refused)      -DNP_HAVE_MBEDTLS=1 + the host32 prefix, -DNP_ONLINE_CONSOLE_URL_RULES=1
#   - a service address compiled into the build (np_service_url.h NP_ONLINE_URL)                    -DNP_ONLINE_URL="https://localhost:<port>" with URL_PORT=<port> (else MSC_ONLINE_URL names it)
#   - the socket layer without a link backend before a session (so_net.cpp)                         -DNP_SO_CONSOLE_LINK_RULES=1
# The local fake's certificate is trusted through NP_HTTPS_TEST_ROOTS=<root.pem> (-DNP_HTTPS_TEST_HOOKS=1: host test builds only; verification stays on).
#   tools/net/console_rules_host.sh <build root of a finished host32 glnull build> <out exe> [objdir]
#   MBEDTLS=<host32 prefix> (default ${NP_MBEDTLS_WORK:-/tmp/np-mbedtls-work}/mbedtls/host32)   URL_PORT=<https port of the fake>   SRC=<tree to take the online sources from, default this one>
set -e
cd "$(dirname "$0")/../.."
BR=$1; EXE=$2; OBJ=${3:-$(dirname "$EXE")/obj-$(basename "$EXE")}
PFX=${MBEDTLS:-${NP_MBEDTLS_WORK:-/tmp/np-mbedtls-work}/mbedtls/host32}
SRC=${SRC:-.}
[ -f "$BR/host32/np-host32-full-glnull" ] && [ -f "$PFX/lib/libmbedtls.a" ] || { echo "need a finished host32 glnull build in $BR and Mbed TLS in $PFX"; exit 2; }
mkdir -p "$OBJ"
CF="-m32 -msse2 -mfpmath=sse -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNP_DISC_HOST_ORDER=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 -DdNODEBUG=1 -DdSINGLE=1 \
 -Iplatform/gx/include -I$SRC -I. -Iinclude -Isrc/ode -Isrc/zlib -Ilibs/RVL_SDK/include -Iref/RVL_SDK -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h -fno-math-errno -std=gnu++11 -fno-exceptions -fno-rtti \
 -Wno-c++11-narrowing -Wno-enum-constexpr-conversion -fno-strict-return -O2 -g0 -Iplatform/sdk/include -funsigned-char"
RULES="-DNP_ONLINE_CONSOLE_URL_RULES=1 -DNP_SO_CONSOLE_LINK_RULES=1"
[ -n "$URL_PORT" ] && RULES="$RULES -DNP_ONLINE_URL=\"https://localhost:$URL_PORT\""
TLS="-DNP_HAVE_MBEDTLS=1 -DNP_HTTPS_TEST_HOOKS=1 -I$PFX/include"
# every online source (so a build root made before an online change still gives today's code): dwc/, net/online/, and the net/ files that carry a rule
LIST="net/np_menu_flow net/so_net net/np_mpreport_net net/np_https net/np_https_sys net/np_https_tls_mbedtls net/np_https_selftest"
for f in "$SRC"/platform/sdk/dwc/*.cpp "$SRC"/platform/sdk/net/online/*.cpp; do f=${f#"$SRC"/platform/sdk/}; LIST="$LIST ${f%.cpp}"; done
SWAP=""
for f in $LIST; do
    grep -q "$BR/host32/pobj/platform/sdk/$f.cpp.o" "$BR.build.log" || continue   # not part of this build
    o="$OBJ/$(basename $f).o"
    extra="$RULES"; case $f in net/np_https*) extra="$TLS";; esac
    clang++ $CF $extra -c "$SRC/platform/sdk/$f.cpp" -o "$o"
    SWAP="$SWAP $f"
done
# the build's own link line with those objects swapped and the TLS library added
LD=$(grep -m1 "^clang++ -o $BR/host32/np-host32-full-glnull " "$BR.build.log" | sed -e 's/ 2> .*//')
[ -n "$LD" ] || { echo "no link line in $BR.build.log"; exit 2; }
for f in $SWAP; do LD=${LD/"$BR/host32/pobj/platform/sdk/$f.cpp.o"/"$OBJ/$(basename $f).o"}; done
LD=${LD/"-o $BR/host32/np-host32-full-glnull"/"-o $EXE"}
eval "$LD $PFX/lib/libmbedtls.a $PFX/lib/libmbedx509.a $PFX/lib/libmbedcrypto.a" 2> "$OBJ/link.err" || { grep -v 'undefined reference' "$OBJ/link.err" | head; grep -c 'undefined reference' "$OBJ/link.err"; }
[ -x "$EXE" ] && echo "built $EXE (console online rules${URL_PORT:+, service address compiled in: https://localhost:$URL_PORT})"
