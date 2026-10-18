#!/bin/bash
# MSP job (one heavy slot, host32 + gdb): WHICH pacing site changes the simulation of the 30 fps match (job_pace3.sh: pacing on and off agree up to simulation step 637, then
# differ), and which draw of the simulation seed comes first.  Without the harness state hash.
#   tools/heavy.sh -t 900 tools/msp/job_pace4.sh        (needs runs/pace3/mB.trace of job_pace3.sh)
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/host32/np-host32-full-null}
OUT=$B/runs/pace4; mkdir -p $OUT
SAVEDIR=${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}
M=/home/claude/msc/tools/scripts/nav_match60.txt
run() {  # run <name> <gdb|none> <frames> [env...]
  local n=$1 g=$2 f=$3; shift 3
  local D=$OUT/data-$n-$$ T0=$(date +%s); rm -rf $D; mkdir -p $D; cp -a $SAVEDIR/. $D/
  local CMD=($EXE); local TR="NP_DET_TRACE=$OUT/$n.trace"; [ $g = gdb ] && { CMD=(gdb -q -batch -x $NP/tools/msp/seedwatch.gdb $EXE); TR=; }
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$M MSC_MAX_FRAMES=$f MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 \
      MSC_WATCH=1 MSC_WATCH_EVERY=100000 NP_WIDESCREEN=0 MSC_VIRTUAL_FRAME_US=33000 $TR "$@" \
      timeout -k 3 ${RUNT:-300} "${CMD[@]}" > $OUT/$n.log 2>&1 )
  rm -rf $D
  echo "$n: $(grep -ac '^HIT ' $OUT/$n.log) writes, $( [ -f $OUT/$n.trace ] && wc -l < $OUT/$n.trace || echo 0) steps, $(( $(date +%s) - T0 )) s"
}
REF=$B/runs/pace3/mB.trace
for s in 1 2 4 8 16 32; do
  run mCs$s none 4600 MSC_FRAME_PACING_SITES=$s
  echo "== B vs C with only site $s paced"; python3 -I tools/det/stepcmp.py $REF $OUT/mCs$s.trace 2>&1 | sed -n '2,4p' | cut -c1-210
done
run gB gdb ${GFR:-2800} MSC_FRAME_PACING=0
run gC gdb ${GFR:-2800}
echo "== seed writes, gdb, no instrumentation: B vs C"; python3 -I tools/msp/seedwatch_diff.py $OUT/gB.log $OUT/gC.log
