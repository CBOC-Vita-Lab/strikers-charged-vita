#!/bin/bash
# W12 SHADER: the alpha-test triviality analysis (vgx_shadergen.cpp AlphaTestAlwaysPasses / AnalyzeFs), no GL, a few seconds.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/work/msc}
XXH="$MSC/deps/xxHash"; [[ -f "$XXH/xxhash.h" ]] || XXH="$(cd "$(dirname "$0")/.." && pwd)/third_party"   # (no deps/xxHash on this box: the repo copy)
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -I"$VGX/src" -I"$XXH" "$VGX/tests/alphatrivial.cpp" "$VGX/src/vgx_shadergen.cpp" -o "$BUILD/alphatrivial"
"$BUILD/alphatrivial"
