#!/bin/bash
# MSC_DYNRES_BLIND: the guard's verdicts (dynres_blind.cpp) and the trace model with the GPU timers silenced under overload (tools/dynres_sim.cpp --blind-test).
# BREAK=1: the guard ignores the CPU lanes (unit test) and the model runs without the guard; both must then FAIL.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-dynres-blind-test}
mkdir -p "$BUILD"
DEF=()
[ "${BREAK:-0}" = 1 ] && DEF=(-DBLIND_BREAK=1)
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "${DEF[@]}" "$VGX/tests/dynres_blind.cpp" -o "$BUILD/dynres_blind"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "${DEF[@]}" -I "$VGX/src" "$VGX/tools/dynres_sim.cpp" -o "$BUILD/dynres_sim"
rc=0
"$BUILD/dynres_blind" || rc=1
"$BUILD/dynres_sim" --blind-test || rc=1
exit $rc
