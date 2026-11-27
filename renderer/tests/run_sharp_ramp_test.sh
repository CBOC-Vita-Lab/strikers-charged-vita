#!/bin/bash
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-sharpramp-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/sharp_ramp.cpp" -o "$BUILD/sharp_ramp"
"$BUILD/sharp_ramp"
