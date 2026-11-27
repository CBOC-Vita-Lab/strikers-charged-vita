#!/bin/bash
# walk15: host test of MSC_VGX_I8CHURN (decoder, the churn decision, the cache's use of it) and its mutants.
#   tests/run_i8churn_test.sh [--mutants]       needs xxhash.h (MSC/deps/xxHash like run_texture_tests.sh)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/tmp/mem10msc}
BUILD=${TEST_BUILD:-/tmp/walk15-i8churn}
CXX=${CXX:-clang++}
mkdir -p "$BUILD"
XX=${XXH:-$MSC/deps/xxHash}
[ -f "$XX/xxhash.h" ] || XX=$(dirname "$(find / -name xxhash.h -path '*xxHash*' 2>/dev/null | head -1)")
build() { # <srcdir override for src files> <out>
  local S=$1 O=$2
  "$CXX" -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$S" -I"$VGX/src" -I"$XX" "$VGX/tests/texture_i8churn.cpp" "$S/vgx_texdecode.cpp" "$S/vgx_texcache.cpp" "$VGX/src/vgx_guestmem.cpp" "$VGX/src/vgx_backend_null.cpp" -o "$O"
}
build "$VGX/src" "$BUILD/i8churn"
env -u MSC_VGX_I8CHURN MSC_VGX_DXT1=0 "$BUILD/i8churn" off
MSC_VGX_I8CHURN=0 MSC_VGX_DXT1=0 "$BUILD/i8churn" off
MSC_VGX_I8CHURN=1 MSC_VGX_DXT1=0 "$BUILD/i8churn" on
if [[ ${1:-} == --mutants ]]; then
  n=0; bad=0
  mut() { # <file> <sed expr> <name>
    local M="$BUILD/mut$n"; rm -rf "$M"; mkdir -p "$M"; cp "$VGX/src/vgx_i8churn.h" "$VGX/src/vgx_texdecode.cpp" "$VGX/src/vgx_texcache.cpp" "$M/"
    sed -i "$2" "$M/$1"
    if cmp -s "$M/$1" "$VGX/src/$1"; then echo "MUTANT $3: sed changed nothing"; bad=1; n=$((n+1)); return; fi
    if build "$M" "$BUILD/m$n" 2>/dev/null && MSC_VGX_I8CHURN=1 MSC_VGX_DXT1=0 "$BUILD/m$n" on >/dev/null 2>&1; then echo "MUTANT $3 SURVIVED"; bad=1; else echo "mutant $3 caught"; fi
    n=$((n+1))
  }
  mut vgx_i8churn.h 's/s.run >= minRun/s.run > minRun/' "run off by one (5th upload)"
  mut vgx_i8churn.h 's/s.run >= minRun/s.run + 1 >= minRun/' "run off by one (3rd upload)"
  mut vgx_i8churn.h 's/frame - s.last <= window/frame - s.last <= 1000000/' "window ignored"
  mut vgx_i8churn.h 's/        s.run = 0;/        (void)0;/' "gap does not reset the run"
  mut vgx_i8churn.h 's/(w \& 7u) == 0 \&\& (h \& 3u) == 0/true/' "shape check dropped"
  mut vgx_texdecode.cpp 's/s + y \* 8, 8)/s + y * 8 + 1, 7)/' "decoder row copy shifted"
  mut vgx_texdecode.cpp 's/ty < h; ty += 4)/ty + 4 < h; ty += 4)/' "decoder skips the last tile row"
  [[ $bad == 0 ]] || { echo "I8 churn mutants: FAILED"; exit 1; }
fi
echo "I8 churn tests: all passed"
