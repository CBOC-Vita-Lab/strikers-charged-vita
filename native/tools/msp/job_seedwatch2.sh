#!/bin/bash
# MSP job (one heavy slot, host32 + gdb): which instrumentation variable makes the 30 fps match with pacing on (C) draw its AI random numbers in another order than
# with pacing off (B)?  (job_seedwatch.sh: identical write sequences of the simulation seed with no instrumentation, a difference at write 440 with
# MSC_STATE_HASH + NP_DET_NAN + MSC_FRAME_PACING_LOG + NP_DET_TRACE.)  One variable at a time, B and C under gdb, 2520 frames each.
#   tools/heavy.sh -t 900 tools/msp/job_seedwatch2.sh
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/host32/np-host32-full-null}
OUT=$B/runs/seedwatch2; mkdir -p $OUT
SAVEDIR=${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}
SCRIPT=/home/claude/msc/tools/scripts/nav_match60.txt
run() {  # run <name> <var=value> [env...]
  local n=$1 v=$2; shift 2
  local D=$OUT/data-$n-$$; rm -rf $D; mkdir -p $D; cp -a $SAVEDIR/. $D/
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_MAX_FRAMES=${FR:-2520} MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 \
      MSC_WATCH=1 MSC_WATCH_EVERY=100000 NP_WIDESCREEN=0 MSC_VIRTUAL_FRAME_US=33000 $v "$@" \
      timeout -k 3 ${RUNT:-300} gdb -q -batch -x $NP/tools/msp/seedwatch.gdb $EXE > $OUT/$n.log 2>&1 )
  rm -rf $D
  echo "$n: $(grep -ac '^HIT ' $OUT/$n.log) writes of the seed"
}
for v in MSC_STATE_HASH=30 NP_DET_NAN=1 MSC_FRAME_PACING_LOG=1 NP_DET_TRACE=$OUT/trace.tmp MSC_WATCH_EVERY=30; do
  k=$(echo $v | sed 's/=.*//')
  run ${k}_B $v MSC_FRAME_PACING=0
  run ${k}_C $v
  echo "== $v: B vs C"; python3 -I tools/msp/seedwatch_diff.py $OUT/${k}_B.log $OUT/${k}_C.log | head -12 | cut -c1-200
done
rm -f $OUT/trace.tmp
