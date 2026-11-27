#!/bin/sh
# nxfly25: host test of the atlas apart rules (vgx_atlasapart.h); --mutants: VGX_ATLASAPART_BREAK 1-6 must each fail.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/atlas_apart.cpp" -o "$BUILD/atlas_apart"
"$BUILD/atlas_apart"
if [ "$1" = "--mutants" ]; then
  for b in 1 2 3 4 5 6; do
    $CXX -std=c++17 -O1 -DVGX_ATLASAPART_BREAK=$b "$VGX/tests/atlas_apart.cpp" -o "$BUILD/atlas_apart_m$b"
    if "$BUILD/atlas_apart_m$b" > /dev/null; then echo "mutant $b NOT caught"; exit 1; else echo "mutant $b caught"; fi
  done
fi
