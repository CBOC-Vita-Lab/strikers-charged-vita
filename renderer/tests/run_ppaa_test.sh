#!/bin/bash
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-ppaa-test}
mkdir -p "$BUILD"
${CXX:-c++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/ppaa.cpp" -o "$BUILD/ppaa"
"$BUILD/ppaa"
