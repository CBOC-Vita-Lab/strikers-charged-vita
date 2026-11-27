#!/bin/sh
# scenes30: host test of the held-scene GPU governor (vgx_heldgov.h) and the fill governor's hold (vgx_fxgov.h); --mutants: VGX_HELDGOV_BREAK 1-6 must each fail.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/held_gov.cpp" -o "$BUILD/held_gov"
"$BUILD/held_gov"
if [ "$1" = "--mutants" ]; then
  for b in 1 2 3 4 5 6; do
    $CXX -std=c++17 -O1 -DVGX_HELDGOV_BREAK=$b "$VGX/tests/held_gov.cpp" -o "$BUILD/held_gov_m$b"
    if "$BUILD/held_gov_m$b" > /dev/null; then echo "mutant $b NOT caught"; exit 1; else echo "mutant $b caught"; fi
  done
fi
