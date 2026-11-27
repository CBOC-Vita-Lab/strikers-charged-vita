#!/bin/bash
# Builds and runs the tests of the online transport (platform/sdk/net/online) on host32: STUN, hole punching against every NAT type,
# the encrypted session, loss / delay / reordering, NAT timeouts, Wi-Fi drop and rebuild, the relay framing. Virtual time: seconds.
#   heavy.sh tools/net/run_online_tests.sh [outdir] [-v]       default /tmp/np-online-test
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np-online-test}
mkdir -p "$OUT"
FL="${M:--m32} -isystem /usr/include/x86_64-linux-gnu -DNP_TARGET_HOST32=1 -DNP_NATIVE=1 -DBUILD_VERSION=0 -DVERSION_R4QE01 -DNDEBUG=1 \
 -I. -Iinclude -Itools/net -fno-strict-aliasing -fwrapv -fno-short-enums -std=gnu++11 -fno-exceptions -fno-rtti -O1 -g1 -Wall -Wno-unused-function $SAN"
N=platform/sdk/net/online
clang ${M:--m32} -O1 -g1 -c -w $N/third_party/monocypher/monocypher.c -o "$OUT/monocypher.o"
clang++ $FL $N/np_net.cpp $N/np_crypto.cpp $N/np_hash.cpp $N/np_turn.cpp $N/np_stun.cpp $N/np_peer.cpp $N/np_online.cpp $N/np_fault.cpp $N/np_signal_fake.cpp $N/np_signal_http.cpp $N/np_http.cpp $N/np_http_plain.cpp platform/sdk/net/np_https.cpp platform/sdk/net/np_https_sys.cpp $N/np_udp_posix.cpp \
  tools/net/natsim.cpp tools/net/turnsim.cpp tools/net/online_test.cpp "$OUT/monocypher.o" -x c++ -o "$OUT/online_test" \
  -lpthread 2>"$OUT/build.err" || { head -40 "$OUT/build.err"; exit 1; }
"$OUT/online_test" "$@"
# (port17) the UDP bind fallback chain (np_bind_chain.h) against a fake Vita bind; mutants: tools/net/bind_chain_mutants.sh
clang++ -std=gnu++11 -I. tools/net/bind_chain_test.cpp -o "$OUT/bind_chain_test" && "$OUT/bind_chain_test"
# (netclient13, O6) ASan + UBSan fuzz of the parsers under it: STUN, TURN (with signed answers), peer punch / DATA / CTRL, JSON, text. 200000 cases by default;
# NP_FUZZ_CASES=<n> changes the count, NP_FUZZ_CASES=0 skips it. Mutants: tools/net/fuzz_net_mutants.sh.
if [ "${NP_FUZZ_CASES-}" != "0" ]; then bash tools/net/run_fuzz_net.sh "$OUT/fuzz"; fi
# (mp24) which queue a search joins: PLAY WITH ANYONE = unranked quick, the hub's RANKED MATCH = ranked (np_quick_policy.h); mutants: tools/net/quick_policy_mutants.sh
clang++ -std=gnu++11 -I. tools/net/quick_policy_test.cpp -o "$OUT/quick_policy_test" && "$OUT/quick_policy_test"
