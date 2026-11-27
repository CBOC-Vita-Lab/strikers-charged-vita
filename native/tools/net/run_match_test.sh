#!/bin/bash
# onmatch24: the ranked / quick queue under the console's rules against the local TLS fake (tools/net/https_server.py), and its mutants.
#   tools/net/run_match_test.sh [outdir]      MBEDTLS=<host32 prefix> (default ${NP_MBEDTLS_WORK:-/tmp/np-mbedtls-work}/mbedtls/host32)
# Needs python3 with `cryptography` (the server makes its certificates). About a minute. Skips (exit 0, says so) without Mbed TLS.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-${TMPDIR:-/tmp}/onmatch24-test}
PFX=${MBEDTLS:-${NP_MBEDTLS_WORK:-/tmp/np-mbedtls-work}/mbedtls/host32}
[ -f "$PFX/lib/libmbedtls.a" ] || { echo "skipped: no host32 Mbed TLS in $PFX (tools/build-mbedtls.py --target host32)"; exit 0; }
mkdir -p "$OUT"
FL="-m32 -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Isrc/ode -Ilibs/RVL_SDK/include -Ilibs/Runtime/include -idirafter libs/MSL_C/include -idirafter libs/MetroTRK/include \
 -fno-strict-aliasing -fwrapv -fno-short-enums -fshort-wchar -fno-delete-null-pointer-checks -w -include np/compat.h \
 -std=gnu++11 -fno-exceptions -fno-rtti -fdeclspec -O1 -g1 -DNP_ONLINE_CONSOLE_URL_RULES=1 -DNP_HAVE_MBEDTLS=1 -DNP_HTTPS_TEST_HOOKS=1 -I$PFX/include"
