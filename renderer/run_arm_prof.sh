#!/bin/bash
# run_arm_prof.sh <arm-executable> <tag> [nav-script]
# Runs an armhf build under qemu-arm with the navigation harness (virtual clock), a mark per
# presented frame and a block profile of the script's "prof begin/end" window. Takes the shared
# heavy-job lock (tools/heavy.sh: yields to the emulator workstream's priority turns). Extra
# variables pass through (MSC_VGX_HASH=1 MSC_DUMP_DIR=<dir> for a frame hash). Output: build/host-gl-arm/runs/<tag>.log, <tag>.prof
#   afterwards: tools/prof_split.py <tag>   (or /home/claude/msc/tools/prof_report.py)
set -e
MSC=/home/claude/msc
BIN=$(realpath "${1:?arm executable}"); TAG=${2:?tag}; SCRIPT=$(realpath "${3:-$MSC/vgx/scripts/prof_open_play.txt}")
OUT=$MSC/build/host-gl-arm/runs
mkdir -p "$OUT"
RUN=$(mktemp -d $OUT/run.XXXXXX)
trap 'rm -rf "$RUN"' EXIT
# Private copy of the executable: later rebuilds do not change what a queued run measures.
[ "$BIN" -ef "$OUT/$TAG.bin" ] || cp "$BIN" "$OUT/$TAG.bin"
rm -f "$OUT/$TAG.prof"
# PROF_LOCK_BY=<epoch seconds>: give the lock back without running if it is granted later than
# that (for callers that must finish inside a fixed time: a run that would be cut off wastes
# the turn for everybody). Exit status 75 then.
if [ -n "$PROF_LOCK_BY" ] ; then
  GUARD="if [ \$(date +%s) -gt $PROF_LOCK_BY ]; then echo 'lock granted too late, not run'; exit 75; fi; "
else
  GUARD=""
fi
PORT=$MSC/wt/gx MSC_ARM_BIN=$OUT/$TAG.bin MSC_FRAME_TRACE=1 NAV_TIMEOUT=${NAV_TIMEOUT:-3600} \
  $MSC/tools/heavy.sh bash -c "${GUARD}exec $MSC/tools/run_nav.sh -a -m 1 -p '$OUT/$TAG.prof' -d '$RUN' '$SCRIPT'" > "$OUT/$TAG.log" 2>&1
