#!/bin/bash
# MSC_AB in-run A/B harness (src/vgx_ab.h), no GL (seconds). BREAK=1 (the arm never alternates), 2 (no knob is re-stored), 3 (a kApply flip is never requested), 4 (a kApply value made again on every request): the run must FAIL.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/vgx-ab15}; mkdir -p "$OUT"
F=(-std=c++20 -O1 -Wall -Wextra -I"$VGX/src"); [[ ${BREAK:-0} != 0 ]] && F+=(-DVGX_AB_BREAK=${BREAK})
${CXX:-g++} "${F[@]}" "$VGX/tests/ab15.cpp" -o "$OUT/ab15"
if [[ ${BREAK:-0} != 0 ]]; then if "$OUT/ab15" >/dev/null; then echo "ab15: FAILED (mutant $BREAK passed)"; exit 1; fi; echo "ab15: mutant $BREAK caught"; exit 0; fi
"$OUT/ab15"