D=platform/sdk/dwc; N=platform/sdk/net
build() {   # exe, a folder with changed copies of sources (same layout; may be empty)
  local exe=$1 ov=$2 srcs="" f
  for f in $D/np_online_api.cpp $D/np_online_fake.cpp $D/np_online_queue.cpp $D/np_online_json.cpp $D/np_online_http.cpp $D/np_online_store.cpp $D/np_online_name.cpp \
           $D/np_dwc_account.cpp $D/np_dwc_error.cpp $D/np_dwc_login.cpp $D/np_dwc_friend.cpp $D/np_dwc_rank.cpp $D/np_dwc_match.cpp $D/np_online_https.cpp \
           $N/online/np_http.cpp $N/online/np_http_plain.cpp $N/np_https.cpp $N/np_https_sys.cpp $N/np_https_tls_mbedtls.cpp platform/compat/np_gamefixes.cpp tools/net/onmatch24_test.cpp; do
    if [ -n "$ov" ] && [ -f "$ov/$f" ]; then srcs="$srcs $ov/$f"; else srcs="$srcs $f"; fi
  done
  clang++ $FL $srcs -L"$PFX/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -o "$exe" 2> "$exe.err" || { head -40 "$exe.err"; return 1; }
}
SERVER=""
fresh_server() {   # a new fake (its record of requests is global)
  [ -n "$SERVER" ] && { kill $SERVER 2>/dev/null || true; wait $SERVER 2>/dev/null || true; }
  rm -rf "${OUT:?}/run"; mkdir -p "$OUT/run"
  python3 -I tools/net/https_server.py "$OUT/run" > "$OUT/server.log" 2>&1 &
  SERVER=$!
  for i in $(seq 1 100); do [ -f "$OUT/run/ports.json" ] && break; kill -0 $SERVER 2>/dev/null || { cat "$OUT/server.log"; exit 1; }; sleep 0.2; done
  [ -f "$OUT/run/ports.json" ] || { echo "server did not start"; exit 1; }
}
trap '[ -n "$SERVER" ] && kill $SERVER 2>/dev/null || true' EXIT
build "$OUT/onmatch24_test" ""
fresh_server
"$OUT/onmatch24_test" "$OUT/run" 2>"$OUT/test.log" | tee "$OUT/test.out"
echo "== mutants: each must fail the test =="
mutant() {   # name, text the failure output must contain (grep -E), then pairs: file, sed expression (every file changed in the mutant)
  local name=$1 expect=$2; shift 2
  rm -rf "${OUT:?}/m-$name"; mkdir -p "$OUT/m-$name"
  while [ $# -ge 2 ]; do
    local file=$1 expr=$2; shift 2
    mkdir -p "$OUT/m-$name/$(dirname "$file")"
    sed -e "$expr" "$file" > "$OUT/m-$name/$file"
    cmp -s "$file" "$OUT/m-$name/$file" && { echo "MUTANT $name: the edit to $file changed nothing"; exit 1; }
  done
  build "$OUT/m-$name/exe" "$OUT/m-$name" || { echo "MUTANT $name: does not build"; exit 1; }
  fresh_server
  if "$OUT/m-$name/exe" "$OUT/run" > "$OUT/m-$name/out" 2>&1; then echo "MUTANT $name: NOT CAUGHT"; exit 1; fi
  if [ -n "$expect" ] && ! grep -Eq "$expect" "$OUT/m-$name/out"; then echo "MUTANT $name: failed, but not the expected way ($expect)"; tail -8 "$OUT/m-$name/out"; exit 1; fi
  echo "MUTANT $name: caught ($(grep -c '^FAIL' "$OUT/m-$name/out") checks failed)"
}
# BEFORE THE FIX (the console's s26 build): the queue request names its own "Content-Type" line (np_dwc_match.cpp Headers) and the HTTPS client refuses that line
# as a reserved header before any network use (np_https.cpp), so np_http gives no handle and the match fails at once ("not set up", error 6 code 20100).
mutant before-the-fix "reached the server" \
    $D/np_dwc_match.cpp 's|    return h;   // (no Content-Type line.*|    h.push_back("Content-Type: application/json"); return h;|' \
    $N/np_https.cpp 's/if (strncasecmp(\*h, "Content-Type:", 13) == 0) {/if (0) {/'
# a socket that is not closed (the descriptor and the count leak; the budget must stop it after kMaxSockets)
mutant socket-never-closed "open at the end|file descriptors|every socket" $N/np_https_sys.cpp 's/if (s->fd >= 0) { close(s->fd); s->fd = -1; SockCounted(false); }/if (s->fd >= 0) { s->fd = -1; }/'
# the count is not kept (a leak would be invisible to the cap)
mutant count-not-kept "open at the end|every socket" $N/np_https_sys.cpp 's/__atomic_sub_fetch(&g_sockLive, 1, __ATOMIC_RELAXED);/(void)g_sockLive;/'
# a given-up or cancelled poll that is not released: the request runs on and keeps its socket
mutant cancel-not-released "open at the end|file descriptors|socket|polls" $N/online/np_http.cpp 's/    if (s->tls) np_https_release(s->tlsHandle);/    if (s->tls) { (void)s; }/'
# the gap after failed requests does not grow (a dead service is asked every 2 s)
mutant no-backoff "gaps grew" $D/np_dwc_match.cpp 's/    return gap > 10000 ? 10000 : gap;/    return kPollMs;/'
# the held friends poll is not ended when another request starts (the console's one worker waits out the hold)
mutant held-friends-kept "answered inside a caller" $D/np_online_http.cpp 's/        if (step != sFriends) EndHeldFriends();/        (void)0;/'
# a request kind the HTTPS client refuses again (PUT)
mutant put-refused "is started|arrived as such" $N/np_https.cpp 's/else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) ok = ok;/else if (strcmp(method, "POST") == 0) ok = ok;/'
# the search never gives up on a dead service
mutant no-giveup "5 times|ends with the connection error" $D/np_dwc_match.cpp 's/const int kMaxNoAnswer = 5;/const int kMaxNoAnswer = 100000;/'
# the cancel does not leave the queue
mutant cancel-no-delete "DELETE" $D/np_dwc_match.cpp 's/NpNet::HttpFire("DELETE"/(void)("DELETE"/'
echo "match test and mutants: ok"
