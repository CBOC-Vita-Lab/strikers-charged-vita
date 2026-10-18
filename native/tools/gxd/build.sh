#!/bin/bash
# GXD (wave 5): build the armhf "full" executable (game + SDK + GX glue + vgx null back end + platform harness) of the np worktree for ONE vgx source tree.
#   tools/heavy.sh -t 590 tools/gxd/build.sh <variant> <vgx source dir> [make vars...]
# The game objects are shared by all variants (BUILD_ROOT=$B); only the glue and vgx libraries (GX_BUILD) and the executable differ:
#   $B/armhf/np-armhf-full-<variant>   libraries in $B/armhf/gx-<variant>   map in $B/armhf/np-armhf-full-<variant>.map
# Resumable (make is incremental): call it again after a timeout.
V=${1:?variant}; VGX=${2:?vgx source dir}; shift 2
SRC=${SRC:-/home/claude/msc/npw/W5GXD}
B=${GXDB:-/home/claude/msc/build/np/W5GXD/b}
L=$(dirname $B)
# A queued job that is no longer needed (the W5 integration build covers it) is made a no-op without killing it: touch $L/skip-<variant>
if [ -e $L/skip-$V ]; then echo "== build $V skipped ($L/skip-$V)"; exit 0; fi
# Late binding (a job queued long before its code is final): L/late-<variant> holds "<branch or commit> <new variant name>"; the vgx tree of <variant> is made
# from that commit when the job runs (once; the commit is recorded in L/late-<variant>.done) and the executable gets the new name (L/variant-<variant>).
if [ -f $L/late-$V ]; then
  read BR NV < $L/late-$V
  bash $SRC/tools/gxd/snap.sh $V $BR > /dev/null || exit 1
  echo "$BR $(git -C ${DEV:-/home/claude/msc/npw/W5GXD-vgx-dev} rev-parse --short $BR)" > $L/late-$V.done
  echo $NV > $L/variant-$V; rm -f $L/late-$V
fi
[ -f $L/variant-$V ] && V=$(cat $L/variant-$V)
mkdir -p $B/armhf
cd $SRC || exit 1
echo "== build $V vgx=$VGX $* $(date +%T)"
clang --target=arm-linux-gnueabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O1 -c tools/perf/extra_stubs.c -o $B/extra_stubs.o || exit 1
make -j2 TARGET=armhf OPT=-O2 NOPIC=1 LDEXTRA="-Wl,-Map=$B/armhf/np-armhf-full-$V.map $B/extra_stubs.o" WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 \
  EXESUF=-full-$V BUILD_ROOT=$B DEBUG=-g0 GX_VGX=$VGX GX_BUILD=$B/armhf/gx-$V "$@" 2>&1 \
  | tee $B/make-$V.log | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -${TAILN:-25}
rc=${PIPESTATUS[0]}
ls -la $B/armhf/np-armhf-full-$V 2>&1 | tail -1
echo "== done $V rc=$rc $(date +%T)"
# a script L/post-<variant>.sh runs in the same heavy turn (the queue wait is longer than the build and a run)
if [ $rc = 0 ] && [ -f $L/post-$V.sh ]; then echo "== post-$V.sh $(date +%T)"; bash $L/post-$V.sh; fi
exit $rc
