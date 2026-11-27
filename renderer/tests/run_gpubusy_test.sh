#!/bin/bash
# MSC_GPU_BUSY_FIX: vgx_gpubusy.h on traces (gpubusy.cpp). BREAK=1..2: a deliberate fault the checks must catch (the run must FAIL).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-gpubusy-test}
mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$VGX/src" "$VGX/tests/gpubusy.cpp" -o "$BUILD/gpubusy"
if [ -n "${BREAK:-}" ]; then "$BUILD/gpubusy" --break="$BREAK"; else "$BUILD/gpubusy"; fi
