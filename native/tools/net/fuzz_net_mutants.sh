#!/bin/bash
# Each defect put back into a copy of the sources must make tools/net/fuzz_net_test fail (an ASan report, a UBSan report or a VIOLATION line).
#   bash tools/net/fuzz_net_mutants.sh [outdir] [cases]        default 20000 cases each; prints "caught" or "SURVIVED" per mutant, exit 1 if one survived
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np-fuzz-mutants}
CASES=${2:-20000}
M=${M:--m32}
mkdir -p "$OUT"
N=platform/sdk/net/online
SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
gcc $M -O1 -g1 -c -w $SANF $N/third_party/monocypher/monocypher.c -o "$OUT/monocypher.o"
declare -a NAMES SRC FROM TO
add() { NAMES+=("$1"); SRC+=("$2"); FROM+=("$3"); TO+=("$4"); }
add turn_copystr_overread  $N/np_turn.cpp   'src && i < n && i + 1 < cap && src[i]' 'src && src[i] && i < n && i + 1 < cap'
add stun_length_unchecked  $N/np_stun.cpp    '|| 20u + alen > len' ''
add peer_oversize_accepted $N/np_peer.cpp    'if (n > kMaxDatagram) { ++stats_.dropTooBig; return false; }' ''
add peer_punch_mac_ignored $N/np_peer.cpp    'if (!MacEqual(mac, d + 24)) {' 'if (false) {'
add peer_replay_window_off $N/np_peer.cpp    'if (back >= 64 || (rxBits_ >> back & 1)) {' 'if (false) {'
add turn_lifetime_unclamped $N/np_turn.cpp   'if (lifetime_ > 86400) lifetime_ = 86400;' ''
add turn_integrity_ignored $N/np_turn.cpp    'if (n < 20) return false;
    const size_t body = Get16(d + 2);
    if (body + 20 > n) return false;
    size_t o = 20;
    while (o + 4 <= 20 + body) {
        const uint16_t t = uint16_t(Get16(d + o));
        const size_t l = Get16(d + o + 2);
        if (o + 4 + l > 20 + body) return false;
        if (t == aIntegrity) {' 'if (n >= 0) return true;
    const size_t body = Get16(d + 2);
    size_t o = 20;
    while (o + 4 <= 20 + body) {
        const uint16_t t = uint16_t(Get16(d + o));
        const size_t l = Get16(d + o + 2);
        if (t == aIntegrity) {'
add json_high_bytes_pass   $N/np_text_safe.h 'if (TextAsciiOverride() >= 0) return TextAsciiOverride() != 0;' 'return false;'
add safecopy_unfiltered    $N/np_text_safe.h 'if (on && (c < 0x20 || c > 0x7E))' 'if (false)'
bad=0
for i in "${!NAMES[@]}"; do
  T="$OUT/${NAMES[$i]}"
  rm -rf "$T"; mkdir -p "$T/$N" "$T/platform/sdk/dwc" "$T/tools/net"
  cp $N/*.h $N/*.cpp "$T/$N/"; cp platform/sdk/dwc/np_online_json.* "$T/platform/sdk/dwc/"; cp tools/net/fuzz_net_test.cpp "$T/tools/net/"
  python3 - "$T/${SRC[$i]}" "${FROM[$i]}" "${TO[$i]}" <<'PY'
import sys
p, a, b = sys.argv[1:4]
s = open(p).read()
if a not in s: sys.exit("mutant text not found: " + a[:60])
open(p, 'w').write(s.replace(a, b, 1))
PY
  g++ $M -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNDEBUG=1 -I"$T" -I. -Iinclude -fno-strict-aliasing -fwrapv -std=gnu++11 -O1 -g1 -w $SANF \
    "$T/$N/np_net.cpp" "$T/$N/np_crypto.cpp" "$T/$N/np_hash.cpp" "$T/$N/np_turn.cpp" "$T/$N/np_stun.cpp" "$T/$N/np_peer.cpp" "$T/platform/sdk/dwc/np_online_json.cpp" "$T/tools/net/fuzz_net_test.cpp" "$OUT/monocypher.o" \
    -o "$T/fuzz" -lpthread 2>"$T/build.err" || { head -20 "$T/build.err"; echo "mutant ${NAMES[$i]}: does not build"; bad=1; continue; }
  if ASAN_OPTIONS=detect_leaks=0 "$T/fuzz" --cases "$CASES" > "$T/run.log" 2>&1; then echo "mutant ${NAMES[$i]}: SURVIVED"; bad=1; else echo "mutant ${NAMES[$i]}: caught ($(grep -m1 -o 'ERROR: AddressSanitizer: [a-z-]*\|runtime error: [a-z ]*\|VIOLATION.*' "$T/run.log" | head -1 | cut -c1-90))"; fi
  rm -rf "$T"
done
exit $bad
