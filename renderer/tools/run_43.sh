#!/bin/bash
# tools/run_43.sh <nav-script> <dump-dir> <max-frames>: the PC GL build with the game's
# widescreen setting OFF (Config.toml [video] widescreen = false), which tools/run_nav.sh
# cannot do (it writes its own Config.toml). For looking at the 4:3 mode the Vita host shows in
# a 725x544 box with side bars. Only for runs under 30 seconds (no heavy lock taken):
#   MSC_VGX_LEAD=2 MSC_DUMP_LIST=1300,1604,1900 tools/run_43.sh scripts/prof_open_play.txt <dir> 1902
set -e
MSC=/home/claude/msc; SCRIPT=$(realpath ${1:?nav script}); DIR=${2:?dump dir}; FRAMES=${3:?max frames}
mkdir -p "$DIR"; DIR=$(realpath "$DIR")
RUN=$(mktemp -d $MSC/build/host-gl/runs/run43.XXXXXX); trap 'rm -rf "$RUN"' EXIT
BIN=$RUN/$(basename ${VGX_BIN:-$MSC/build/host-gl/msc_gl_hle}); cp ${VGX_BIN:-$MSC/build/host-gl/msc_gl_hle} "$BIN"
mkdir -p "$RUN/MSCRecomp"; cp -a $MSC/build/nav/nand/saved "$RUN/MSCRecomp/NAND"
printf '[video]\nframe_rate = 60\nwidescreen = false\n[paths]\ndvd_root = "/home/claude/msc/game"\n' > "$RUN/MSCRecomp/Config.toml"
export XDG_DATA_HOME=$RUN MSC_INPUT_SCRIPT=$SCRIPT MSC_WATCH=1 MSC_WATCH_EVERY=60 MSC_FRAME_LOG=600 MSC_MAX_FRAMES=$FRAMES MSC_VIRTUAL_TIME=1 MSC_DUMP_DIR=$DIR
cd $MSC/wt/gx
timeout -k 5 120 nice -n 10 "$BIN" 2>&1
