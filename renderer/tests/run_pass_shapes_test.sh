#!/bin/sh
# held25: host test of the pass-shape list (vgx_passshapes.h); --mutants: VGX_PASSSHAPES_BREAK 1-9 must each fail.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
CXX=${CXX:-g++}
$CXX -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/pass_shapes.cpp" -o "$BUILD/pass_shapes"
"$BUILD/pass_shapes"
if [ "$1" = "--mutants" ]; then
  for b in 1 2 3 4 5 6 7 8 9; do
    $CXX -std=c++17 -O1 -DVGX_PASSSHAPES_BREAK=$b "$VGX/tests/pass_shapes.cpp" -o "$BUILD/pass_shapes_m$b"
    if "$BUILD/pass_shapes_m$b" > /dev/null; then echo "mutant $b NOT caught"; exit 1; else echo "mutant $b caught"; fi
  done
fi
