#!/bin/bash
# MSP: build an executable of THIS worktree into /home/claude/msc/build/np/MSP/<target>/ (incremental; call again after a timeout).
#   tools/heavy.sh -t 1500 tools/msp/build.sh host32            # np-host32-full-null  (vgx null back end; scripted matches, virtual time)
#   tools/heavy.sh -t 1500 tools/msp/build.sh armhf             # np-armhf-full        (O2, NOPIC, ThinLTO: the console configuration under qemu-arm)
# armhf builds host32 first (one heavy slot gives both executables; NO_HOST32=1 skips that).
# Extra make variables follow the target (e.g. OPT=-O2).  Unfiltered compiler output: build/np/MSP/make-<target>.log
T=${1:?host32|armhf}; shift
if [ $T = armhf ] && [ -z "$NO_HOST32" ]; then
  "$0" host32 || exit $?
  # optional hook (a file the owner leaves here): work for the host32 executable that should run in this same heavy slot before the long armhf build
  [ -x "$(dirname "$0")/after_host32.sh" ] && { "$(dirname "$0")/after_host32.sh"; echo "== after_host32 rc=$?"; }
fi
NP=$(cd "$(dirname "$0")/../.." && pwd)
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
mkdir -p $B; cd $NP || exit 1
echo "== build $T $* $(date +%T)"
if [ $T = host32 ]; then
  make -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 GX_BACKEND=null EXESUF=-full-null BUILD_ROOT=$B OPT=${OPT:--O1} DEBUG=-g0 WARN="${WARN:--Wno-everything -Wtautological-unsigned-enum-zero-compare -Wtautological-unsigned-zero-compare -Wtautological-constant-out-of-range-compare}" "$@" 2>&1 | tee $B/make-$T.log | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -25
  rc=${PIPESTATUS[0]}; ls -la $B/host32/np-host32-full-null 2>&1 | tail -1
else
  clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -c tools/perf/extra_stubs.c -o $B/extra_stubs.o || exit 1
  make -j2 TARGET=armhf LDEXTRA="-Wl,-Map=$B/armhf/np-armhf-full.map $B/extra_stubs.o" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 EXESUF=-full BUILD_ROOT=$B DEBUG=-g0 OPT=${OPT:--O2} NOPIC=1 "$@" 2>&1 | tee $B/make-$T.log | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -25
  rc=${PIPESTATUS[0]}; ls -la $B/armhf/np-armhf-full 2>&1 | tail -1
fi
echo "== done $T rc=$rc $(date +%T)"
exit $rc
