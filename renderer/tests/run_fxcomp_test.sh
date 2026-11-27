#!/bin/sh
# The effects compositor's rules (renderer/src/vgx_fxcomp.h) and the governor's accumulation step. Second part: a deliberately broken size rule (the threshold test flipped)
# must make the test FAIL, or the test checks nothing.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -I"$VGX/src" "$VGX/tests/fx_comp.cpp" -o "$BUILD/fx_comp"
"$BUILD/fx_comp"
M=$(mktemp -d "$BUILD/fx_comp_mut.XXXXXX"); cp "$VGX/src/vgx_fxgov.h" "$M/"
sed 's/return px >= minTriPx \* static_cast<double>(tris);/return px > minTriPx * static_cast<double>(tris) * 2.0;/' "$VGX/src/vgx_fxcomp.h" > "$M/vgx_fxcomp.h"
cmp -s "$M/vgx_fxcomp.h" "$VGX/src/vgx_fxcomp.h" && { echo "mutation did not apply"; exit 2; }
${CXX:-clang++} -std=c++20 -O1 -I"$M" "$VGX/tests/fx_comp.cpp" -o "$M/fx_comp_mut"
if "$M/fx_comp_mut"; then echo "FAIL: the broken size rule passed the test"; rm -rf "$M"; exit 1; fi
echo "fx_comp: the deliberately broken size rule fails the test, as it must"
rm -rf "$M"
