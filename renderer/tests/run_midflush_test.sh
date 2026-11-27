#!/bin/bash
# MSC_VGX_MIDFLUSH scheduling rule, no GL (seconds). BREAK=1: the forced flush is removed (mutant), the run must FAIL.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/vgx-midflush}; mkdir -p "$OUT"
F=(-std=c++20 -O1 -Wall -Wextra -I"$VGX/src"); [[ ${BREAK:-0} == 1 ]] && F+=(-DVGX_MIDFLUSH_BREAK=1)
${CXX:-g++} "${F[@]}" "$VGX/tests/midflush.cpp" -o "$OUT/midflush"
if [[ ${BREAK:-0} == 1 ]]; then if "$OUT/midflush" >/dev/null; then echo "midflush: FAILED (mutant passed)"; exit 1; fi; echo "midflush: mutant caught"; exit 0; fi
"$OUT/midflush"
