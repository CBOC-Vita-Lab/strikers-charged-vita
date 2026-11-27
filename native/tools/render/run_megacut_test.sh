#!/bin/bash
# The cut into and out of a Mega Strike on the host (MSC_MEGA_WARM): two scripted runs of the same match around the Mega Strike of nav_match60
# (frame 2994) with a simulated GPU time, so the dynamic resolution changes the picture size at the cuts: MSC_MEGA_WARM=0 and the default. Passes when
# every dumped picture is the same in both runs and the default makes no more render targets, pass objects and size changes at the cut than the 1.1 rules.
#   BIN=<np-host32-full-gl executable> SCRIPT=<nav_match60.txt> NP_DISC_DIR=<game data> [OUTROOT=<dir>] tools/render/run_megacut_test.sh
# Skips (exit 0, says why) without the GL build or the game data. Two runs of about a minute each: run it through your build lock (heavy.sh).
cd "$(dirname "$0")/../.."
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then echo "mega cut test: skipped (set BIN to the host32 GL executable, np-host32-full-gl)"; exit 0; fi
RUN=tools/rt/run_h32.sh
if [ -z "$SCRIPT" ] || [ ! -f "$SCRIPT" ] || [ ! -d "$NP_DISC_DIR" ]; then echo "mega cut test: skipped (set SCRIPT to nav_match60.txt and NP_DISC_DIR to the game data)"; exit 0; fi
export OUTROOT=${OUTROOT:-${TMPDIR:-/tmp}/np-megacut-test} SCRIPT
LIST=2990,2994,2998,3010,3024,3040,3100
COMMON="MSC_DYNRES_MAX=1.0 MSC_DYNRES_FAKE=2,34 MSC_MEGA_CUTLOG=1 MSC_AI_BUDGET=off"
KEEP=$LIST EXTRA_ENV="$COMMON MSC_MEGA_WARM=0" bash $RUN "$BIN" megacut-off 3102 $LIST > /dev/null
KEEP=$LIST EXTRA_ENV="$COMMON" bash $RUN "$BIN" megacut-on 3102 $LIST > /dev/null
ok=1
if ! diff <(cd $OUTROOT/megacut-off && sed 's/frame_//' pics.md5) <(cd $OUTROOT/megacut-on && sed 's/frame_//' pics.md5) > /dev/null; then echo "FAIL: pictures differ between MSC_MEGA_WARM=0 and the default"; ok=0; fi
n=$(grep -c . $OUTROOT/megacut-on/pics.md5)
if [ "$n" -lt 7 ]; then echo "FAIL: only $n pictures dumped"; ok=0; fi
# the sums of the two cuts: made targets and objects (fields after "efb made" and "pool objects made")
sum() { grep "^\[mega-cut\] sum" "$1/log.txt" | sed -E 's/.*efb made ([0-9]+).*pool objects made f([0-9]+) c([0-9]+) d([0-9]+).*/\1 \2 \3 \4/' | awk '{a+=$1; b+=$2; c+=$3; d+=$4} END {print a+0, b+0, c+0, d+0}'; }
off=$(sum $OUTROOT/megacut-off); on=$(sum $OUTROOT/megacut-on)
echo "targets, pass framebuffers, colour, depth made in the cut zones: off [$off]  on [$on]"
set -- $off; offT=$1; offF=$2; set -- $on; onT=$1; onF=$2
if [ "$onT" -gt "$offT" ] || [ "$onF" -gt "$offF" ]; then echo "FAIL: the default made more at the cuts than MSC_MEGA_WARM=0"; ok=0; fi
[ $ok = 1 ] && echo "mega cut test: passed ($n pictures equal)" || exit 1
