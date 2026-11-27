#!/bin/bash
# rung25: the rung premake bookkeeping test and its mutant (the mutant must FAIL).
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-rungpremake-test}
mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/rungpremake.cpp" -o "$BUILD/rungpremake"
"$BUILD/rungpremake"
if "$BUILD/rungpremake" mutant > "$BUILD/mutant.out" 2>&1; then echo "rungpremake: the mutant PASSED (the test does not see a step that frees)"; exit 1; fi
echo "rungpremake: mutant caught ($(grep -c FAIL "$BUILD/mutant.out") checks failed)"
