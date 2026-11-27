#!/bin/bash
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-dumpplay-test}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/dump_play.cpp" -o "$BUILD/dump_play"
"$BUILD/dump_play"
# mutants: each must be caught (the test exits non-zero)
for n in 1 2 3; do
    ${CXX:-clang++} -std=c++20 -O1 -DVGX_DUMPPLAY_BREAK=$n "$VGX/tests/dump_play.cpp" -o "$BUILD/dump_play_b$n"
    if "$BUILD/dump_play_b$n" >/dev/null; then echo "MUTANT $n NOT CAUGHT"; exit 1; else echo "mutant $n caught"; fi
done
