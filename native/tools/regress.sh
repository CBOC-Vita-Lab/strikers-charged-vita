#!/bin/bash
# Regression suite of the native port (NPB3). Run it after every merge into main, as ONE heavy job:
#   /home/claude/msc/tools/heavy.sh -t 590 tools/regress.sh [-x exe] [-q] [-t tag] [-T "boot kickoff fulltime survey movie det"]
#
#   -x exe   the executable (default build/np/host32/np-host32-full-null, the vgx null back end: no Mesa, fast; with -q build/np/armhf/np-armhf-full)
#   -q       run the armhf executable under qemu-arm (cortex-a9) instead of the host32 one (3-5x slower per frame, boot/kickoff/det only by default)
#   -t tag   name of this run (logs in build/np/B3/regress/<tag>-<test>.log, summary in build/np/B3/regress/<tag>.txt)
#   -T list  the tests to run (default: unit boot kickoff fulltime fulltime30 survey movie det detfresh; with -q: boot kickoff det)
#
# Tests (all use the deterministic virtual clock, a private data directory without a save file, and the oracle's / our nav scripts):
#   boot      ESRB -> intro movie skipped -> title screen -> script exit                         (tools/nav/boot_title.txt)
#   kickoff   the oracle's nav_kickoff.txt: menus, captains, stadium, loading, walk-out, kick-off, open play, a few hundred frames of play
#   fulltime  the whole 3-minute match to full time, the post-game screen, DONE, the rematch      (tools/nav/nav_fulltime.txt)
#   fulltime30 the same at the default 30 fps presentation: every hold of the script halved       (tools/fps/nav/nav_fulltime30.txt, made by tools/fps/make_fulltime30.py; fulltime itself runs at MSC_FPS=60)
#   survey    goals, replays, pause menu on and off                                               (tools/nav/survey_full.txt)
#   (every simulation test runs with NP_DET_NAN=1: a non-finite ball or character position/velocity/speed fails the test)
#   movie     the intro movie plays to its end by itself and the title follows                    (tools/nav/movie_end.txt)
#   det       determinism: the kickoff script twice; the second time with an environment 4 KB bigger, the heaps mapped elsewhere, stack and heap
#             garbage changed (NP_STACK_POISON, NP_HEAP_POISON): the per-step simulation digests (NP_DET_TRACE) must be identical, and there must be 500+ steps
#   detfresh  the same with a fresh data directory (the first-run 'create save data' popup path); det starts from a data dir with a save file
#   unit      the unit tests: platform/harness (host32 and x86-64, mock game) and platform/sdk `check` (host32): builds into build/np/B3/unit-*,
#             about 2-3 minutes the first time, seconds after (not in the default list with -q)
#
# A test passes when the process exits 0 by itself (the script's `exit`), no TIMEOUT / crash / stub-abort line is in the log, and every expected marker
# is there. The summary (one line per test: PASS/FAIL, frames, wall seconds) is printed and written to the summary file. Exit status 0 = all pass.
NP=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)   # the worktree this script lives in (DEC: runs in any worktree)
B=${BUILD_ROOT:-/home/claude/msc/build/np}
EXE=; Q=0; TAG=$(date +%m%d-%H%M); LIST=
while getopts "x:qt:T:" o; do case $o in x) EXE=$OPTARG;; q) Q=1;; t) TAG=$OPTARG;; T) LIST=$OPTARG;; *) exit 2;; esac; done
[ -n "$EXE" ] || { [ $Q = 1 ] && EXE=$B/armhf/np-armhf-full || EXE=$B/host32/np-host32-full-null; }
[ -n "$LIST" ] || { [ $Q = 1 ] && LIST="boot kickoff det detfresh" || LIST="unit boot kickoff fulltime fulltime30 survey movie det detfresh"; }
OUT=$B/B3/regress; mkdir -p $OUT
SUM=$OUT/$TAG.txt
NAV=$NP/tools/nav
ORACLE_KICKOFF=/home/claude/msc/tools/scripts/nav_kickoff.txt
QEMU=/home/claude/research/qemu-full/build/qemu-arm

