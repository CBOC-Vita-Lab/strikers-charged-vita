#!/bin/bash
# MSC_VGX_LODBIAS and MSC_VITA_SHAPE: parsing, where the bias applies, the size of the shaped picture.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-picture-math-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/picture_math.cpp" -o "$BUILD/picture_math"
"$BUILD/picture_math"
