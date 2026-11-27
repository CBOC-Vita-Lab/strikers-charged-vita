#!/bin/bash
# MSC_VGX_FOGNOOP (nxgpu25): the no-op fog rule of src/vgx_fognoop.h against the fog code the generator emits (tests/fognoop.cpp), and its four deliberate breaks (each must fail).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/fognoop}
CXX=${CXX:-g++}
mkdir -p "$BUILD"
"$CXX" -std=c++20 -O2 -g -Wall -Wextra -I"$VGX/src" "$VGX/tests/fognoop.cpp" -o "$BUILD/fognoop"
"$BUILD/fognoop"
for m in 1 2 3 5; do ! "$BUILD/fognoop" mutate $m >/dev/null 2>&1 || { echo "mutant $m NOT caught"; exit 1; }; done
echo "fognoop: PASS"
