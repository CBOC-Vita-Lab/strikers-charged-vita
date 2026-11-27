#!/bin/bash
# The service host fallback: unit test against a local fake TLS layer (no sockets, no DNS), then the mutants (each must make the test fail), then
# source checks that the TLS rules and the User-Agent are untouched.
#   tools/net/run_service_hosts_test.sh [outdir]       default /tmp/np-service-hosts-test
set -eo pipefail
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np-service-hosts-test}
mkdir -p "$OUT"
FL="${M:--m32} -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNDEBUG=1 -DNP_SERVICE_HOSTS_TEST=1 -I. -Iinclude -fno-strict-aliasing -fwrapv -std=gnu++11 -fno-exceptions -fno-rtti -O1 -g1 -w"
build() {   # <include override dir or ""> <out exe>
    local inc=""; [ -n "$1" ] && inc="-I$1"
    clang++ $inc $FL platform/sdk/net/online/np_http.cpp platform/sdk/net/online/np_http_plain.cpp \
        tools/net/service_hosts_test.cpp -x c++ -o "$2" -lpthread 2>"$OUT/build.err" || { head -30 "$OUT/build.err"; return 1; }
}
build "" "$OUT/sht"
"$OUT/sht"
H=platform/sdk/net/online/np_service_hosts.h
mut() {   # <name> <sed expression>
    rm -rf "$OUT/mut"; mkdir -p "$OUT/mut/platform/sdk/net/online"
    sed -e "$2" $H > "$OUT/mut/$H"
    cmp -s $H "$OUT/mut/$H" && { echo "MUTANT $1 changed nothing"; exit 1; }
    if build "$OUT/mut" "$OUT/sht_m" 2>/dev/null && "$OUT/sht_m" >/dev/null 2>&1; then echo "MUTANT SURVIVED: $1"; exit 1; fi
    echo "mutant killed: $1"
}
mut "CERT falls back"            's/case NP_HTTPS_E_DNS: case NP_HTTPS_E_CONNECT:/case NP_HTTPS_E_CERT: case NP_HTTPS_E_DNS: case NP_HTTPS_E_CONNECT:/'
mut "IO falls back"              's/case NP_HTTPS_E_DNS: case NP_HTTPS_E_CONNECT:/case NP_HTTPS_E_IO: case NP_HTTPS_E_DNS: case NP_HTTPS_E_CONNECT:/'
mut "timeout in any stage"       's/return stage >= NP_HTTPS_S_QUEUED \&\& stage <= NP_HTTPS_S_TLS;/return true;/'
mut "no memo (always new first)" 's/if (at \&\& Now() + 1 - at < kRetryAfterMs) return SwapToOld(url);/(void)at;/'
mut "memo never expires"         's/Now() + 1 - at < kRetryAfterMs/true/'
mut "memo not set"               's/DownAt() = Now() + 1;/;/'
mut "memo not cleared on answer" 's/if (BodyIsOurs(body, bodyLen)) { DownAt() = 0; return false; }/if (BodyIsOurs(body, bodyLen)) return false;/'
mut "switch ignored"             's/return GetMode() == MODE_AUTO \&\& HasPrefix/return HasPrefix/'
mut "old mode ignored"           's/if (s == NewBase() \&\& GetMode() == MODE_OLD) return OldBase();//'
mut "prefix match loose"         's/ \&\& (url.size() == n || url\[n\] == .\/. || url\[n\] == .?.)//'
mut "fallback over plain http"   's/return std::string(OldBase()) + url.substr/return std::string("http:\/\/x") + url.substr/'
mut "short connect limit lost"   's/kNewHostConnectMs = 6000/kNewHostConnectMs = 0/'
mut "zone check removed (every answer final)" 's/if (BodyIsOurs(body, bodyLen)) { DownAt/if (true) { DownAt/'
mut "JSON answers treated as zone"            's/return b\[i\] == .{.;/return false;/'
mut "default is old"                           's/return MODE_AUTO;   \/\/ the default/return MODE_OLD;   \/\/ the default/'
mut "zone answer does not mark the host down"  's/DownAt() = Now() + 1;   \/\/ the zone answered/;   \/\/ the zone answered/'
mut "leading space not skipped"                's/if (b\[i\] == . . || b\[i\] == .\\t. || b\[i\] == .\\r. || b\[i\] == .\\n.) continue;//'
mut "plain-text answer counts as ours"         's/return b\[i\] == .{.;/return true;/'
# the TLS client itself: verification stays mandatory, no new switch, the User-Agent is the game's
grep -q 'VERIFY_REQUIRED' platform/sdk/net/np_https_tls_mbedtls.cpp || { echo "FAIL: VERIFY_REQUIRED missing"; exit 1; }
! grep -n 'VERIFY_NONE\|VERIFY_OPTIONAL\|ServiceHosts' platform/sdk/net/np_https*.cpp platform/sdk/net/np_https*.h || { echo "FAIL: TLS layer changed"; exit 1; }
grep -q 'User-Agent: StrikersVita' platform/sdk/net/np_https.cpp || { echo "FAIL: User-Agent changed"; exit 1; }
echo "TLS checks: ok (verification mandatory, no hosts code in the TLS layer, User-Agent StrikersVita)"
