#!/bin/sh
# nxfly25: host test of the MegaPrewarm size rule (vgx_megawarm.h); --mutants: VGX_MEGAWARM_BREAK 1-3 must each fail.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/mega_warm.cpp" -o "$BUILD/mega_warm"
"$BUILD/mega_warm"
if [ "$1" = "--mutants" ]; then
  for b in 1 2 3; do
    $CXX -std=c++17 -O1 -DVGX_MEGAWARM_BREAK=$b "$VGX/tests/mega_warm.cpp" -o "$BUILD/mega_warm_m$b"
    if "$BUILD/mega_warm_m$b" > /dev/null; then echo "mutant $b NOT caught"; exit 1; else echo "mutant $b caught"; fi
  done
fi
