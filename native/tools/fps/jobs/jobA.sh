#!/bin/bash
# FPS job A (ONE heavy slot, heavy.sh -t 3000): host32 null build of the W5FPS worktree (INT stage-1 flags), then the proof runs on the virtual clock.
# Every run: [ft] <frame> <sim steps> <retraces> <state> per presented frame, [fps] lines, [pace] lines (MSC_FRAME_PACING_LOG=2), NP_DET_TRACE (digest per simulation step),
# MSC_AI_BUDGET=off (the single-player AI does not depend on the clock). Logs: build/np/W5FPS/runs/a/<name>.{log,trace}. Runs that finished are not repeated
# (the script can be run again); a run is not started after BUDGET seconds of the script.
NP=${NP:-/home/claude/msc/npw/W5FPS}
R=${R:-/home/claude/msc/build/np/W5FPS}
EXE=$R/host32/np-host32-full-null
OUT=$R/runs/a; mkdir -p $OUT
SAVEDIR=/home/claude/msc/build/np/B3/rundata-save
BUDGET=${BUDGET:-2500}
T00=$(date +%s)
cd $NP || exit 1
echo "== build $(date +%T) np $(git rev-parse --short HEAD) vgx $(git -C ${GX_VGX:-/home/claude/msc/npw/W5FPS-vgx} rev-parse --short HEAD)"
make -k -j2 TARGET=host32 BUILD_ROOT=$R GX_VGX=${GX_VGX:-/home/claude/msc/npw/W5FPS-vgx} WITH_SDK=1 WITH_GX=1 WITH_PLAT=1 OPT=-O1 DEBUG=-g0 EXESUF=-full-null GX_BACKEND=null > $R/h32null.log 2>&1
echo "== build rc=$? $(date +%T) after $(( $(date +%s) - T00 )) s: $(ls -la $EXE 2>&1 | cut -c25-)"
[ -x $EXE ] || { grep -a "error" $R/h32null.log | head -20; exit 1; }
run() {  # run <name> <script> <frames> <wide> <save> [env...]
  local name=$1 script=$2 frames=$3 wide=$4 save=$5; shift 5
  if [ -f $OUT/$name.log ] && grep -aq "^rc=" $OUT/$name.log; then echo "$name: done before"; return; fi
  if [ $(( $(date +%s) - T00 )) -gt $BUDGET ]; then echo "$name: SKIPPED (budget)"; return; fi
  local D=$OUT/data-$name T0=$(date +%s)
  rm -rf $D; mkdir -p $D; [ $save = 1 ] && cp -a $SAVEDIR/. $D/
  ( cd $D; env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$script MSC_MAX_FRAMES=$frames MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 MSC_WATCH=1 \
      MSC_WATCH_EVERY=100000 MSC_FRAME_TRACE=1 MSC_STATE_HASH=60 NP_DET_NAN=1 NP_DET_TRACE=$OUT/$name.trace NP_WIDESCREEN=$wide MSC_FRAME_PACING_LOG=2 MSC_AI_BUDGET=off \
      MSC_FPS_LOG=1 "$@" timeout -k 3 ${RUNT:-300} $EXE > $OUT/$name.log 2>&1; echo "rc=$?" >> $OUT/$name.log )
  rm -rf $D
  echo "$name: $(tail -1 $OUT/$name.log) $(grep -ac '^\[ft\]' $OUT/$name.log) ft lines, $(( $(date +%s) - T0 )) s"
}
MI=$NP/tools/fps/nav/match_idle.txt
MV=$NP/tools/nav/movie_end.txt
MN=$NP/tools/msp/nav/pace_menu.txt
# open play without any input: the same play at every rate (the digests can be compared); frame lengths 33.3 / 16.7 / mixed / switching
# value order (a cut turn keeps what finished; the budget skips the rest): 30 + auto + reference 60 + movie + menus first
run mi-30      $MI 9000 0 1 MSC_FPS=30
run mi-auto    $MI 9000 0 1 MSC_FPS=auto
run mi-autopred $MI 9000 0 1 MSC_FPS=auto MSC_FPS_MODEL=pred
run mi-60      $MI 9000 0 1 MSC_FPS=60
run mv-30      $MV 9000 0 0 MSC_FPS=30
run mv-auto    $MV 9000 0 0 MSC_FPS=auto
run mn-30      $MN 2400 1 0 MSC_FPS=30
run mn-auto    $MN 2400 1 0 MSC_FPS=auto
run mi-game    $MI 9000 0 1 MSC_FPS=game
run mi-30c40   $MI 9000 0 1 MSC_FPS=30 MSC_VIRTUAL_FRAME_US=40000
run mm-30      /home/claude/msc/tools/scripts/nav_match60.txt 4600 0 1 MSC_FPS=30
run mm-autopred /home/claude/msc/tools/scripts/nav_match60.txt 4600 0 1 MSC_FPS=auto MSC_FPS_MODEL=pred
run mi-unl25   $MI 9000 0 1 MSC_FPS=unlocked MSC_VIRTUAL_FRAME_US=25000
run mv-60      $MV 9000 0 0 MSC_FPS=60
run mv-game    $MV 9000 0 0 MSC_FPS=game
run mn-60      $MN 2400 1 0 MSC_FPS=60
run mn-game    $MN 2400 1 0 MSC_FPS=game
run mn-autopred $MN 2400 1 0 MSC_FPS=auto MSC_FPS_MODEL=pred
run mn-unl25   $MN 2400 1 0 MSC_FPS=unlocked MSC_VIRTUAL_FRAME_US=25000
run mi-60c20   $MI 9000 0 1 MSC_FPS=60 MSC_VIRTUAL_FRAME_US=20000
run mm-60      /home/claude/msc/tools/scripts/nav_match60.txt 4600 0 1 MSC_FPS=60
echo "== jobA done $(date +%T) after $(( $(date +%s) - T00 )) s"
