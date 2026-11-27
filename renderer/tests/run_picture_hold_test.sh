#!/bin/bash
# MSC_PICTURE_HOLD: the FULL picture policy on simulated GPU-time traces (picture_hold.cpp). BREAK=1..2: a deliberate fault the checks must catch (the run must FAIL).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-hold-test}
mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$VGX/src" "$VGX/tests/picture_hold.cpp" -o "$BUILD/picture_hold"
if [ -n "${BREAK:-}" ]; then "$BUILD/picture_hold" --break="$BREAK"; else "$BUILD/picture_hold"; fi
