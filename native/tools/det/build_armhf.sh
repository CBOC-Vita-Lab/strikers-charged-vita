#!/bin/bash
# Build the armhf executable (OPT=-O2 NOPIC=1, the flags of NPP's tuned3 and of the Vita proxy) of the current tree into the private root build/np/B3a
# (initial copy of NPP/tuned3's objects, mtimes refreshed so only changed files rebuild). Resumable: call again after a timeout.
#   heavy.sh -t 590 tools/det/build_armhf.sh [extra make vars, e.g. LTO=thin]
cd /home/claude/msc/np
B=/home/claude/msc/build/np/B3a
[ -f $B/extra_stubs.o ] || clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -c tools/perf/extra_stubs.c -o $B/extra_stubs.o || exit 1
make -j2 TARGET=armhf LDEXTRA="-Wl,-Map=$B/armhf/np-armhf-full.map $B/extra_stubs.o" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 EXESUF=-full BUILD_ROOT=$B DEBUG=-g0 OPT=-O2 NOPIC=1 "$@" 2>&1 \
  | tee $B/make.log | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -${TAILN:-15}
rc=${PIPESTATUS[0]}
ls -la $B/armhf/np-armhf-full 2>&1 | tail -1
exit $rc
