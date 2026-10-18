#!/bin/bash
# NPS2: the menu navigation script of the real game stalled after the first D-pad press in a virtual-time run (menunav.log, rc=124).
# Runs it again with gdb samples of the main thread (virtual time), then in real time.   heavy.sh -t 420 tools/nps2_menunav_diag.sh
cd "$(dirname "$0")/.."
ROOT=/home/claude/msc/build/np/NPS2x
S=tools/nav/nps2_menunav.txt
echo "== virtual time"; NOBUILD=1 VIRT=1 SAMPLE_DELAY=${VDELAY:-95} MAXF=3000 WATCH_EVERY=0 EXTRA_ENV="MSC_WATCH_BUTTONS=1" LOG=$ROOT/menunav-vt.log GDBLOG=$ROOT/menunav-vt.gdb tools/nps2_vthang.sh $S | tail -3
grep -E '^\[script\]|TIMEOUT' $ROOT/menunav-vt.log | tail -12
echo "== real time"; NOBUILD=1 VIRT=0 SAMPLE_DELAY=${RDELAY:-45} MAXF=3000 WATCH_EVERY=0 EXTRA_ENV="MSC_WATCH_BUTTONS=1" LOG=$ROOT/menunav-rt.log GDBLOG=$ROOT/menunav-rt.gdb tools/nps2_vthang.sh $S | tail -3
grep -E '^\[script\]|TIMEOUT' $ROOT/menunav-rt.log | tail -14