# name|script|max frames|wall seconds (host32; x3 under qemu)|markers
declare -A SCRIPT FRAMES SECS MARK SAVE XENV   # SAVE[t]=1: the data dir starts with a save file (no first-run popup)
SCRIPT[boot]=$NAV/boot_title.txt;        FRAMES[boot]=3000;   SECS[boot]=60;   MARK[boot]="STATE title screen"
SCRIPT[kickoff]=$ORACLE_KICKOFF;         FRAMES[kickoff]=4300; SECS[kickoff]=120; MARK[kickoff]="STATE title screen;STATE main menu;captains confirmed;stadium confirmed;STATE kick-off;STATE open play"
SCRIPT[fulltime]=$NAV/nav_fulltime.txt;  FRAMES[fulltime]=40000; SECS[fulltime]=240; MARK[fulltime]="STATE open play;STATE full time;STATE post-game: DONE button visible;STATE results done, rematch started;script end"
# fulltime: the script plays one whole match with 10800 frames of holds = 3 minutes at 60 fps pacing, so it runs at MSC_FPS=60 (W5 FPS: at the default 30 the holds cover twice the game time,
# the first match ends under them and a rematch and the series-complete pop-up follow). fulltime30: the same script with every hold halved (tools/fps/make_fulltime30.py), at the default 30.
XENV[fulltime]="MSC_FPS=60"
SCRIPT[fulltime30]=$NP/tools/fps/nav/nav_fulltime30.txt;  FRAMES[fulltime30]=40000; SECS[fulltime30]=240; MARK[fulltime30]="STATE open play;STATE full time;STATE post-game: DONE button visible;STATE results done, rematch started;script end"
SCRIPT[survey]=$NAV/survey_full.txt;     FRAMES[survey]=30000; SECS[survey]=200; MARK[survey]="STATE open play;STATE pausing (survey);STATE resumed (survey)"
SCRIPT[movie]=$NAV/movie_end.txt;        FRAMES[movie]=14000; SECS[movie]=150;  MARK[movie]="STATE intro movie playing;STATE title screen reached by itself"
SCRIPT[det]=/home/claude/msc/tools/scripts/nav_match60.txt; FRAMES[det]=4600;    SECS[det]=120;    MARK[det]="STATE open play"; SAVE[det]=1
SCRIPT[detfresh]=/home/claude/msc/tools/scripts/nav_match60.txt; FRAMES[detfresh]=4600; SECS[detfresh]=120; MARK[detfresh]="STATE open play"

# one run: run_one <test> <logfile> [padBytes]; prints "<rc> <frames> <wall>"
run_one() {
  local t=$1 log=$2 pad=${3:-0} D=$OUT/data-$TAG-$1-$$ T0=$(date +%s) rc fr
  rm -rf $D; mkdir -p $D
  [ -n "${SAVE[$t]}" ] && cp -a ${SAVEDIR:-/home/claude/msc/build/np/B3/rundata-save}/. $D/
  local tmo=${SECS[$t]}; [ $Q = 1 ] && tmo=$((tmo * 3))
  (
    cd $D
    export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=${SCRIPT[$t]} MSC_MAX_FRAMES=${FRAMES[$t]} MSC_VIRTUAL_TIME=1 \
           MSC_FRAME_LOG=100000 MSC_WATCH=1 MSC_WATCH_EVERY=100000 MSC_STATE_HASH=${HASHEVERY:-30} NP_DET_NAN=1
    [ "$pad" != 0 ] && export NPB3PAD=$(head -c $pad /dev/zero | tr '\0' 'x')
    [ -n "$TRACE" ] && export NP_DET_TRACE=$TRACE
    [ -n "${XENV[$t]}" ] && export ${XENV[$t]}
    [ -n "$EXTRA_ENV" ] && export $EXTRA_ENV
    if [ $Q = 1 ]; then timeout -k 3 $tmo $QEMU -cpu cortex-a9 -L /usr/arm-linux-gnueabihf $EXE > $log 2>&1
    else timeout -k 3 $tmo $EXE > $log 2>&1; fi
    echo "rc=$?" >> $log
  )
  rc=$(tail -1 $log | sed 's/rc=//')
  fr=$(grep -a "^\[plat\] script exit at frame" $log | tail -1 | grep -o 'frame [0-9]*' | grep -o '[0-9]*')
  rm -rf $D
  echo "$rc ${fr:-0} $(( $(date +%s) - T0 ))"
}

