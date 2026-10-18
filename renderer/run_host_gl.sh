#!/bin/bash
# run_host_gl.sh <frames> [input-script] [dump-every] [dump-dir]
# Runs the GL harness. Uses a private copy of the runtime data directory so
# parallel runs of other builds are not disturbed.
FR=${1:-600}; SCRIPT=${2:-}; EVERY=${3:-0}; DIR=${4:-/home/claude/msc/build/host-gl/frames}
cd /home/claude/msc/wt/gx || exit 1
export XDG_DATA_HOME=/home/claude/msc/build/host-gl/appdata
export MSC_MAX_FRAMES=$FR MSC_FRAME_LOG=${MSC_FRAME_LOG:-300}
[ -n "$SCRIPT" ] && export MSC_INPUT_SCRIPT=$SCRIPT
[ "$EVERY" != "0" ] && export MSC_DUMP_FRAMES=$EVERY
export MSC_DUMP_DIR=$DIR
exec nice -n 10 /home/claude/msc/build/host-gl/msc_gl
