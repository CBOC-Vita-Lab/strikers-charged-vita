#!/bin/bash
# MSC_DYNRES_LATEHOLD: when a late frame asks for a step down, and the hold.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-dynres-late-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/dynres_late.cpp" -o "$BUILD/dynres_late"
"$BUILD/dynres_late"
