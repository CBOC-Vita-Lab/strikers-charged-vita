#!/bin/bash
# NPS2: which voices take the AX mixer's reference path (ratio above 8.0 / unusable coefficient bank) in the front end?
# Builds the full-null exe of this tree, runs the menu navigation script in virtual time with NP_AX_LOG=1 MSC_AX_STATS=1 for ~1400 frames.
#   heavy.sh -t 420 tools/nps2_axlog.sh
cd "$(dirname "$0")/.."
ROOT=/home/claude/msc/build/np/NPS2x
make -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 GX_BACKEND=null WITH_PLAT=1 EXESUF=-full-null OPT=-O1 DEBUG=-g0 BUILD_ROOT=$ROOT 2>&1 | grep -E "^FAIL|error:|unresolved references" | head
EXE=$ROOT/host32/np-host32-full-null
D=$ROOT/axlog-data; rm -rf $D; mkdir -p $D
( cd $D && env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=/home/claude/msc/npw/NPS2/tools/nav/nps2_savetest.txt MSC_WATCH=1 MSC_WATCH_EVERY=0 MSC_FRAME_LOG=0 \
  MSC_MAX_FRAMES=${FR:-1300} MSC_VIRTUAL_TIME=1 NP_AX_LOG=1 MSC_AX_STATS=1 timeout -k 5 ${TMO:-300} $EXE > $ROOT/axlog.log 2>&1; echo "rc=$?" >> $ROOT/axlog.log )
grep -E "reference voice|^rc=|\[ax|ax:" $ROOT/axlog.log | cut -c1-260 | head -60
grep -n -i "AX stats" -A40 $ROOT/axlog.log | head -70 | cut -c1-220
