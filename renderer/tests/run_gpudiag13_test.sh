#!/bin/bash
# gpudiag13 (vgx_gpudiag13.h): the pure parts of MSC_VGX_SCENE_PROBE / DROP_CLASS / GPU_BISECT / MASKFULL. BREAK=1..4: a deliberate fault the checks must catch (the run must FAIL).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-gpudiag13-test}
mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -I "$VGX/src" "$VGX/tests/gpudiag13.cpp" -o "$BUILD/gpudiag13"
if [ -n "${BREAK:-}" ]; then "$BUILD/gpudiag13" --break="$BREAK"; else "$BUILD/gpudiag13"; fi
