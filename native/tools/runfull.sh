#!/bin/bash
# Runs np-<target>-full (default host32, with the private 32-bit Mesa) on a nav script, from a private data dir.
#   heavy.sh -t 600 tools/runfull.sh [-n name] [-s script] [-t seconds] [-f maxframes] [-d dumpEvery|-l dumplist] [-k] [-e watchEvery] [-V]
#   -n  run name (log: build/np/B2/<name>.log, data: build/np/B2/data-<name>, frames: build/np/B2/frames-<name>)
#   -s  nav script (default tools/nav/boot_title.txt)   -t  wall-clock limit (default 300)
#   -f  MSC_MAX_FRAMES   -d  MSC_DUMP_FRAMES=n   -l  MSC_DUMP_LIST=a,b-c   -k  keep the data dir (default: recreate = no save file)
#   -e  MSC_WATCH_EVERY (default 60)   -V  MSC_VIRTUAL_TIME=1
# Extra variables pass through from the environment (NP_STUB_ABORT=1, MSC_WATCH_BUTTONS=1, ...).
cd $(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=${BUILD_ROOT:-/home/claude/msc/build/np}
NAME=run; SCRIPT=tools/nav/boot_title.txt; TMO=300; FR=; DUMP=; LIST=; KEEP=0; EVERY=60; VT=0; EXE=$B/host32/np-host32-full
while getopts "n:s:t:f:d:l:ke:VX:" o; do case $o in
 n) NAME=$OPTARG;; s) SCRIPT=$OPTARG;; t) TMO=$OPTARG;; f) FR=$OPTARG;; d) DUMP=$OPTARG;; l) LIST=$OPTARG;;
 k) KEEP=1;; e) EVERY=$OPTARG;; V) VT=1;; X) EXE=$OPTARG;; *) exit 2;; esac; done
D=$B/B2/data-$NAME
[ $KEEP = 1 ] || rm -rf $D
mkdir -p $D $B/B2/frames-$NAME
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D
SCRIPT=$(realpath $SCRIPT)
export MSC_INPUT_SCRIPT=$SCRIPT MSC_WATCH=${MSC_WATCH:-1} MSC_WATCH_EVERY=$EVERY MSC_FRAME_LOG=${MSC_FRAME_LOG:-$EVERY}
[ -n "$FR" ] && export MSC_MAX_FRAMES=$FR
[ -n "$DUMP" ] && export MSC_DUMP_FRAMES=$DUMP
[ -n "$LIST" ] && export MSC_DUMP_LIST=$LIST
export MSC_DUMP_DIR=$B/B2/frames-$NAME
[ $VT = 1 ] && export MSC_VIRTUAL_TIME=1
LOG=$B/B2/$NAME.log
echo "== runfull $NAME $(date +%T) script=$SCRIPT exe=$EXE" > $LOG
timeout -k 5 $TMO /home/claude/msc/npw/vgx-np/native/test/run32.sh $EXE >> $LOG 2>&1
echo "rc=$? $(date +%T)" >> $LOG
grep -v '^\[vgx\] frame' $LOG | tail -${TAILN:-25}
