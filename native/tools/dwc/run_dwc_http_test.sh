#!/bin/bash
# Host test of the HTTP client against the service's local fake (node >= 22.5, server worktree with node_modules). Starts three fakes on
# loopback ports, runs tools/dwc/http_test, stops the fakes it started.
#   tools/dwc/run_dwc_http_test.sh [server dir] [outdir]   (server dir = the folder with fake/server.mjs; default <repo>/server)
set -e
cd "$(dirname "$0")/../.."
SERVER=${1:-$(git rev-parse --show-toplevel)/server}
if [ ! -f "$SERVER/fake/server.mjs" ] || ! command -v node >/dev/null; then echo "skipped: no service fake at $SERVER (or no node)"; exit 0; fi
OUT=${2:-${TMPDIR:-/tmp}/msc_dwc_test}
mkdir -p "$OUT"
FL="-m32 -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h \
 -std=gnu++11 -fno-exceptions -fno-rtti -fdeclspec -O1 -g1"
D=platform/sdk/dwc
clang++ $FL $D/np_online_api.cpp $D/np_online_fake.cpp $D/np_online_queue.cpp $D/np_online_json.cpp $D/np_online_http.cpp $D/np_online_store.cpp \
  $D/np_online_name.cpp $D/np_dwc_account.cpp $D/np_dwc_error.cpp $D/np_dwc_login.cpp $D/np_dwc_friend.cpp $D/np_dwc_rank.cpp platform/compat/np_gamefixes.cpp \
  tools/dwc/http_test.cpp -o "$OUT/http_test" -lpthread 2>"$OUT/http_build.err" || { head -60 "$OUT/http_build.err"; exit 1; }
P1=$((20000 + RANDOM % 5000)); P2=$((P1 + 1)); P3=$((P1 + 2)); P4=$((P1 + 3)); DEAD=$((P1 + 4))
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM
start() {   # port, env...
  local port=$1; shift
  ( cd "$SERVER" && exec env "$@" node fake/server.mjs --http-port "$port" --https-port $((port + 100)) >"$OUT/fake_$port.log" 2>&1 ) &
  PIDS+=($!)
}
start $P1 RANK_PUT_POLICY=credit
start $P2 RANK_PUT_POLICY=open
start $P3 MIN_CLIENT_PROTO=2 LATEST_APP_VERSION=1.2.0 RANK_PUT_POLICY=open
P5=$((P1 + 5)); P6=$((P1 + 6))   # (gxnet14) the proof-of-work fakes: a rising price from 9 bits, and 20 bits (above the client's limit)
start $P5 REGISTER_POW_BITS=9 REGISTER_POW_FREE=1 REGISTER_IP_BURST=50 REGISTER_IP_PER_HOUR=50
start $P6 REGISTER_POW_BITS=20
for port in $P1 $P2 $P3 $P5 $P6; do
  for i in $(seq 1 100); do curl -sf "http://127.0.0.1:$port/v1/info" >/dev/null 2>&1 && break; sleep 0.2; done
done
"$OUT/http_test" "http://127.0.0.1:$P1" "http://127.0.0.1:$P2" "http://127.0.0.1:$P3" "http://127.0.0.1:$DEAD" "http://127.0.0.1:$P5" "http://127.0.0.1:$P6"
