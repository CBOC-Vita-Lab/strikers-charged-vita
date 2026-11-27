#!/bin/bash
# cpu60-25: MSC_VGX_TEXSIZE_MEMO unit test: the memoised TexDataSize equals the plain loop (default), the verify mode counts nothing wrong, mode 0 is the plain loop. SANITIZE=1: ASan + UBSan.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/texsize-memo}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$VGX/third_party")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/texsize_memo.cpp" "$VGX/src/vgx_texdecode.cpp" -o "$BUILD/texsize_memo"
env -u MSC_VGX_TEXSIZE_MEMO "$BUILD/texsize_memo"
MSC_VGX_TEXSIZE_MEMO=2 "$BUILD/texsize_memo"
MSC_VGX_TEXSIZE_MEMO=0 "$BUILD/texsize_memo"
