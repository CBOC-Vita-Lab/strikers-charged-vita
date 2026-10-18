#!/bin/bash
# Layout independence over many matches: for each seed (MSC_VIRTUAL_TICK_NS = 1000 + 37 * seed changes the clock the game seeds its simulation from, so every seed is a
# different match), run the same executable under three process layouts and compare the per-step digests.
#   heavy.sh -t 900 tools/det/seeds.sh [-x exe] [-q] [-f frames] [-s script] [-t tag] first last
#   layouts:  A = plain;  B = environment +4096, arenas elsewhere (bit 31 kept), stack poison 0xAA, heap poison 0xA5;  C = environment +7, stack poison 0x00, heap poison 0x00
# The data dir starts with a save file (build/np/B3/rundata-save). Prints one line per seed; exit 0 when every seed is identical in all three.
B=/home/claude/msc/build/np; NP=/home/claude/msc/np
EXE=$B/B3x/host32/np-host32-full-null; FR=4400; SCRIPT=/home/claude/msc/tools/scripts/nav_match60.txt; TAG=sd; Q=0
while getopts "x:qf:s:t:" o; do case $o in x) EXE=$OPTARG;; q) Q=1;; f) FR=$OPTARG;; s) SCRIPT=$OPTARG;; t) TAG=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1)); A=${1:?first}; Z=${2:?last}
OUT=$B/B3/sd; mkdir -p $OUT; BAD=0
runone() {   # runone <seed> <layout> <extra env...>
  local seed=$1 lay=$2; shift 2
  local D=$OUT/data-$TAG-$seed-$lay T=$OUT/$TAG-$seed-$lay.trace
  rm -rf $D; mkdir -p $D; cp -a $B/B3/rundata-save/. $D/
  ( cd $D; export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_MAX_FRAMES=$FR MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_TICK_NS=$((1000 + 37 * seed)) \
      MSC_FRAME_LOG=100000 MSC_WATCH_EVERY=100000 NP_DET_TRACE=$T NP_DET_NAN=1 "$@"
    if [ $Q = 1 ]; then timeout -k 3 400 /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf $EXE > $OUT/$TAG-$seed-$lay.log 2>&1
    else timeout -k 3 200 $EXE > $OUT/$TAG-$seed-$lay.log 2>&1; fi
    echo "rc=$?" >> $OUT/$TAG-$seed-$lay.log )
  rm -rf $D
}
for s in $(seq $A $Z); do
  PAD4K=$(head -c 4096 /dev/zero | tr '\0' 'x'); PAD7=$(head -c 7 /dev/zero | tr '\0' 'x')
  runone $s A
  runone $s B NPB3PAD=$PAD4K NP_MEM1_BASE=0x8A000000 NP_MEM2_BASE=0x84000000 NP_STACK_POISON=0xAA NP_HEAP_POISON=0xA5
  runone $s C NPB3PAD=$PAD7 NP_STACK_POISON=0x00 NP_HEAP_POISON=0x00
  r=$(python3 -I $NP/tools/det/stepcmp.py $OUT/$TAG-$s-A.trace $OUT/$TAG-$s-B.trace $OUT/$TAG-$s-C.trace 2>&1)
  rc=$?
  steps=$(wc -l < $OUT/$TAG-$s-A.trace); score=$(grep -ao 'score=[0-9]*-[0-9]*' $OUT/$TAG-$s-A.log | tail -1)
  nan=$(grep -ah "^\[det\]" $OUT/$TAG-$s-A.log | head -2 | tr "\n" "|")
  if [ $rc = 0 ]; then echo "seed $s: IDENTICAL ${nan:+NONFINITE: $nan} ($steps steps; rcs: $(tail -qn1 $OUT/$TAG-$s-A.log $OUT/$TAG-$s-B.log $OUT/$TAG-$s-C.log | tr '\n' ' '))"
  else BAD=$((BAD+1)); echo "seed $s: DIFFERENT ($steps steps)"; echo "$r" | grep -A3 "first difference\|TOO FEW" | head -6; fi
  [ $rc = 0 ] && gzip -f $OUT/$TAG-$s-?.trace
done
echo "seeds $A..$Z: $BAD differing"; exit $([ $BAD = 0 ] && echo 0 || echo 1)
