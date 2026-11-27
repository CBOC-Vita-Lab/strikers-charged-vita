#!/bin/bash
# AddressSanitizer + UndefinedBehaviorSanitizer fuzz of the online client's parsers (STUN, TURN, peer punch / frames, JSON, text): tools/net/fuzz_net_test.cpp.
# No sockets, no service. Default 200000 cases (about 3 minutes under ASan on an idle core; 25+ on the loaded helper box); NP_FUZZ_CASES / NP_FUZZ_SEED or the arguments change them.
#   bash tools/net/run_fuzz_net.sh [outdir] [fuzz_net_test arguments, e.g. --cases 3000000 --seed 7]
# g++ (the clang here has no ASan runtime); M=-m32 (default, the console is 32-bit) or M=-m64.
set -e -o pipefail
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np-fuzz-net}
shift || true
mkdir -p "$OUT"
M=${M:--m32}
N=platform/sdk/net/online
SANF="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
gcc $M -O1 -g1 -c -w $SANF $N/third_party/monocypher/monocypher.c -o "$OUT/monocypher.o"
g++ $M -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DNDEBUG=1 -I. -Iinclude -fno-strict-aliasing -fwrapv -std=gnu++11 -O1 -g1 -Wall -Wno-unused-function $SANF \
  $N/np_net.cpp $N/np_crypto.cpp $N/np_hash.cpp $N/np_turn.cpp $N/np_stun.cpp $N/np_peer.cpp platform/sdk/dwc/np_online_json.cpp tools/net/fuzz_net_test.cpp "$OUT/monocypher.o" \
  -o "$OUT/fuzz_net_test" -lpthread 2>"$OUT/build.err" || { head -40 "$OUT/build.err"; exit 1; }
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 "$OUT/fuzz_net_test" "$@" 2>&1 | grep -v '^\[np-net\]'   # (the transport logs every state change; the fuzz keeps only its own lines and the sanitizers')
