#!/bin/bash
# The NEON LA88 decoder against the scalar one, under qemu-arm (the build host is x86: there the test compares scalar with scalar).
#   tests/run_texdecode_neon_test.sh        TEST_BUILD=<dir> QEMU=<qemu-arm>
VGX=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${TEST_BUILD:-/tmp/vgx-tests}
QEMU=${QEMU:-qemu-arm}
mkdir -p "$BUILD"
set -e
FL=(-std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -I"$VGX/src")
clang++ "${FL[@]}" "$VGX/tests/texdecode_neon.cpp" "$VGX/src/vgx_texdecode.cpp" -o "$BUILD/texdecode_neon_host"
"$BUILD/texdecode_neon_host"
clang++ --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb "${FL[@]}" \
  "$VGX/tests/texdecode_neon.cpp" "$VGX/src/vgx_texdecode.cpp" -o "$BUILD/texdecode_neon_arm"
"$QEMU" -cpu cortex-a9 -L /usr/arm-linux-gnueabihf "$BUILD/texdecode_neon_arm"
