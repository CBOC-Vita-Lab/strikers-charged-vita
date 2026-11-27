#!/bin/bash
# GX producer census as a test. In the two-stage frame-send pipeline the record stream has ONE producer (the send worker). Every GX call or renderer record
# the game thread makes while a send job is pending must be deferred (NpFsDefer) or gated, or two threads write the same words of the stream (the "corrupt
# record stream" abort). The census (MSC_FS_GXCENSUS=1) runs in serial pipelined mode and lists the game-thread records made while a job is pending.
#
# What runs: the host32 null-renderer executable BUILT WITH -DVGX_PRODUCER_CHECK, one run per route, with the Vita player's own default environment
# (tools/pipe/player_env.py reads it from platform/vita/src/vita_plan.cpp: kSpeed, kQuality and the few single defaults, so a new default is tested at once;
# PLAYER_ENV=0 runs the host defaults instead). Routes: the two Mega routes of tools/routes (Mega Saves and Strikes), tools/nav/survey_full.txt (the walk-out, two goals, replays, the pause menu), and the nav script NAV (a match) when given.
# Fails when any run lists a record. Prints, per route, every call site (symbolized with addr2line when it is available).
#
#   GXCENSUS_EXE=<exe> DISC=<extracted disc> SAVE=<data dir to copy> [NAV=<nav_match script>] tools/pipe/run_gxcensus_test.sh
#   GXCENSUS_BUILD=1 [BUILD_ROOT=<dir>] ... builds the census executable first (incremental in BUILD_ROOT; the gate's null build can carry the flag itself:
#                    bash tools/mp/build_host32.sh EXTRA=-DVGX_PRODUCER_CHECK, the hook does nothing without MSC_FS_GXCENSUS)
#   ROUTES="a.txt b.txt" replaces the list of routes; FRAMES=<n> overrides the frame count of every route (default 4400 for NAV, 6000 for the Mega routes, 9000 for the survey).
#   STRESS=<n>  instead of the census, runs every route n times in the Vita's own threading (MSC_FRAMESEND=thread MSC_FS_PIPE=1 MSC_VGX_BACKEND_THREAD=1,
#               MSC_VGX_STREAMTRACE=1) and fails on any "corrupt record stream" abort or any run that does not end with rc=0. Any executable will do (no producer hook needed).
# Skips (exit 0) unless an executable (or GXCENSUS_BUILD=1), DISC and SAVE are given.
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE/../.." || exit 1
if [ -z "$GXCENSUS_EXE" ]; then
  if [ "${GXCENSUS_BUILD:-0}" != 1 ]; then echo "SKIP gxcensus: set GXCENSUS_EXE or GXCENSUS_BUILD=1"; exit 0; fi
  BR=${BUILD_ROOT:-$(pwd)/build/gxcensus}
  BUILD_ROOT=$BR GX_VGX=${GX_VGX:-$(pwd)/../renderer} bash tools/mp/build_host32.sh EXTRA=-DVGX_PRODUCER_CHECK || exit 1
  GXCENSUS_EXE=$BR/host32/np-host32-full-null
fi
[ -x "$GXCENSUS_EXE" ] || { echo "FAIL gxcensus: no executable $GXCENSUS_EXE"; exit 1; }
if [ -z "$DISC" ] || [ -z "$SAVE" ]; then echo "SKIP gxcensus: set DISC (the extracted disc) and SAVE (a data directory to copy)"; exit 0; fi
PENV=()
if [ "${PLAYER_ENV:-1}" != 0 ]; then
  mapfile -t PENV < <(python3 -I "$HERE/player_env.py" platform/vita/src/vita_plan.cpp) || { echo "FAIL gxcensus: player_env.py"; exit 1; }
  [ "${#PENV[@]}" -ge 10 ] || { echo "FAIL gxcensus: the player environment has only ${#PENV[@]} entries"; exit 1; }
