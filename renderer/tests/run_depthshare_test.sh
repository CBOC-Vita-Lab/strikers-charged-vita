#!/bin/bash
# the shared depth block table (vitaGL 0044) and the host model's window: a pass run, then a mutant run (the A2 clears removed) that must fail
set -eu
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-depthshare-test}
mkdir -p "$BUILD"
CORE="$VGX/../dependencies/vitaGL/source/utils/depth_share_core.h"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined "$VGX/tests/depthshare.cpp" -o "$BUILD/depthshare"
"$BUILD/depthshare"
# mutant: the identity of the last user is not cleared at a join or a leave (the alias15 behaviour)
grep -v '/\* A2 \*/' "$CORE" > "$BUILD/depth_share_core_mut.h"
${CXX:-g++} -std=c++20 -O1 -DDEPTH_SHARE_CORE="\"$BUILD/depth_share_core_mut.h\"" "$VGX/tests/depthshare.cpp" -o "$BUILD/depthshare_mut"
if "$BUILD/depthshare_mut" > "$BUILD/mut.out"; then echo "MUTANT SURVIVED (A2 test does not catch the stale last_fb)"; exit 1; fi
echo "mutant (A2 clears removed) fails as it must: $(grep -c FAIL "$BUILD/mut.out") checks"
