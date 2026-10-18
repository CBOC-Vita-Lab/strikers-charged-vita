#!/bin/bash
# NPS2 diagnostic: the whole game (full-null host32) in virtual time for ~50 s; every few seconds gdb samples the main thread
# (virtual clock, VIC sources, stack). Use when a virtual-time run seems stuck.   heavy.sh -t 240 tools/nps2_vthang.sh [script]
cd "$(dirname "$0")/.."
ROOT=/home/claude/msc/build/np/NPS2x
S=$(readlink -f ${1:-tools/nav/boot_title.txt})
[ "$NOBUILD" = 1 ] || make -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 GX_BACKEND=null WITH_PLAT=1 EXESUF=-full-null OPT=-O1 DEBUG=-g0 BUILD_ROOT=$ROOT 2>&1 | grep -E "^FAIL|error:|unresolved references" | head -20
EXE=$ROOT/host32/np-host32-full-null
LOG=${LOG:-$ROOT/vthang.log}
GDBLOG=${GDBLOG:-$ROOT/vthang.gdb}
D=$ROOT/run-vthang; rm -rf $D $GDBLOG; mkdir -p $D
cd $D
VIRT=${VIRT:-1}; [ "$VIRT" = 1 ] && export MSC_VIRTUAL_TIME=1
env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$S MSC_WATCH=1 MSC_WATCH_EVERY=${WATCH_EVERY:-60} MSC_STATE_HASH=60 MSC_MAX_FRAMES=${MAXF:-1200} MSC_FRAME_LOG=0 $EXTRA_ENV $EXE > $LOG 2>&1 &
PID=$!
sleep ${SAMPLE_DELAY:-12}
for k in 1 2 3 4 5 6; do
  echo "=== sample $k $(date +%T) cpu=$(ps -o cputime= -p $PID)" >> $GDBLOG
  timeout 40 gdb -p $PID -batch -ex 'x/gx &npclock::g_ns' -ex 'x/gx &npclock::g_tickNs' -ex 'x/gx &npclock::g_reads' -ex 'x/gx &npclock::g_mode' -ex 'x/2wx &g_nsrc' -ex 'x/24wx &g_src' -ex 'x/gx &np_vt_hint' -ex 'bt 16' -ex 'info threads' >> $GDBLOG 2>&1
  sleep 5
done
kill -TERM $PID 2>/dev/null; sleep 2; kill -KILL $PID 2>/dev/null
grep -c . $LOG; grep -E '^\[nav\]|^\[hash\]' $LOG | tail -5
