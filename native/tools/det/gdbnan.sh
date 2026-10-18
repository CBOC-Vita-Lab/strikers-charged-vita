#!/bin/bash
# armhf under qemu's gdb stub: stop at the first write of a non-finite value (exponent all ones) into a float at ADDR (taken from the "[det] ... &pos 0x..." line of a
# NP_DET_NAN=1 run of the same script with the same data dir state), print where.
#   heavy.sh -t 600 tools/det/gdbnan.sh <addr> [frames]    (fresh data dir, nav_fulltime, the layout of matrix.sh's `nan` config)
ADDR=${1:?addr}; FR=${2:-6700}; PORT=${PORT:-23457}
B=/home/claude/msc/build/np; NP=/home/claude/msc/np; EXE=${EXE:-$B/B3a/armhf/np-armhf-full}
D=$B/B3/data-gdbnan; rm -rf $D; mkdir -p $D; cd $D
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$NP/tools/nav/nav_fulltime.txt MSC_MAX_FRAMES=$FR MSC_VIRTUAL_TIME=1 \
       MSC_FRAME_LOG=100000 MSC_WATCH_EVERY=100000 MSC_STATE_HASH=30 NP_DET_NAN=1 MSC_WATCH=1 MSC_NO_CRASH_HANDLER=1
timeout -k 3 ${TMO:-500} /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf -g $PORT $EXE > $B/B3/gdbnan-run.log 2>&1 &
Q=$!
sleep 2
timeout -k 3 ${TMO:-500} gdb-multiarch -batch -nx -ex "set pagination off" -ex "set architecture arm" -ex "file $EXE" -ex "target remote :$PORT" \
  -ex "handle SIGUSR1 SIGUSR2 SIGALRM SIGPIPE nostop noprint pass" \
  -ex "break NpDetStepBegin" -ex "continue" -ex "delete 1" \
  -ex "watch *(unsigned int*)$ADDR if (*(unsigned int*)$ADDR & 0x7f800000) == 0x7f800000" -ex "continue" \
  -ex "info registers pc sp lr r0 r1 r2 r3 r4" -ex "bt 16" -ex "x/10i \$pc-16" -ex "info all-registers s0 s1 s2 s3 s4 s5 s6 s7" \
  2>&1 | grep -v "^warning\|^Reading\|^\[New\|^\[Switching" | cut -c1-220 | tail -80
kill $Q 2>/dev/null
grep -a "^\[det\]" $B/B3/gdbnan-run.log | head -3 | cut -c1-250
rm -rf $D
