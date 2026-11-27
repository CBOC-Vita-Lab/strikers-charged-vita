#!/bin/bash
# MSC_LOCK60: the picture-size governor on simulated GPU-time traces (lock60.cpp). BREAK=1..4: a deliberate fault the checks must catch (the run must FAIL).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-lock60-test}
mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$VGX/src" "$VGX/tests/lock60.cpp" -o "$BUILD/lock60"
if [ -n "${BREAK:-}" ]; then "$BUILD/lock60" --break="$BREAK"; else "$BUILD/lock60"; fi
