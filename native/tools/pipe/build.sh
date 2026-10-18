#!/bin/bash
# NPP: build the armhf "full" executable (game + SDK + GX/vgx null back end + NPV platform/harness) of the current worktree
# into /home/claude/msc/build/np/W5PIPE/<variant>/armhf/np-armhf-full.   Resumable (make is incremental): call it again after a timeout.
#   tools/heavy.sh -t 590 tools/pipe/build.sh <variant> [make vars: OPT=-O2 LTO=thin ...]
# The unfiltered compiler output goes to <build>/<variant>/make.log; MAKEWARN="-Wall ..." is passed as WARN= (the Makefile default is -w).
V=${1:?variant}; shift
SRC=${SRC:-/home/claude/msc/npw/W5PIPE}   # SRC=<dir>: build another source tree (a git archive snapshot) into the same variant layout
cd $SRC || exit 1
B=/home/claude/msc/build/np/W5PIPE/$V
mkdir -p $B
echo "== build $V $* $(date +%T)"
# gap symbols that only the optimised objects reference (see extra_stubs.c)
clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -c tools/pipe/extra_stubs.c -o $B/extra_stubs.o || exit 1
make -j2 TARGET=armhf LDEXTRA="-Wl,-Map=$B/armhf/np-armhf-full.map $B/extra_stubs.o" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 EXESUF=-full BUILD_ROOT=$B DEBUG=-g0 ${MAKEWARN:+"WARN=$MAKEWARN"} "$@" 2>&1 \
  | tee $B/make.log | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -${TAILN:-25}
rc=${PIPESTATUS[0]}
ls -la $B/armhf/np-armhf-full 2>&1 | tail -1
echo "== done $V rc=$rc $(date +%T)"
exit $rc
