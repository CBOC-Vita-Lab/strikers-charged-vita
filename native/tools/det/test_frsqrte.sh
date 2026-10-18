#!/bin/bash
# nlSqrt / nlRecipSqrt over the whole float range (tools/det/test_frsqrte.cpp): armhf under qemu (the NEON __frsqrte) and host32. Exit 0 = no wrong value.
#   heavy.sh -t 120 tools/det/test_frsqrte.sh
NP=/home/claude/msc/np; B=/home/claude/msc/build/np/B3; mkdir -p $B; cd $NP; rc=0
clang++ --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fno-math-errno -DNP_NATIVE=1 -DNP_TARGET_ARMHF=1 -Iinclude -I. tools/det/test_frsqrte.cpp -o $B/test_frsqrte_arm -static-pie 2>/dev/null || \
clang++ --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2 -fno-math-errno -DNP_NATIVE=1 -DNP_TARGET_ARMHF=1 -Iinclude -I. tools/det/test_frsqrte.cpp -o $B/test_frsqrte_arm || { echo "armhf build failed"; exit 2; }
echo "== armhf (qemu)"; /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf $B/test_frsqrte_arm || rc=1
clang++ -m32 -msse2 -mfpmath=sse -O2 -fno-math-errno -DNP_NATIVE=1 -Iinclude -I. -isystem /usr/include/x86_64-linux-gnu tools/det/test_frsqrte.cpp -o $B/test_frsqrte_h32 && { echo "== host32"; $B/test_frsqrte_h32 || rc=1; }
exit $rc
