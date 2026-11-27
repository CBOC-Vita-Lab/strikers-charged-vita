#!/bin/bash
# MSC_DYNRES_SEVERE: when the deeper floor opens and closes.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-dynres-severe-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/dynres_severe.cpp" -o "$BUILD/dynres_severe"
"$BUILD/dynres_severe"
