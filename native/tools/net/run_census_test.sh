#!/bin/bash
# O9 (netclient13): the socket census after failing polls (tools/net/socket_census_test.cpp): HTTPS client + the online UDP socket, counted by np_net_census.h.
#   bash tools/net/run_census_test.sh [outdir]       needs the host32 mbedtls: MBEDTLS=<prefix> or the one tools/https/run_https_test.sh builds under $NP_MBEDTLS_WORK
#   MUTANT=1 bash tools/net/run_census_test.sh       builds with -DMUTANT_LEAK: the test must FAIL (exit 1)
set -e -o pipefail
cd "$(dirname "$0")/../.."
ROOT=$(cd .. && pwd)
OUT=${1:-${TMPDIR:-/tmp}/np-census-test}
mkdir -p "$OUT"
if [ -z "$MBEDTLS" ]; then
  W=${NP_MBEDTLS_WORK:-${TMPDIR:-/tmp}/np-mbedtls-work}
  [ -f "$W/mbedtls/host32/lib/libmbedtls.a" ] || python3 "$ROOT/tools/build-mbedtls.py" --target host32 --work "$W" > "$OUT.mbedtls.log" 2>&1 \
    || { tail -20 "$OUT.mbedtls.log"; echo "could not build the host32 Mbed TLS (see $OUT.mbedtls.log)"; exit 2; }
  MBEDTLS=$W/mbedtls/host32
fi
PFX=$MBEDTLS
[ -f "$PFX/lib/libmbedtls.a" ] || { echo "no $PFX/lib/libmbedtls.a"; exit 2; }
N=platform/sdk/net
EXTRA=""
[ -n "$MUTANT" ] && EXTRA="-DMUTANT_LEAK=1"
g++ -m32 -std=gnu++11 -fno-exceptions -fno-rtti -O1 -g -Wall -Wno-unused-parameter -idirafter /usr/include/x86_64-linux-gnu \
  -DNP_HAVE_MBEDTLS=1 -DNP_HTTPS_TEST_HOOKS=1 -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNDEBUG=1 $EXTRA -I. -Iinclude -I"$PFX/include" \
  $N/np_https.cpp $N/np_https_sys.cpp $N/np_https_tls_mbedtls.cpp $N/online/np_http.cpp $N/online/np_http_plain.cpp $N/online/np_udp_posix.cpp $N/online/np_net.cpp tools/net/socket_census_test.cpp \
  -L"$PFX/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -o "$OUT/socket_census_test" 2> "$OUT/build.err" || { head -40 "$OUT/build.err"; exit 1; }
"$OUT/socket_census_test" 2>&1 | grep -v '^\[net-https\]\|^\[np-net\]'
