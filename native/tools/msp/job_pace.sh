#!/bin/bash
# MSP job (one heavy slot): the 30 fps frame-pacing proof on host32 (np-host32-full-null) with the harness's virtual clock.
#   tools/heavy.sh -t 900 tools/msp/job_pace.sh [scene ...]      scenes: menu hof match survey (default: menu hof match)
# Configurations per scene (MSC_FRAME_PACING_LOG=2 -> [pace] / [pace-ev] lines on stderr):
#   A   60 Hz     one presented frame = one retrace (MSC_VIRTUAL_FRAME_US unset), pacing on      (must be the original path)
#   A0  60 Hz, MSC_FRAME_PACING=0                                                                 (match only: simulation digests must equal A)
#   B   30 fps    MSC_VIRTUAL_FRAME_US=33000 (the game thread needs 33 ms per frame), pacing off  (what 30 fps does to the game today)
#   C   30 fps    the same with pacing on
# Logs: $BUILD_ROOT/runs/pace/<scene>-<cfg>.log ; the table: tools/msp/pace_report.py
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/host32/np-host32-full-null}
OUT=$B/runs/pace; mkdir -p $OUT
SCENES=${*:-menu hof match}
SAVEDIR=${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}
declare -A SCRIPT FRAMES WIDE SAVE
SCRIPT[menu]=$NP/tools/msp/nav/pace_menu.txt;   FRAMES[menu]=${MENUF:-2400};  WIDE[menu]=1; SAVE[menu]=0
SCRIPT[hof]=$NP/tools/msp/nav/pace_hof.txt;     FRAMES[hof]=${HOFF:-4200};    WIDE[hof]=1;  SAVE[hof]=0
SCRIPT[match]=/home/claude/msc/tools/scripts/nav_match60.txt; FRAMES[match]=${MATCHF:-4600}; WIDE[match]=0; SAVE[match]=1
SCRIPT[survey]=$NP/tools/nav/survey_full.txt;   FRAMES[survey]=${SURVEYF:-30000}; WIDE[survey]=0; SAVE[survey]=0
run() {  # run <scene> <cfg> [env...]
  local sc=$1 cfg=$2; shift 2
  local D=$OUT/data-$sc-$cfg-$$ T0=$(date +%s)
  rm -rf $D; mkdir -p $D
  [ "${SAVE[$sc]}" = 1 ] && cp -a $SAVEDIR/. $D/
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=${SCRIPT[$sc]} MSC_MAX_FRAMES=${FRAMES[$sc]} MSC_VIRTUAL_TIME=1 \
      MSC_FRAME_LOG=100000 MSC_WATCH=1 MSC_WATCH_EVERY=100000 MSC_STATE_HASH=30 NP_DET_NAN=1 NP_WIDESCREEN=${WIDE[$sc]} MSC_FRAME_PACING_LOG=2 "$@" \
      timeout -k 3 ${RUNT:-300} $EXE > $OUT/$sc-$cfg.log 2>&1; echo "rc=$?" >> $OUT/$sc-$cfg.log )
  rm -rf $D
  echo "$sc $cfg: $(tail -1 $OUT/$sc-$cfg.log) $(grep -ac '^\[pace\]' $OUT/$sc-$cfg.log) pace lines, $(( $(date +%s) - T0 )) s"
}
for sc in $SCENES; do
  run $sc A
  [ $sc = match ] && run $sc A0 MSC_FRAME_PACING=0
  [ $sc = match ] && run $sc P MSC_PERFORMANCE_MODE=1     # Performance Mode at 60 Hz: the simulation hashes must equal A's
  [ $sc = match ] && run $sc F MSC_GAME_FIXES=all         # the game-fix options (net, electro, nk): the log lines, and whether a plain match differs
  run $sc B MSC_VIRTUAL_FRAME_US=33000 MSC_FRAME_PACING=0
  run $sc C MSC_VIRTUAL_FRAME_US=33000
done
if [ -f $OUT/match-A0.log ] && [ -f $OUT/match-A.log ]; then
  grep -a '^\[hash\]' $OUT/match-A.log > $OUT/match-A.hash; grep -a '^\[hash\]' $OUT/match-A0.log > $OUT/match-A0.hash
  echo "match A (pacing on) vs A0 (pacing off) at 60 Hz: $(wc -l < $OUT/match-A.hash) / $(wc -l < $OUT/match-A0.hash) hash lines, $(diff $OUT/match-A.hash $OUT/match-A0.hash | grep -c '^[<>]') differ"
fi
if [ -f $OUT/match-A.log ]; then
  grep -a '^\[hash\]' $OUT/match-A.log > $OUT/match-A.hash
  for c in P F; do
    [ -f $OUT/match-$c.log ] || continue
    grep -a '^\[hash\]' $OUT/match-$c.log > $OUT/match-$c.hash
    echo "match A vs $c: $(wc -l < $OUT/match-A.hash) / $(wc -l < $OUT/match-$c.hash) hash lines, $(diff $OUT/match-A.hash $OUT/match-$c.hash | grep -c '^[<>]') differ"
  done
  grep -a "^\[fixes\]" $OUT/match-F.log | sort | uniq -c | head
fi
for sc in $SCENES; do   # the same frame times with pacing off and on: the simulation must not notice (hash lines every 30 presented frames)
  if [ -f $OUT/$sc-B.log ] && [ -f $OUT/$sc-C.log ]; then
    grep -a '^\[hash\]' $OUT/$sc-B.log > $OUT/$sc-B.hash; grep -a '^\[hash\]' $OUT/$sc-C.log > $OUT/$sc-C.hash
    echo "$sc B (pacing off) vs C (on) at 30 fps: $(wc -l < $OUT/$sc-B.hash) / $(wc -l < $OUT/$sc-C.hash) hash lines, $(diff $OUT/$sc-B.hash $OUT/$sc-C.hash | grep -c '^[<>]') differ"
  fi
done
python3 -I tools/msp/pace_report.py $OUT $SCENES
