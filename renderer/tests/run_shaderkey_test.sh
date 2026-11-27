#!/bin/bash
# tools15: the two shader hashes (pack key P and text hash T) on ONE log line where the renderer reports a slow compile ("[shaderplay]" in a match, "[shaderkey]" outside one).
#   tests/run_shaderkey_test.sh      (a second, no GL driver; mutant: the P value dropped from the line must fail the source check)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-${TEST_BUILD:-/tmp/shaderkey-test}}; mkdir -p "$BUILD"
CXX=${CXX:-clang++}
"$CXX" -std=c++20 -O1 -Wall -Wextra -I"$VGX/src" -I"$VGX/third_party" "$VGX/tests/shaderkey.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/shaderkey"
"$BUILD/shaderkey"
check() { # file : both log formats carry P and T from ShaderKeys
  local f=$1
  grep -q 'ShaderKeys(text, &kp, &kt)' "$f" && grep -q '\[pack key P=%016llX T=%016llX\]' "$f" && grep -q '\[shaderkey\] picture %lu: a %s shader compiled in %.1f ms (not in a match): P=%016llX T=%016llX' "$f"
}
check "$VGX/src/vgx_backend_gl.cpp" || { echo "FAIL: the compile log lines do not carry both hashes"; exit 1; }
sed 's/\[pack key P=%016llX T=%016llX\]//' "$VGX/src/vgx_backend_gl.cpp" > "$BUILD/mut1.cpp"
if check "$BUILD/mut1.cpp"; then echo "FAIL: mutant (P dropped from the [shaderplay] line) survived"; exit 1; fi
sed 's/(not in a match): P=%016llX T=%016llX//' "$VGX/src/vgx_backend_gl.cpp" > "$BUILD/mut2.cpp"
if check "$BUILD/mut2.cpp"; then echo "FAIL: mutant ([shaderkey] line without P) survived"; exit 1; fi
# the old parser (tools/pack17/pack17_request.py) still reads the extended [shaderplay] line
python3 -I - "$VGX/.." <<'P'
import re, sys
src = open(sys.argv[1] + "/tools/pack17/pack17_request.py").read()
rx = re.search(r"PLAY = re.compile\(r'(.*)'\)", src).group(1)
line = "[shaderplay] picture 12: a fragment shader (text hash 00000000000000AB) took 9.5 ms to compile in play (the pack did not answer it; see the 'shader pack MISS' lines) [pack key P=00000000000000CD T=00000000000000AB]"
m = re.search(rx, line); assert m and m.group(3) == "00000000000000AB", "old parser broke"
P
echo "shaderkey tests: all passed"
