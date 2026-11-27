#!/bin/bash
# split17: held-present counters (split17.cpp). BREAK=1..3: a deliberate fault the checks must catch (the run must FAIL).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-split17-test}
mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$VGX/src" "$VGX/tests/split17.cpp" -o "$BUILD/split17"
if [ -n "${BREAK:-}" ]; then "$BUILD/split17" --break="$BREAK"; else "$BUILD/split17"; fi
