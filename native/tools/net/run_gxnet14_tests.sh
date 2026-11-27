#!/bin/bash
# Wave 14 (gxnet14) host tests that need no service and no sockets: the proof-of-work solver (against the service's reference nonces) and the keyboard's UTF-16 to UTF-8.
# The edge-page / pacing-hint / proof-of-work flows have their tests in online_test.cpp (--held, run_online_tests.sh) and tools/dwc/http_test.cpp (run_dwc_http_test.sh).
#   bash tools/net/run_gxnet14_tests.sh [outdir]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/np-gxnet14-test}
mkdir -p "$OUT"
for t in pow_test ime_utf8_test; do
  clang++ -std=gnu++11 -I. -Wall -O1 -o "$OUT/$t" "tools/net/$t.cpp"
  "$OUT/$t"
done
