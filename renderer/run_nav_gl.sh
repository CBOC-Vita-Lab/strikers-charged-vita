#!/bin/bash
# run_nav_gl.sh <nav-script> <dump-dir> [dump-every] [max-frames]
# Runs the GL harness (build/host-gl/msc_gl) under the navigation harness with
# the deterministic virtual clock, from a private copy of the saved NAND.
# Takes the shared heavy-job lock (tools/heavy.sh). Extra variables pass through:
#   MSC_DUMP_LIST=a,b-c  MSC_RENDER_SIZE=WxH  MSC_VGX_TRACE=<frame>  MSC_VGX_LOG=1 ...
#   VGX_BIN=<executable> (default build/host-gl/msc_gl)   VGX_REAL=1 (real clock)
#   VGX_NOLOCK=1 (no heavy lock: null-back-end runs, or GL runs known to take well under a minute)
#   VGX_RATE=<30|60> (the game's frame_rate setting, default 60)
#   VGX_WATCH_EVERY=<n> ([watch] line period)
set -e
MSC=/home/claude/msc
SCRIPT=${1:?nav script}; DIR=${2:?dump dir}; EVERY=${3:-0}; FRAMES=${4:-0}
mkdir -p "$DIR"
SCRIPT=$(realpath "$SCRIPT"); DIR=$(realpath "$DIR")
RUNS=${VGX_RUNS:-$MSC/build/host-gl/runs}; mkdir -p "$RUNS"
RUN=$(mktemp -d $RUNS/run.XXXXXX)
trap 'rm -rf "$RUN"' EXIT
# The executable is copied now, so a rebuild while this run waits for the lock (or runs) cannot
# change what is being tested.
BIN=$RUN/$(basename ${VGX_BIN:-$MSC/build/host-gl/msc_gl})
cp ${VGX_BIN:-$MSC/build/host-gl/msc_gl} "$BIN"
export MSC_DUMP_DIR=$DIR
[ "$EVERY" != 0 ] && export MSC_DUMP_FRAMES=$EVERY
OPTS=""
[ -n "$VGX_REAL" ] && OPTS="-r"
# VGX_RATE=30: Config.toml frame_rate (default 60). With MSC_VIRTUAL_FRAME_US=40000 on top the
# game presents every fourth retrace (15 frames per second): workstream FI.
[ -n "$VGX_RATE" ] && OPTS="$OPTS -f $VGX_RATE"
# VGX_WATCH_EVERY=<n>: a [watch] line every n frames (default 60; 1 gives game state per frame: buttons, ball, score).
[ -n "$VGX_WATCH_EVERY" ] && OPTS="$OPTS -e $VGX_WATCH_EVERY"
# VGX_NOLOCK=1: for runs known to take well under a minute (MSC_VGX_LEAD fast-forward).
LOCK="$MSC/tools/heavy.sh"
[ -n "$VGX_NOLOCK" ] && LOCK=""
PORT=$MSC/wt/gx MSC_HOST_BIN=$BIN \
  $LOCK $MSC/tools/run_nav.sh $OPTS -d "$RUN" "$SCRIPT" "$FRAMES"
