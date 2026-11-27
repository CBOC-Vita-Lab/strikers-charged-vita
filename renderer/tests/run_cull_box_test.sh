#!/bin/bash
# MSC_VGX_CULL: the box-outside-scissor test is conservative (random cameras and boxes).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-cull-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra "$VGX/tests/cull_box.cpp" -o "$BUILD/cull_box"
"$BUILD/cull_box" && echo "cull box test: passed"
