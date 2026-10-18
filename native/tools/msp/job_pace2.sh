#!/bin/bash
# MSP job (one heavy slot, host32): does 30 fps pacing change the SIMULATION of a match?  Step-keyed traces (NP_DET_TRACE, src/Game/NpDet.cpp:
# one digest line per fixed simulation step, whatever the presented-frame timing) of the match script, compared with tools/det/stepcmp.py.
#   tools/heavy.sh -t 900 tools/msp/job_pace2.sh
# Runs (match scene, NP_WIDESCREEN=0, virtual clock): A 60 Hz pacing on, A0 60 Hz pacing off, B 30 fps (MSC_VIRTUAL_FRAME_US=33000) pacing off, B2 = B again
# (is the 30 fps run itself repeatable?), C 30 fps pacing on, C2 = C again, and C with only ONE hook site paced (MSC_FRAME_PACING_SITES = 1 camera
# 2 letterbox 4 emission rolls 8 texture animations 16 blinking 32 frame counter): the site whose run differs from B is the one that reaches the simulation.
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/host32/np-host32-full-null}
OUT=$B/runs/pace2; mkdir -p $OUT
SAVEDIR=${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}
SCRIPT=/home/claude/msc/tools/scripts/nav_match60.txt
run() {  # run <name> [env...]
  local n=$1; shift
  local D=$OUT/data-$n-$$ T0=$(date +%s)
  rm -rf $D; mkdir -p $D; cp -a $SAVEDIR/. $D/
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_MAX_FRAMES=${FR:-4600} MSC_VIRTUAL_TIME=1 \
      MSC_FRAME_LOG=100000 MSC_WATCH=1 MSC_WATCH_EVERY=100000 MSC_STATE_HASH=30 NP_DET_NAN=1 NP_WIDESCREEN=0 MSC_FRAME_PACING_LOG=1 NP_DET_TRACE=$OUT/$n.trace "$@" \
      timeout -k 3 ${RUNT:-200} $EXE > $OUT/$n.log 2>&1; echo "rc=$?" >> $OUT/$n.log )
  rm -rf $D
  echo "$n: $(tail -1 $OUT/$n.log), $(wc -l < $OUT/$n.trace) steps traced, $(( $(date +%s) - T0 )) s"
}
cmp() {  # cmp <ref> <other>...: first differing step and fields
  python3 -I tools/det/stepcmp.py "$@" 2>&1 | sed 's/^/    /' | cut -c1-210
}
run A
run A0 MSC_FRAME_PACING=0
run B MSC_VIRTUAL_FRAME_US=33000 MSC_FRAME_PACING=0
run B2 MSC_VIRTUAL_FRAME_US=33000 MSC_FRAME_PACING=0
run C MSC_VIRTUAL_FRAME_US=33000
run C2 MSC_VIRTUAL_FRAME_US=33000
for s in 1 2 4 8 16 32; do run Cs$s MSC_VIRTUAL_FRAME_US=33000 MSC_FRAME_PACING_SITES=$s; done
echo "== A (60 Hz, pacing on) vs A0 (60 Hz, pacing off): the original path"; cmp $OUT/A.trace $OUT/A0.trace
echo "== B (30 fps, pacing off) vs B2 (the same): is the 30 fps run repeatable"; cmp $OUT/B.trace $OUT/B2.trace
echo "== C vs C2 (30 fps, pacing on, twice)"; cmp $OUT/C.trace $OUT/C2.trace
echo "== B vs C (pacing off / on at 30 fps)"; cmp $OUT/B.trace $OUT/C.trace
for s in 1 2 4 8 16 32; do echo "== B vs C with only site $s paced"; cmp $OUT/B.trace $OUT/Cs$s.trace; done
echo "== A vs B (60 Hz and 30 fps, pacing off), A vs C"; cmp $OUT/A.trace $OUT/B.trace $OUT/C.trace
