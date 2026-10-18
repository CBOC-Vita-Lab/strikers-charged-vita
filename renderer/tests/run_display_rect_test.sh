#!/bin/bash
# The Vita host's picture rectangle (vita/vgx_display_rect.h): 4:3 box 117..842 of 960, or the whole screen.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
MSC=${MSC:-/home/claude/msc}
BUILD=${TEST_BUILD:-$MSC/build/cxr/tests}
mkdir -p "$BUILD"
${CXX:-clang++} -std=c++20 -O1 -Wall -Wextra -I"$VGX/vita" "$VGX/tests/display_rect.cpp" -o "$BUILD/display_rect"
"$BUILD/display_rect" && echo "display rect test: passed"
