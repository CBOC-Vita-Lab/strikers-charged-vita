#!/bin/bash
# nlSqrt / nlRecipSqrt over the whole float range (tools/det/test_frsqrte.cpp): armhf under qemu (the NEON __frsqrte) and host32. Exit 0 = no wrong value.
#   tools/det/test_frsqrte.sh        (BUILD=dir for the binaries; QEMU=path to qemu-arm; a leg whose toolchain or runner is missing is skipped, and with no leg at all the script says SKIP, exit 77)
NP=$(cd "$(dirname "$0")/../.." && pwd); B=${BUILD:-${TMPDIR:-/tmp}/np-frsqrte}; mkdir -p "$B"; cd "$NP" || exit 2; rc=0; ran=0
QEMU=${QEMU:-$(command -v qemu-arm || true)}
if [ -n "$QEMU" ] && [ -x "$QEMU" ] && { clang++ --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fno-math-errno -DNP_NATIVE=1 -DNP_TARGET_ARMHF=1 -Iinclude -I. tools/det/test_frsqrte.cpp -o "$B/test_frsqrte_arm" -static-pie 2>/dev/null || \
   clang++ --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fno-math-errno -DNP_NATIVE=1 -DNP_TARGET_ARMHF=1 -Iinclude -I. tools/det/test_frsqrte.cpp -o "$B/test_frsqrte_arm" 2>/dev/null; }; then
    echo "== armhf (qemu)"; ran=1; "$QEMU" -cpu cortex-a9 -L /usr/arm-linux-gnueabihf "$B/test_frsqrte_arm" || rc=1
else echo "armhf leg skipped (needs qemu-arm and an armhf C++ runtime to link)"; fi
if clang++ -m32 -msse2 -mfpmath=sse -O2 -fno-math-errno -DNP_NATIVE=1 -Iinclude -I. -isystem /usr/include/x86_64-linux-gnu tools/det/test_frsqrte.cpp -o "$B/test_frsqrte_h32" 2>/dev/null; then
    echo "== host32"; ran=1; "$B/test_frsqrte_h32" || rc=1
else echo "host32 leg skipped (no 32-bit multilib)"; fi
[ $ran = 0 ] && { echo "SKIP frsqrte: neither the armhf leg (qemu-arm + armhf runtime) nor the host32 leg (multilib) can be built here"; exit 77; }
exit $rc
