#!/bin/sh
# gpueff18: host test of the crowd trim's welded-pass count and skewed-quad cut (vgx_ctskew.h); --mutants: VGX_CTSKEW_BREAK 1-4 must each fail.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/ctskew.cpp" -o "$BUILD/ctskew"
"$BUILD/ctskew"
if [ "$1" = "--mutants" ]; then
  for b in 1 2 3 4; do
    $CXX -std=c++17 -O1 -DVGX_CTSKEW_BREAK=$b "$VGX/tests/ctskew.cpp" -o "$BUILD/ctskew_m$b"
    if "$BUILD/ctskew_m$b" > /dev/null; then echo "mutant $b NOT caught"; exit 1; else echo "mutant $b caught"; fi
  done
fi
