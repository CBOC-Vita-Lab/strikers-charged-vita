#!/bin/bash
# 1.3 MSC_VGX_ASYNC_SHADERS: the compile worker's queue and skip / block rules (tests/async_shaders.cpp), on the model and on the real GL back end
# over the stub GL with the host simulation (MSC_VGX_ASYNC_SHADERS_SIM). A few seconds, no GL driver, no game data.
# BREAK=1..5: a rule removed (1 every pending draw may be skipped, 2 the skippable class ignored, 3 no age bound, 4 passes / copied draws skippable,
# 5 the back end treats an EFB copy as reading nothing): must fail.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/async-shaders}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I"$VGX/src" -I"$VGX/src/glstub" -I"$VGX/third_party" -pthread)
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
run_all() {
    local bin=$1
    local rc=0
    "$bin" model || rc=1
    env MSC_VGX_ASYNC_SHADERS=1 MSC_VGX_ASYNC_SHADERS_SIM=60 "$bin" backend || rc=1
    env MSC_VGX_ASYNC_SHADERS=1 MSC_VGX_ASYNC_SHADERS_SIM=1000 "$bin" age || rc=1
    env MSC_VGX_ASYNC_SHADERS=0 MSC_VGX_ASYNC_SHADERS_SIM=60 "$bin" off || rc=1
    return $rc
}
if [[ -n ${BREAK:-} ]]; then
    "$CXX" "${FLAGS[@]}" -DVGX_ASYNC_BREAK="$BREAK" "$VGX/tests/async_shaders.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/async_shaders_break$BREAK"
    if run_all "$BUILD/async_shaders_break$BREAK" > "$BUILD/break$BREAK.out" 2>&1; then echo "async_shaders: FAILED (BREAK=$BREAK passed)"; exit 1; fi
    grep FAIL "$BUILD/break$BREAK.out" | head -3
    echo "async_shaders: BREAK=$BREAK is caught"
    exit 0
fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/async_shaders.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/async_shaders"
run_all "$BUILD/async_shaders" 2>/dev/null
echo "async_shaders: PASS"
