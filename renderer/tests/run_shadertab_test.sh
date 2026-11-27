#!/bin/bash
# renderer/tests/run_shadertab_test.sh: builds and runs the ShaderTab test (MSC_DM_COMPACT), then a mutant (the high part's size forgets the split) that must fail it
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-/tmp/vgx_shadertab_test}; mkdir -p "$BUILD"
${CXX:-g++} -std=c++20 -O1 -Wall -Wextra -I"$VGX/src" "$VGX/tests/shadertab.cpp" -o "$BUILD/shadertab"
"$BUILD/shadertab"
M=$(mktemp -d "$BUILD/mut.XXXXXX"); cp "$VGX/src/vgx_shadertab.h" "$M/"
sed -i 's/splitCache_ + hi_.size()/hi_.size()/' "$M/vgx_shadertab.h"
cmp -s "$M/vgx_shadertab.h" "$VGX/src/vgx_shadertab.h" && { echo "FAIL: the mutation did not apply"; exit 1; }
${CXX:-g++} -std=c++20 -O1 -I"$M" "$VGX/tests/shadertab.cpp" -o "$M/shadertab_mut"
if "$M/shadertab_mut"; then echo "FAIL: the mutant passed"; rm -rf "$M"; exit 1; fi
echo "shadertab: the mutant fails the test, as it must"; rm -rf "$M"
