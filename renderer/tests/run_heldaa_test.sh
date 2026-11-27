#!/bin/bash
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-heldaa-test}
mkdir -p "$BUILD"
for t in heldaa sizesets; do
  ${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/$t.cpp" -o "$BUILD/$t"
  "$BUILD/$t"
done
