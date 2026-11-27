#!/bin/bash
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-poolkeep-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/pool_keep.cpp" -o "$BUILD/pool_keep"
"$BUILD/pool_keep"