# check <test> <log>: prints "" when fine, else the reasons
check() {
  local t=$1 log=$2 why="" m
  [ "$(tail -1 $log)" = "rc=0" ] || why="$why exit $(tail -1 $log);"
  grep -aEq "\[script\] TIMEOUT|SIGSEGV|SIGBUS|Segmentation|Aborted|nlBreak|stub (called|abort)|Assertion|terminate called" $log && why="$why crash/timeout line: $(grep -aE '\[script\] TIMEOUT|SIGSEGV|SIGBUS|Segmentation|Aborted|nlBreak|stub (called|abort)|Assertion|terminate called' $log | head -1 | cut -c1-100);"
  grep -aq "^\[det\] .*non-finite" $log && why="$why non-finite value in the simulation: $(grep -a '^\[det\] .*non-finite' $log | head -1 | cut -c1-110);"
  local IFS=';'
  for m in ${MARK[$t]}; do grep -aqF "$m" $log || why="$why missing '$m';"; done
  echo "$why"
}

: > $SUM
echo "== regress $TAG exe=$EXE qemu=$Q $(date '+%F %T')" | tee -a $SUM
FAILS=0; N=0
for t in $LIST; do
  N=$((N+1)); log=$OUT/$TAG-$t.log
  if [ "$t" != unit ] && [ -z "${SCRIPT[$t]}" ]; then echo "$t: UNKNOWN test" | tee -a $SUM; FAILS=$((FAILS+1)); continue; fi
  if [ "$t" = unit ]; then
    T0=$(date +%s); : > $log
    ( make -C $NP/platform/harness TARGET=x86_64 OUT=$B/B3/unit-harness test
      make -C $NP/platform/harness TARGET=host32 OUT=$B/B3/unit-harness test
      make -C $NP/platform/sdk T=host32 B=$B/B3/unit-sdk -j2 check ) >> $log 2>&1; urc=$?
    nfail=$(grep -c '^FAIL' $log); nok=$(grep -c '^ok ' $log)
    wall=$(( $(date +%s) - T0 ))
    if [ $urc = 0 ] && [ "$nfail" = 0 ] && grep -q "all tests passed" $log; then echo "PASS unit ($nok harness cases ok, sdk tests passed) wall=${wall}s" | tee -a $SUM
    else echo "FAIL unit: make rc=$urc, $nfail failing cases (log $log)" | tee -a $SUM; FAILS=$((FAILS+1)); fi
    continue
  fi
  if [ "$t" != det ] && [ "$t" != detfresh ]; then read rc fr wall < <(run_one $t $log 0); why=$(check $t $log); fi
  if [ "$t" = det ] || [ "$t" = detfresh ]; then
    # run A: plain. run B: environment 4 KB bigger, arenas mapped elsewhere (MEM2 below MEM1), stack scribbled with 0xAA before every simulation step,
    # every game-heap allocation filled with 0xA5. Per-step digests of the simulation (src/Game/NpDet.cpp) must be identical.
    TRACE=$OUT/$TAG-$t-a.trace read rc fr wall < <(TRACE=$OUT/$TAG-$t-a.trace run_one $t $log 0); why=$(check $t $log)
    log2=$OUT/$TAG-$t-b.log
    read rc2 fr2 wall2 < <(TRACE=$OUT/$TAG-$t-b.trace EXTRA_ENV="NP_MEM1_BASE=0x8A000000 NP_MEM2_BASE=0x84000000 NP_STACK_POISON=0xAA NP_HEAP_POISON=0xA5" run_one $t $log2 4096)
    why2=$(check $t $log2); [ -n "$why2" ] && why="$why [layout run: $why2]"
    nd=$(wc -l < $OUT/$TAG-$t-a.trace 2>/dev/null || echo 0)
    [ "$nd" -ge 500 ] || why="$why only $nd simulation steps traced;"
    cmpout=$(python3 -I $NP/tools/det/stepcmp.py $OUT/$TAG-$t-a.trace $OUT/$TAG-$t-b.trace 2>&1); [ $? = 0 ] || why="$why traces differ: $(echo "$cmpout" | grep -m1 'first difference' | cut -c1-100);"
    wall=$((wall + wall2)); fr="$fr+$fr2 ($nd steps compared)"
  fi
  if [ -z "$why" ]; then echo "PASS $t frames=$fr wall=${wall}s" | tee -a $SUM
  else echo "FAIL $t frames=$fr wall=${wall}s:$why  (log $log)" | tee -a $SUM; FAILS=$((FAILS+1)); fi
done
echo "REGRESS $([ $FAILS = 0 ] && echo PASS || echo FAIL) $((N-FAILS))/$N  (exe $EXE, $(date '+%T'))" | tee -a $SUM
exit $([ $FAILS = 0 ] && echo 0 || echo 1)
