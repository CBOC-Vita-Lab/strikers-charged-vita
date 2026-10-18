#!/bin/bash
# NPS2 smoke: builds the full host32 variant with the null renderer (EXESUF=-full-null) of THIS tree and runs a nav script
#   (a) in real time, (b) twice in virtual time (MSC_VIRTUAL_TIME=1); the two virtual runs must give the same [nav] and [hash] lines.
#   heavy.sh -t 900 tools/nps2_smoke.sh [script] [frames]
cd "$(dirname "$0")/.."
ROOT=/home/claude/msc/build/np/NPS2x
S=$(readlink -f ${1:-tools/nav/boot_title.txt}); FR=${2:-1500}
[ "$NOBUILD" = 1 ] || make -j2 TARGET=host32 WITH_SDK=1 WITH_GX=1 GX_BACKEND=null WITH_PLAT=1 EXESUF=-full-null OPT=-O1 DEBUG=-g0 BUILD_ROOT=$ROOT 2>&1 | grep -E "^FAIL|error:|unresolved references" | head -20
EXE=$ROOT/host32/np-host32-full-null
[ -x $EXE ] || { echo "no exe"; exit 1; }
export NP_DISC_DIR=/home/claude/msc/game MSC_INPUT_SCRIPT=$S MSC_WATCH=1 MSC_WATCH_EVERY=60 MSC_STATE_HASH=60 MSC_MAX_FRAMES=$FR MSC_FRAME_LOG=0
run() { # name, wall-clock limit, extra env...
  n=$1; tmo=$2; shift 2
  rm -rf $ROOT/run-$n; mkdir -p $ROOT/run-$n
  ( cd $ROOT/run-$n && env NP_DATA_DIR=$ROOT/run-$n "$@" timeout -k 5 $tmo $EXE > $ROOT/run-$n.log 2>&1; echo "rc=$?" >> $ROOT/run-$n.log )
  echo "== $n: $(grep -c '\[nav\]' $ROOT/run-$n.log) nav lines, $(grep -c '\[hash\]' $ROOT/run-$n.log) hash lines, $(tail -1 $ROOT/run-$n.log)"
}
t0=$(date +%s); run real ${REAL_S:-150}; t1=$(date +%s)
run vt1 ${VT_S:-90} MSC_VIRTUAL_TIME=1; t2=$(date +%s)
run vt2 ${VT_S:-90} MSC_VIRTUAL_TIME=1; t3=$(date +%s)
echo "real $((t1-t0)) s, virtual $((t2-t1)) s and $((t3-t2)) s"
for n in vt1 vt2; do grep -E '^\[(nav|hash)\]|^\[script\]' $ROOT/run-$n.log | sed 's/ dt=[^ ]*//; s/ fps=[^ ]*//; s/ ms=[^ ]*//' > $ROOT/run-$n.det; done
if cmp -s $ROOT/run-vt1.det $ROOT/run-vt2.det; then echo "virtual runs IDENTICAL ($(wc -l < $ROOT/run-vt1.det) lines)"; else echo "virtual runs DIFFER"; diff $ROOT/run-vt1.det $ROOT/run-vt2.det | head -20; fi
