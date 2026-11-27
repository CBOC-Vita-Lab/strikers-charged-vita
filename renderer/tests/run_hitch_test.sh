#!/bin/bash
# MSC_HITCH_MS: which frame is blamed and for what.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-hitch-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/hitch.cpp" -o "$BUILD/hitch"
"$BUILD/hitch"
for b in 1 2 3; do   # wave 9 mutants: 1 the period is ignored, 2 the fold bins by frame % 599, 3 held lines share the menu budget: all must fail
    ${CXX:-clang++} -std=c++20 -O1 -DVGX_HITCH_BREAK=$b "$VGX/tests/hitch.cpp" -o "$BUILD/hitch_break$b"
    if "$BUILD/hitch_break$b" > "$BUILD/break$b.out" 2>&1; then echo "hitch: FAILED (BREAK=$b passed)"; exit 1; fi
    echo "hitch: BREAK=$b is caught ($(grep -c FAIL "$BUILD/break$b.out") checks failed)"
done