fi
echo "gxcensus: ${#PENV[@]} player defaults: ${PENV[*]}"
KS='MSC_WATCH=keepstate:s32:[[g_pTeams]+B4]+334'   # defines keepstate for mega_route.txt (without it the wait is skipped after 400 frames)
if [ -z "$ROUTES" ]; then
  ROUTES="tools/routes/mega_route.txt tools/routes/mega_strike_route.txt tools/nav/survey_full.txt"
  [ -n "$NAV" ] && ROUTES="$NAV $ROUTES"
fi
fail=0
for r in $ROUTES; do
 for run in $(seq 1 "${STRESS:-1}"); do
  case "$r" in /*) route=$r ;; *) route=$(pwd)/$r ;; esac
  name=$(basename "$r" .txt)
  case "$name" in mega_route|mega_strike_route) dflt=6000 ;; survey_full) dflt=9000 ;; *) dflt=4400 ;; esac
  frames=${FRAMES:-$dflt}
  W=$(mktemp -d); cp -a "$SAVE" "$W/data"
  (
    cd "$W/data" || exit 1
    export NP_DISC_DIR=$DISC NP_DATA_DIR=$W/data MSC_INPUT_SCRIPT=$route MSC_MAX_FRAMES=$frames MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=33000 MSC_FRAME_LOG=100000
    for kv in "${PENV[@]}"; do export "$kv"; done
    [ "$name" = mega_route ] && export "$KS"
    if [ -n "$STRESS" ]; then export MSC_FRAMESEND=thread MSC_FS_PIPE=1 MSC_VGX_BACKEND_THREAD=1 MSC_VGX_STREAMTRACE=1
    else export MSC_FRAMESEND=serial MSC_FS_PIPE=1 MSC_FS_GXCENSUS=1; fi
    timeout -k 3 "${TMO:-420}" setarch i386 -R "$GXCENSUS_EXE" > "$W/out.log" 2> "$W/err.log"; echo "rc=$?" >> "$W/err.log"
  )
  if [ -n "$STRESS" ]; then
    c=$(grep -a -c "corrupt record stream" "$W/err.log"); rc=$(tail -1 "$W/err.log")
    echo "gxstress $name run $run: corrupt=$c $rc"
    if [ "$c" != 0 ]; then grep -a "corrupt record: header" "$W/err.log" | head -1 | cut -c1-200; fi
    if [ "$c" != 0 ] || [ "$rc" != "rc=0" ]; then fail=1; fi
    rm -rf "$W"; continue
  fi
  line=$(grep -a "census: .* game-thread records while a job was pending" "$W/err.log" | tail -1)
  nf=$(grep -a -c '' "$W/err.log")
  end=$(grep -a "\[script\] .*exit\|script exit" "$W/err.log" | tail -1 | cut -c1-80)
  echo "gxcensus $name ($frames frames): $(tail -1 "$W/err.log") ${line:-NO CENSUS LINE} ${end:+| $end}"
  case "$line" in
    *"census: 0 game-thread records"*) ;;
    "") echo "FAIL gxcensus $name: no census line (no producer hook in the executable, or the run did not end cleanly)"; fail=1 ;;
    *)
      base=$(grep -a "load base of the executable" "$W/err.log" | sed 's/.*base of the executable \(0x[0-9a-f]*\).*/\1/' | head -1)
      grep -a "census site" "$W/err.log" | head -20 | while read -r l; do
        hits=$(echo "$l" | sed 's/.*hits \([0-9]*\).*/\1/'); pcs=$(echo "$l" | sed 's/.* pcs //')
        echo "  site: $hits hits"
        n=0; for pc in $pcs; do
          [ $n -ge 7 ] && break; n=$((n+1)); [ $n -le 1 ] && continue   # (the first frame is the hook itself)
          if command -v addr2line >/dev/null && [ -n "$base" ]; then
            echo "    $(addr2line -f -C -e "$GXCENSUS_EXE" "$(printf '0x%x' $((0x$pc - base)))" | head -1)"
          else echo "    $pc"; fi
        done
      done
      echo "FAIL gxcensus $name: the game thread records while a send job is pending"; fail=1 ;;
  esac
  rm -rf "$W"
 done
done
if [ -n "$STRESS" ]; then what=gxstress; else what=gxcensus; fi
[ $fail = 0 ] && echo "PASS $what"
exit $fail
