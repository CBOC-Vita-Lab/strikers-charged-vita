#!/bin/sh
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/fx_gov.cpp" -o "$BUILD/fx_gov"
"$BUILD/fx_gov"
