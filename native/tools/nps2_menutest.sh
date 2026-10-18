#!/bin/bash
# NPS2: unit test of the menu navigation against REAL game objects (platform/sdk/inputtest/test_menu_nav.cpp).
# Needs the object tree of tools/nps2_smoke.sh (build/np/NPS2x/host32). heavy.sh -t 300 tools/nps2_menutest.sh
cd "$(dirname "$0")/.."
ROOT=/home/claude/msc/build/np/NPS2x
PD="platform/compat platform/assets platform/sdk/net platform/sdk/input platform/sdk/save platform/sdk/inputtest"
make TARGET=host32 OPT=-O1 DEBUG=-g0 BUILD_ROOT=$ROOT PLAT_DIRS="$PD" $ROOT/host32/pobj/platform/sdk/inputtest/test_menu_nav.cpp.o $ROOT/host32/pobj/platform/sdk/input/np_menu_nav.cpp.o $ROOT/host32/pobj/platform/sdk/input/np_touch.cpp.o $ROOT/host32/pobj/platform/sdk/input/np_game_input.cpp.o 2>&1 | grep -E "error|Error" -A3 | head -30
T=$ROOT/host32/pobj/platform/sdk/inputtest/test_menu_nav.cpp.o
[ -f $T ] || { echo "no test object"; exit 1; }
# the objects of the last -sp build, without the game's own main and the harness' main; the freshly built glue objects
OBJS=$(find $ROOT/host32/obj -name '*.o')
POBJ=$(find $ROOT/host32/pobj -name '*.o' | grep -v 'np_host_main\|inputtest')
clang++ -m32 -o $ROOT/host32/test_menu_nav $T $OBJS $POBJ $ROOT/host32/sdk/libnpsdk.a $ROOT/host32/libnpstubs.a -rdynamic -lpthread -lm -ldl 2> $ROOT/host32/test_menu_nav.link.err || { grep -c "undefined reference" $ROOT/host32/test_menu_nav.link.err; head -5 $ROOT/host32/test_menu_nav.link.err; exit 1; }
cd $ROOT && NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$ROOT/menutest-data timeout 60 $ROOT/host32/test_menu_nav > $ROOT/menutest.log 2>&1
echo "test_menu_nav rc=$?"; grep -v '^NP STUB\|^\[game\]\|^\[os\]' $ROOT/menutest.log | tail -${TAILN:-40}
[ "$NOMENUNAV" = 1 ] || { echo "=== menunav $(date +%T)"; /home/claude/msc/npw/NPS2/tools/nps2_menunav.sh; }
