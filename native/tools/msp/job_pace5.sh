#!/bin/bash
# MSP job (one heavy slot, host32 + gdb): is the 30 fps difference between pacing off (B) and on (C) really the AI's script time budget?  Run the same
# 4600-frame match (nav_match60.txt, virtual time, 33 ms frames, no state hash) four times under gdb: pacing off/on, with the AI script time budget left
# at what the game sets in a single-player match (1.0; "control") and forced to -1 (what network and recorded matches use: no script is skipped,
# no draw from nlDefaultSeed in CheckScriptTimeBudget).  Then compare the simulation traces.
#   tools/heavy.sh -t 900 tools/msp/job_pace5.sh        (needs runs/pace3/mB.trace, mC.trace of job_pace3.sh)
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/host32/np-host32-full-null}
OUT=$B/runs/pace5; mkdir -p $OUT
SAVEDIR=${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}
M=/home/claude/msc/tools/scripts/nav_match60.txt
run() {  # run <name> <budget> <frames> [env...]
  local n=$1 nb=$2 f=$3; shift 3
  local D=$OUT/data-$n-$$ T0=$(date +%s); rm -rf $D; mkdir -p $D; cp -a $SAVEDIR/. $D/
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$M MSC_MAX_FRAMES=$f MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 \
      MSC_WATCH=1 MSC_WATCH_EVERY=100000 NP_WIDESCREEN=0 MSC_VIRTUAL_FRAME_US=33000 NP_DET_TRACE=$OUT/$n.trace "$@" \
      timeout -k 3 ${RUNT:-300} gdb -q -batch -ex "set \$nb = $nb" -x $NP/tools/msp/budget.gdb $EXE > $OUT/$n.log 2>&1 )
  rm -rf $D
  echo "$n: $(grep -a '^BUDGET' $OUT/$n.log | tr '\n' ';'), $(wc -l < $OUT/$n.trace) steps, $(( $(date +%s) - T0 )) s"
}
cmp() { echo "== $1 vs $2"; python3 -I tools/det/stepcmp.py $1 $2 2>&1 | sed -n '1,3p' | cut -c1-210; }
F=${FR:-4600}
run bc1 1.0  $F MSC_FRAME_PACING=0     # control: budget as the game sets it, pacing off
run cc1 1.0  $F                        # control: pacing on
run bn1 -1.0 $F MSC_FRAME_PACING=0     # budget -1, pacing off
run cn1 -1.0 $F                        # budget -1, pacing on
cmp $B/runs/pace3/mB.trace $OUT/bc1.trace     # gdb does not change the run (pacing off)
cmp $B/runs/pace3/mC.trace $OUT/cc1.trace     # gdb does not change the run (pacing on)
cmp $OUT/bc1.trace $OUT/cc1.trace             # control: pacing off vs on, budget 1.0 (expected to differ, as job_pace3)
cmp $OUT/bn1.trace $OUT/cn1.trace             # the question: pacing off vs on, budget -1
