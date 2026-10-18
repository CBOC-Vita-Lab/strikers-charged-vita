#!/bin/bash
# Builds np-armhf-full (clang Cortex-A9 -O2, null renderer; runs under qemu-arm) into build/np/NPM2/armhf. Resumable (make is incremental):
#   until /home/claude/msc/tools/heavy.sh -t 590 tools/mp/build_armhf.sh; do [ $? = 124 ] || break; done
cd "$(dirname "$0")/../.."
B=${BUILD_ROOT:-/home/claude/msc/build/np/NPM2}
mkdir -p $B/armhf
clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -c tools/perf/extra_stubs.c -o $B/armhf/extra_stubs.o || exit 1
make -j2 TARGET=armhf LDEXTRA="$B/armhf/extra_stubs.o" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 EXESUF=-full BUILD_ROOT=$B DEBUG=-g0 OPT=-O2 NOPIC=1 "$@" 2>&1 \
  | grep -v -E 'overriding recipe|ignoring old recipe' | grep -E 'FAIL|error|unresolved|Error' | head -${TAILN:-30}
rc=${PIPESTATUS[0]}
ls -la $B/armhf/np-armhf-full 2>&1 | tail -1
echo "armhf build rc=$rc"
exit $rc
