#!/bin/bash
# One whole-match run (host32 null renderer, virtual time) with the stats watch, output under $OUT (default build/np/NPG2/runs).
#   heavy.sh -t 300 tools/stats/run_seed.sh <name> <seed> [exe] [script]
# seed -> MSC_VIRTUAL_TICK_NS = 1000 + 37*seed (as tools/stats/multi.sh).  Extra env (NP_GOALIE_TRACE=1 ...) passes through.
# Log: $OUT/<name>.log   (the last [watch] line has the per-player statistics: tools/stats/summary.py)
NAME=$1; SEED=$2
NP=$(cd "$(dirname "$0")/../.." && pwd)
EXE=${3:-/home/claude/msc/build/np/NPG2/host32/np-host32-full-null}
SCRIPT=${4:-$NP/tools/nav/nav_stats.txt}
OUT=${OUT:-/home/claude/msc/build/np/NPG2/runs}
mkdir -p $OUT; D=$OUT/data-$NAME; rm -rf $D; mkdir -p $D
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D
if [ -n "$GOALIE_WATCH" ]; then W="$(python3 $NP/tools/stats/mkwatch_goalie.py native --stats)"; EVERY=${MSC_WATCH_EVERY:-60}; else W="$(python3 $NP/tools/stats/mkwatch.py native)"; EVERY=${MSC_WATCH_EVERY:-100000}; fi
export MSC_INPUT_SCRIPT=$SCRIPT MSC_WATCH="${MSC_WATCH:-$W}" MSC_WATCH_EVERY=$EVERY MSC_FRAME_LOG=${MSC_FRAME_LOG:-100000}
export MSC_MAX_FRAMES=${MSC_MAX_FRAMES:-30000} MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_TICK_NS=$((1000 + 37 * SEED))
cd $NP
timeout -k 5 ${TMO:-250} /home/claude/msc/npw/vgx-np/native/test/run32.sh $EXE > $OUT/$NAME.log 2>&1
echo "rc=$? $(date +%T)" >> $OUT/$NAME.log
rm -rf $D
grep -o 'score=[0-9]*-[0-9]*' $OUT/$NAME.log | tail -1
