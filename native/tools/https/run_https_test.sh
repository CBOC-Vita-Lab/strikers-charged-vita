#!/bin/bash
# Builds and runs the host tests of platform/sdk/net/np_https (HTTPS client) against a local HTTPS server with a private test CA.
#   tools/https/run_https_test.sh [outdir]
#   MBEDTLS=<prefix>  the host32 prefix made by `python3 tools/build-mbedtls.py --target host32` (<work>/mbedtls/host32).
#                     Unset: the prefix is built (or reused) under ${NP_MBEDTLS_WORK:-$TMPDIR/np-mbedtls-work}; that needs the pinned source
#                     (a git fetch of the Mbed TLS tag, once) and takes a few minutes the first time.
# Needs python3 with the `cryptography` package (the server makes its certificates on the fly: nothing secret is committed).
# About a minute of wall time, mostly deliberate waits (time-outs). Run it through your build lock if the machine is shared
set -e
cd "$(dirname "$0")/../.."
ROOT=$(cd .. && pwd)
OUT=${1:-${TMPDIR:-/tmp}/np-https-tests}
if [ -z "$MBEDTLS" ]; then
  W=${NP_MBEDTLS_WORK:-${TMPDIR:-/tmp}/np-mbedtls-work}
  [ -f "$W/mbedtls/host32/lib/libmbedtls.a" ] || python3 "$ROOT/tools/build-mbedtls.py" --target host32 --work "$W" > "$OUT.mbedtls.log" 2>&1 \
    || { tail -20 "$OUT.mbedtls.log"; echo "could not build the host32 Mbed TLS (see $OUT.mbedtls.log)"; exit 2; }
  MBEDTLS=$W/mbedtls/host32
fi
PFX=$MBEDTLS
[ -f "$PFX/lib/libmbedtls.a" ] || { echo "no $PFX/lib/libmbedtls.a: run tools/build-mbedtls.py --target host32"; exit 2; }
rm -rf "$OUT/run"; mkdir -p "$OUT/run"
N=platform/sdk/net
g++ -m32 -std=gnu++11 -fno-exceptions -fno-rtti -O1 -g -Wall -Wno-unused-parameter -idirafter /usr/include/x86_64-linux-gnu \
  -DNP_HAVE_MBEDTLS=1 -DNP_HTTPS_TEST_HOOKS=1 -DNP_TARGET_HOST32=1 -I. -I"$PFX/include" \
  $N/np_https.cpp $N/np_https_sys.cpp $N/np_https_tls_mbedtls.cpp $N/online/np_http.cpp $N/online/np_http_plain.cpp tools/net/https_test.cpp \
  -L"$PFX/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -o "$OUT/https_test" 2> "$OUT/build.err" || { head -40 "$OUT/build.err"; exit 1; }
python3 -I tools/net/https_server.py "$OUT/run" > "$OUT/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
for i in $(seq 1 100); do [ -f "$OUT/run/ports.json" ] && break; kill -0 $SERVER 2>/dev/null || { cat "$OUT/server.log"; exit 1; }; sleep 0.2; done
[ -f "$OUT/run/ports.json" ] || { echo "server did not start"; cat "$OUT/server.log"; exit 1; }
"$OUT/https_test" "$OUT/run"
# (netclient13, O9) the socket census after failing polls: HTTPS client + online UDP socket, count back to zero (tools/net/socket_census_test.cpp)
MBEDTLS=$PFX bash tools/net/run_census_test.sh "$OUT/census"
