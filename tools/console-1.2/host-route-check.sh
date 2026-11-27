#!/bin/bash
# Run a kit route on the desktop test harness (virtual clock, no console) to check that it still navigates: every "@begin"/"@end" mark
# appears, the goal is scored, the script ends with "script exit". Use it after a menu or script change, never as a performance figure.
#
#   NP_HOST_BIN=<np-host32-full-gl> NP_DISC_DIR=<extracted disc> tools/console-1.2/host-route-check.sh <route.txt> [max frames] [NAME=VALUE ...]
#
# The data folder is a new empty one each time, so the "create save data" pop-up of a fresh private root is exercised too.
# Wrap it in the machine's lock for heavy jobs if there is one (HEAVY=<command>), for example HEAVY=./heavy.sh.
set -u
ROUTE=${1:?usage: host-route-check.sh <route.txt> [max frames] [NAME=VALUE ...]}
FRAMES=${2:-12000}
shift 2 2>/dev/null || shift $#
BIN=${NP_HOST_BIN:?set NP_HOST_BIN to the host harness executable}
DISC=${NP_DISC_DIR:?set NP_DISC_DIR to the extracted disc}
OUT=${OUT:-$(mktemp -d)}
mkdir -p "$OUT/data"
ROUTE=$(cd "$(dirname "$ROUTE")" && pwd)/$(basename "$ROUTE")
cd "$OUT/data" || exit 1
export NP_DISC_DIR=$DISC NP_DATA_DIR=$OUT/data MSC_INPUT_SCRIPT=$ROUTE MSC_MAX_FRAMES=$FRAMES MSC_VIRTUAL_TIME=1 \
  MSC_FRAME_LOG=100000 MSC_FRAME_TIMING=300 MSC_WATCH=1 MSC_WATCH_EVERY=600 \
  EGL_PLATFORM=surfaceless GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=2 MSC_VGX_LEAD=0 MSC_FRAMESEND=nofork
# shellcheck disable=SC2086
${HEAVY:-} env "$@" timeout -k 3 "${TMO:-1500}" "$BIN" > "$OUT/log.txt" 2>&1
echo "exit code $?  log $OUT/log.txt"
grep -a "\[script\]" "$OUT/log.txt" | grep -a -E "@begin|@end|TIMEOUT|STATE|stopwhen|score|exit" | cut -c1-140
grep -a "\[plat\]" "$OUT/log.txt" | tail -2
