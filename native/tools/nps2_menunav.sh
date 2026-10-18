#!/bin/bash
# NPS2: menu navigation in the real game (host32 -full-null, virtual time). Needs the build of tools/nps2_smoke.sh.
#   heavy.sh -t 300 tools/nps2_menunav.sh
cd "$(dirname "$0")/.."
ROOT=/home/claude/msc/build/np/NPS2x
[ "$BUILD" = 1 ] && make -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 GX_BACKEND=null WITH_PLAT=1 EXESUF=-full-null OPT=-O1 DEBUG=-g0 BUILD_ROOT=$ROOT 2>&1 | grep -E "^FAIL|error:|unresolved references" | head
EXE=$ROOT/host32/np-host32-full-null
D=$ROOT/menunav${SUF}-data; rm -rf $D; mkdir -p $D
( cd $D && env NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=/home/claude/msc/npw/NPS2/tools/nav/nps2_menunav.txt \
  MSC_WATCH=1 MSC_WATCH_EVERY=0 MSC_WATCH_BUTTONS=1 MSC_FRAME_LOG=0 MSC_MAX_FRAMES=6000 MSC_VIRTUAL_TIME=${VT:-1} NP_AX_MIX=${NP_AX_MIX:-1} timeout -k 5 200 $EXE > $ROOT/menunav${SUF}.log 2>&1; echo "rc=$?" >> $ROOT/menunav${SUF}.log )
grep -E '^\[script\]|^\[nav\] f=[0-9]+ (pointer|buttons)|now on|^ +ctx|^rc=|TIMEOUT' $ROOT/menunav${SUF}.log | cut -c1-200 | tail -${TAILN:-80}
