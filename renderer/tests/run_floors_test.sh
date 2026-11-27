#!/bin/bash
# MSC_PICTURE_FLOORS: the owner's picture floors (floors.cpp). BREAK=1..3: a deliberate fault the checks must catch; MUTANT=quality|smooth|table (table: the Quality ladder's table has the old rungs, F1): the header's floor constant lowered (the run must FAIL).
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-floors-test}
mkdir -p "$BUILD"
DEFS=""
case "${MUTANT:-}" in
  quality) DEFS="-DVGX_FLOOR_QUALITY_W=700 -DVGX_FLOOR_QUALITY_H=400" ;;
  table) DEFS="-DVGX_FLOORS_TABLE_BREAK" ;;
  smooth) DEFS="-DVGX_FLOOR_SMOOTH_W=600 -DVGX_FLOOR_SMOOTH_H=340" ;;
esac
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined $DEFS -I "$VGX/src" "$VGX/tests/floors.cpp" -o "$BUILD/floors"
if [ -n "${BREAK:-}" ]; then "$BUILD/floors" --break="$BREAK"; else "$BUILD/floors"; fi
