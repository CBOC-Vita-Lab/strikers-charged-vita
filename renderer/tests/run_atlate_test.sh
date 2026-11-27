#!/bin/sh
# The alpha-test sinking plan (renderer/src/vgx_atlate.h). Second part: a plan that lets a late draw pass a barrier (the flush before a barrier removed) must make the test FAIL.
set -e
VGX=$(cd "$(dirname "$0")/.." && pwd); BUILD=${BUILD:-${TMPDIR:-/tmp}}
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -I"$VGX/src" "$VGX/tests/atlate.cpp" -o "$BUILD/atlate"
"$BUILD/atlate"
M=$(mktemp -d "$BUILD/atlate_mut.XXXXXX")
python3 - "$VGX/src/vgx_atlate.h" "$M/vgx_atlate.h" <<'PY'
import sys
s = open(sys.argv[1]).read()
t = s.replace("            case kBarrier:\n                flush();\n", "            case kBarrier:\n", 1)
assert t != s
open(sys.argv[2], "w").write(t)
PY
${CXX:-clang++} -std=c++20 -O1 -I"$M" "$VGX/tests/atlate.cpp" -o "$M/atlate_mut"
if "$M/atlate_mut"; then echo "FAIL: the plan that lets a late draw pass a barrier passed the test"; rm -rf "$M"; exit 1; fi
echo "atlate: the deliberately broken plan fails the test, as it must"
rm -rf "$M"
