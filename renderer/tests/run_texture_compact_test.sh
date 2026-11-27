#!/bin/bash
# MSC_VGX_CMPRA, MSC_VGX_TEX16: formats and the cache's choice. SANITIZE=1: ASan + UBSan.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-$VGX/build/tests/texture-compact}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$VGX/src" -I"$VGX/third_party")
if [[ ${SANITIZE:-0} == 1 ]]; then FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$CXX" "${FLAGS[@]}" "$VGX/tests/texture_compact.cpp" "$VGX/src/vgx_texdecode.cpp" "$VGX/src/vgx_texcache.cpp" "$VGX/src/vgx_guestmem.cpp" "$VGX/src/vgx_backend_null.cpp" -o "$BUILD/texture_compact"
env -u MSC_VGX_CMPRA -u MSC_VGX_TEX16 "$BUILD/texture_compact" default
MSC_VGX_CMPRA=1 MSC_VGX_VITA=1 "$BUILD/texture_compact" cmpra
MSC_VGX_TEX16=1 MSC_VGX_TEX16_MIN=0 "$BUILD/texture_compact" tex16
MSC_VGX_TEX16=1 "$BUILD/texture_compact" tex16min
MSC_VGX_CMPRA=2 MSC_VGX_VITA=1 "$BUILD/texture_compact" cmpra2
MSC_VGX_CMPRA=2 MSC_VGX_CMPRA_TOL=60 MSC_VGX_VITA=1 "$BUILD/texture_compact" cmpra2wide
