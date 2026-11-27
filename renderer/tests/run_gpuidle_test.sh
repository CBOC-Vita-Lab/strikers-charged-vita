#!/bin/bash
# MSC_GPU_IDLE_SKIP: vgx_gpuidle.h on traces (gpuidle.cpp). BREAK=1..3: a deliberate fault the checks must catch (the run must FAIL).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-gpuidle-test}
mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$VGX/src" "$VGX/tests/gpuidle.cpp" -o "$BUILD/gpuidle"
if [ -n "${BREAK:-}" ]; then "$BUILD/gpuidle" --break="$BREAK"; else "$BUILD/gpuidle"; fi
