#!/bin/bash
# 1.3: glStateBits.h, glTextureMemo.h and np_gx_seqfill.h against what they replace. gx_seqfill also runs as NEON code when qemu-arm and an armhf compiler are here.
# (the NEON leg links with the armhf g++ itself, which needs no --target; set ARMLD=clang++ and ARMLDFLAGS="--target=arm-linux-gnueabihf ..." for a clang link)
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
NATIVE=$VGX/../native
BUILD=${TEST_BUILD:-${TMPDIR:-/tmp}/vgx-gl-render-fast-test}
mkdir -p "$BUILD"
CC="${CXX:-clang++} -std=c++11 -O1 -Wall -Wextra -fsanitize=address,undefined -I$NATIVE/include -I$NATIVE/platform/gx/include"
$CC "$VGX/tests/gl_state_bits.cpp" -o "$BUILD/gl_state_bits"
$CC "$VGX/tests/gl_texture_memo.cpp" -o "$BUILD/gl_texture_memo"
$CC "$VGX/tests/gx_seqfill.cpp" -o "$BUILD/gx_seqfill"
"$BUILD/gl_state_bits"
"$BUILD/gl_texture_memo"
"$BUILD/gx_seqfill"
# deliberate break: a change that does not reach the memo must fail
$CC -DBREAK_NO_BUMP "$VGX/tests/gl_texture_memo.cpp" -o "$BUILD/gl_texture_memo_break"
if "$BUILD/gl_texture_memo_break" > /dev/null; then echo "FAIL: the break went unseen"; exit 1; fi
echo "gl_texture_memo: the break is seen"
QEMU=${QEMU:-$(command -v qemu-arm || true)}
if [ -n "$QEMU" ] && [ -x "$QEMU" ] && command -v "${ARMLD:-arm-linux-gnueabihf-g++}" >/dev/null 2>&1; then
    ${ARMCXX:-clang++} --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -std=c++11 -O2 -I"$NATIVE/platform/gx/include" -c "$VGX/tests/gx_seqfill.cpp" -o "$BUILD/gx_seqfill_arm.o"
    ${ARMLD:-arm-linux-gnueabihf-g++} -static ${ARMLDFLAGS:-} "$BUILD/gx_seqfill_arm.o" -o "$BUILD/gx_seqfill_arm"
    "$QEMU" -cpu cortex-a9 "$BUILD/gx_seqfill_arm"
    echo "gx_seqfill: NEON leg ran under qemu-arm"
else
    echo "SKIPPED: gx_seqfill NEON leg (no qemu-arm or armhf g++ here): only the host build of gx_seqfill was tested"
fi
