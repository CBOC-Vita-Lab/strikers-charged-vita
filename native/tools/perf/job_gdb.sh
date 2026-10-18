#!/bin/bash
# NPP: run a variant under qemu-arm's gdb stub until it crashes, print the backtrace.
#   tools/heavy.sh -t 300 tools/perf/job_gdb.sh     env: V=tuned NAV=<nav script> MAXF=4000
cd /home/claude/msc/npw/NPP2 || exit 1
B=/home/claude/msc/build/np/NPP2
V=${V:-tuned}; EXE=$B/$V/armhf/np-armhf-full; PORT=${PORT:-23456}
rm -rf $B/gdb-data; cp -a $B/rundata $B/gdb-data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$B/gdb-data MSC_FRAMESEND=${FS:-thread} MSC_MAX_FRAMES=${MAXF:-9000} MSC_VIRTUAL_TIME=1 MSC_NO_CRASH_HANDLER=1 MSC_WATCH=1 MSC_WATCH_EVERY=500 MSC_VIRTUAL_FRAME_US=33000
export MSC_INPUT_SCRIPT=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
for kv in $XVARS; do export "$kv"; done   # e.g. XVARS="MSC_FS_TRACE=1"
timeout -k 3 ${TMO:-240} /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf -g $PORT $EXE > $B/gdb-run.log 2>&1 &
Q=$!
sleep 2
timeout -k 3 ${TMO:-240} gdb-multiarch -batch -nx -ex "set pagination off" -ex "set architecture arm" -ex "file $EXE" -ex "target remote :$PORT" \
  -ex "handle SIGUSR1 SIGUSR2 SIGALRM SIGPIPE nostop noprint pass" ${BRK:+-ex "break *$BRK"} -ex "continue" -ex "info registers pc sp lr r0 r1 r2 r3" -ex "bt 30" -ex "x/8i \$pc-8" -ex "info threads" -ex "thread apply all bt 12" \
  2>&1 | grep -v "^warning\|^Reading\|^\[New\|^\[Switching" | cut -c1-200 | tail -60
kill $Q 2>/dev/null
grep -a '^\[fs\]' $B/gdb-run.log | tail -${TRACEN:-30} | cut -c1-260
tail -5 $B/gdb-run.log | cut -c1-200
