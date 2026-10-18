#!/bin/bash
# Build an armhf variant (OPT=-O2 NOPIC=1) from scratch into build/np/<name>: e.g. AUTOINIT=zero or AUTOINIT=pattern. Resumable.
#   heavy.sh -t 900 tools/det/build_armhf_v.sh <name> [make vars]       e.g.  tools/det/build_armhf_v.sh B3z AUTOINIT=zero
N=${1:?name}; shift
cd /home/claude/msc/np
B=/home/claude/msc/build/np/$N; mkdir -p $B
[ -f $B/extra_stubs.o ] || clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -c tools/perf/extra_stubs.c -o $B/extra_stubs.o || exit 1
make -j2 TARGET=armhf LDEXTRA="-Wl,-Map=$B/armhf/np-armhf-full.map $B/extra_stubs.o" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 EXESUF=-full BUILD_ROOT=$B DEBUG=-g0 OPT=-O2 NOPIC=1 "$@" 2>&1 \
  | tee -a $B/make.log | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -${TAILN:-15}
rc=${PIPESTATUS[0]}
ls -la $B/armhf/np-armhf-full 2>&1 | tail -1
exit $rc
