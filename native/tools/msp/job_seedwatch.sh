#!/bin/bash
# MSP job (one heavy slot, host32 + gdb): which code draws from the simulation's random seed (gNetworkRandomSeed) differently when 30 fps pacing is on?
#   tools/heavy.sh -t 900 tools/msp/job_seedwatch.sh
# Four pairs of runs of the match script at 30 fps (MSC_VIRTUAL_FRAME_US=33000), B = pacing off, C = on, the first 2520 frames (the first difference of the
# step traces of job_pace2.sh was at simulation step 235, about frame 2435):
#   gdbplain  under gdb, no instrumentation env        gdbinstr  under gdb, with the env of job_pace2.sh (state hash, NaN watch, pace log, step trace)
#   trace     without gdb, with the env of job_pace2.sh and the step trace, compared with stepcmp (does the cut at 2520 frames keep the difference?)
# gdb pairs: tools/msp/seedwatch_diff.py lists the first write of the seed that differs (value or callers).
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/host32/np-host32-full-null}
OUT=$B/runs/seedwatch; mkdir -p $OUT
SAVEDIR=${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}
SCRIPT=/home/claude/msc/tools/scripts/nav_match60.txt
run() {  # run <name> <gdb|none> <plain|instr> [env...]
  local n=$1 g=$2 i=$3; shift 3
  local D=$OUT/data-$n-$$; rm -rf $D; mkdir -p $D; cp -a $SAVEDIR/. $D/
  local INSTR=; [ $i = instr ] && INSTR="MSC_STATE_HASH=30 NP_DET_NAN=1 MSC_FRAME_PACING_LOG=1 NP_DET_TRACE=$OUT/$n.trace"
  local CMD=($EXE); [ $g = gdb ] && CMD=(gdb -q -batch -x $NP/tools/msp/seedwatch.gdb $EXE)
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_MAX_FRAMES=${FR:-2520} MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 \
      MSC_WATCH=1 MSC_WATCH_EVERY=100000 NP_WIDESCREEN=0 MSC_VIRTUAL_FRAME_US=33000 $INSTR "$@" \
      timeout -k 3 ${RUNT:-300} "${CMD[@]}" > $OUT/$n.log 2>&1 )
  rm -rf $D
  echo "$n: $(grep -ac '^HIT ' $OUT/$n.log) writes of the seed, $( [ -f $OUT/$n.trace ] && wc -l < $OUT/$n.trace || echo 0) steps traced"
}
run tB none instr MSC_FRAME_PACING=0
run tC none instr
echo "== step traces, no gdb: B vs C"; python3 -I tools/det/stepcmp.py $OUT/tB.trace $OUT/tC.trace 2>&1 | cut -c1-200
run iB gdb instr MSC_FRAME_PACING=0
run iC gdb instr
echo "== gdb, instrumentation env: B vs C"; python3 -I tools/msp/seedwatch_diff.py $OUT/iB.log $OUT/iC.log
run B gdb plain MSC_FRAME_PACING=0
run C gdb plain
echo "== gdb, plain env: B vs C"; python3 -I tools/msp/seedwatch_diff.py $OUT/B.log $OUT/C.log
