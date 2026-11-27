#!/bin/bash
# MSC_DYNRES_UNDO on the host: the dynamic resolution's check of a step down in an ordinary scene (the Galaxy port's lesson: a smaller picture that buys no
# GPU time was kept for good). The GPU time is simulated (MSC_DYNRES_FAKE=a,b: a + b x the level's relative cost), two scripted runs of the same menu and
# kick-off part of nav_match60 each:
#   flat       a GPU time that does not follow the picture size (FAKE=70,0: always over the budget): with the check off the ladder goes down to the
#              scene's floor and stays; with it on the first step is judged on its frames, found useless and undone, and no step is taken below it again
#              within the block time
#   follows    a GPU time that follows the pixels (FAKE=2,100): the steps are kept; the check on and off end at the same level
# Passes when the verdicts are as above.
#   BIN=<np-host32-full-gl executable> SCRIPT=<nav_match60.txt> NP_DISC_DIR=<game data> [OUTROOT=<dir>] tools/render/run_dynres_undo_test.sh
# Skips (exit 0, says why) without the GL build or the game data. Four runs of about a minute: run it through your build lock (heavy.sh).
cd "$(dirname "$0")/../.."
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then echo "dynres undo test: skipped (set BIN to the host32 GL executable, np-host32-full-gl)"; exit 0; fi
RUN=tools/rt/run_h32.sh
if [ -z "$SCRIPT" ] || [ ! -f "$SCRIPT" ] || [ ! -d "$NP_DISC_DIR" ]; then echo "dynres undo test: skipped (set SCRIPT to nav_match60.txt and NP_DISC_DIR to the game data)"; exit 0; fi
export OUTROOT=${OUTROOT:-${TMPDIR:-/tmp}/np-dynres-undo-test} SCRIPT
FRAMES=${FRAMES:-2000}
COMMON="MSC_DYNRES_MAX=1.0 MSC_AI_BUDGET=off MSC_VGX_MSAA=0"
for scene in flat follows; do
    fake=70,0; [ $scene = follows ] && fake=2,100
    for undo in 0 1; do
        EXTRA_ENV="$COMMON MSC_DYNRES_FAKE=$fake MSC_DYNRES_UNDO=$undo" bash $RUN "$BIN" dynundo-$scene-$undo $FRAMES > /dev/null
    done
done
ok=1
lastlevel() { grep -o "level [0-9]* -> [0-9]* (" "$1/log.txt" | tail -1 | sed -E 's/.*-> ([0-9]+) .*/\1/'; }
count() { grep -c "$2" "$1/log.txt"; }
for scene in flat follows; do for undo in 0 1; do
    d=$OUTROOT/dynundo-$scene-$undo
    echo "$scene undo=$undo: final level $(lastlevel $d), $(count $d 'dynamic resolution: frame') changes, $(count $d 'no gain, undone') undone, $(count $d ', kept') kept"
done; done
f0=$OUTROOT/dynundo-flat-0; f1=$OUTROOT/dynundo-flat-1; p0=$OUTROOT/dynundo-follows-0; p1=$OUTROOT/dynundo-follows-1
if [ "$(count $f0 'no gain, undone')" != 0 ]; then echo "FAIL: the check judged a step with MSC_DYNRES_UNDO=0"; ok=0; fi
if [ "$(count $f1 'no gain, undone')" -lt 1 ]; then echo "FAIL: a flat GPU time: no step was undone"; ok=0; fi
if [ -z "$(lastlevel $f0)" ] || [ "$(lastlevel $f1)" -ge "$(lastlevel $f0)" ]; then echo "FAIL: with the check on, the flat scene did not end at a better level than with it off"; ok=0; fi
if [ "$(count $p1 'no gain, undone')" != 0 ]; then echo "FAIL: a GPU time that follows the pixels: a step was undone"; ok=0; fi
if [ "$(lastlevel $p0)" != "$(lastlevel $p1)" ]; then echo "FAIL: a GPU time that follows the pixels: a different final level with the check on"; ok=0; fi
[ $ok = 1 ] && echo "dynres undo test: passed" || exit 1
