#!/bin/bash
# MSP job (one heavy slot, host32): is the SIMULATION the same with 30 fps pacing on and off?  Step-keyed traces (NP_DET_TRACE: one digest line per fixed
# simulation step: all, rng, game, ball, both teams, ten players) of the whole match script and of the survey (goals, replays, pause) at 30 fps
# (MSC_VIRTUAL_FRAME_US=33000), pacing off (B) and on (C), WITHOUT the harness state hash (MSC_STATE_HASH changes the AI draw order of the 30 fps runs, see
# wave4-MSP.md section 4: the state-hash sampling is not neutral at 30 fps).
#   tools/heavy.sh -t 900 tools/msp/job_pace3.sh
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/host32/np-host32-full-null}
OUT=$B/runs/pace3; mkdir -p $OUT
SAVEDIR=${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}
run() {  # run <name> <script> <frames> [env...]
  local n=$1 s=$2 f=$3; shift 3
  local D=$OUT/data-$n-$$ T0=$(date +%s); rm -rf $D; mkdir -p $D; cp -a $SAVEDIR/. $D/
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$s MSC_MAX_FRAMES=$f MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 \
      MSC_WATCH=1 MSC_WATCH_EVERY=100000 NP_WIDESCREEN=0 MSC_VIRTUAL_FRAME_US=33000 NP_DET_TRACE=$OUT/$n.trace "$@" \
      timeout -k 3 ${RUNT:-250} $EXE > $OUT/$n.log 2>&1; echo "rc=$?" >> $OUT/$n.log )
  rm -rf $D
  echo "$n: $(tail -1 $OUT/$n.log), $(wc -l < $OUT/$n.trace) steps traced, $(( $(date +%s) - T0 )) s"
}
M=/home/claude/msc/tools/scripts/nav_match60.txt
S=$NP/tools/nav/survey_full.txt
run mB $M 4600 MSC_FRAME_PACING=0
run mC $M 4600
run sB $S 30000 MSC_FRAME_PACING=0
run sC $S 30000
echo "== match, 30 fps: pacing off (B) vs on (C)"; python3 -I tools/det/stepcmp.py $OUT/mB.trace $OUT/mC.trace 2>&1 | cut -c1-210
echo "== survey, 30 fps: pacing off (B) vs on (C)"; python3 -I tools/det/stepcmp.py $OUT/sB.trace $OUT/sC.trace 2>&1 | cut -c1-210
