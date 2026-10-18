#!/bin/bash
# One heavy.sh slot doing everything NPB needs after a source change:
#   1. objs + stubs + link of host32 and armhf (tools/job_link.sh)
#   2. the same executables with NPS's SDK layer (WITH_SDK=1, EXESUF=-sdk)
#   3. a short smoke run of each (host32 20 s, armhf under qemu 90 s) with the real disc as data dir
#   4. the vita syntax pass
#   usage: heavy.sh -t 1700 tools/job_full.sh [make vars, default OPT=-O1 DEBUG=-g0]
cd /home/claude/msc/np
VARS="${@:-OPT=-O1 DEBUG=-g0}"
B=/home/claude/msc/build/np
echo "host32 armhf" > $B/.targets
tools/job_link.sh host32 $VARS
rm -f $B/.targets
for T in host32 armhf; do
  echo "== sdk variant $T $(date +%T)"
  make -k -j2 TARGET=$T WITH_SDK=1 EXESUF=-sdk $VARS 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[' | tail -40
  ls -la $B/$T/np-$T-sdk 2>&1 | tail -1
done
for T in host32 armhf; do
  echo "== sdk+gx variant $T $(date +%T)"
  make -k -j2 TARGET=$T WITH_SDK=1 WITH_GX=1 EXESUF=-sdk-gx $VARS 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -30
  ls -la $B/$T/np-$T-sdk-gx 2>&1 | tail -1
done
for T in host32 armhf; do
  echo "== full variant (sdk+gx+plat) $T $(date +%T)"
  make -k -j2 TARGET=$T WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 EXESUF=-full $VARS 2>&1 | grep -v '^clang\|^mkdir\|^g++\|^gcc\|^arm-\|^ar \|^make\[\|^CXX\|^In file\|warning:\|^ *[0-9]* |\|\^~' | tail -20
  ls -la $B/$T/np-$T-full 2>&1 | tail -1
done
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$B/rundata
mkdir -p $NP_DATA_DIR
run() { # name, timeout, command...
  n=$1; t=$2; shift 2
  echo "== smoke $n $(date +%T)"
  ( cd /home/claude/msc/np && timeout $t "$@" > $B/run-$n.log 2>&1; echo "rc=$?" >> $B/run-$n.log )
  head -c 2500 $B/run-$n.log; echo; echo "(log: $B/run-$n.log, $(wc -l < $B/run-$n.log) lines)"
}
QEMU="/home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf"
run host32 20 $B/host32/np-host32
[ -x $B/host32/np-host32-sdk ] && run host32-sdk 20 $B/host32/np-host32-sdk
[ -x $B/armhf/np-armhf-sdk ] && run armhf-sdk 90 $QEMU $B/armhf/np-armhf-sdk
[ -x $B/armhf/np-armhf-sdk-gx ] && MSC_DUMP_FRAMES=1 MSC_DUMP_DIR=$B/dump run armhf-sdk-gx 90 $QEMU $B/armhf/np-armhf-sdk-gx
[ -x $B/host32/np-host32-sdk-gx ] && MSC_DUMP_FRAMES=1 MSC_DUMP_DIR=$B/dump run host32-sdk-gx 40 /home/claude/msc/npw/vgx-np/native/test/run32.sh $B/host32/np-host32-sdk-gx
[ -x $B/host32/np-host32-full ] && MSC_WATCH=1 MSC_WATCH_EVERY=4 MSC_FRAME_LOG=$B/frames.log MSC_STATE_HASH=10 MSC_DUMP_FRAMES=1 MSC_DUMP_DIR=$B/dump run host32-full 60 /home/claude/msc/npw/vgx-np/native/test/run32.sh $B/host32/np-host32-full
echo "== vita syntax $(date +%T)"
tools/syncheck.sh vita src 40 2>&1 | tail -30
echo "== done $(date +%T)"
