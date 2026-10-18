#!/bin/bash
# Determinism matrix: the same executable, script and virtual clock under different process layouts / garbage; per-step traces compared.
#   heavy.sh -t 590 tools/det/matrix.sh [-x exe] [-s script] [-f frames] [-t tag] [-q (armhf under qemu)] [-c "cfg1 cfg2 ..."] [-F (fresh data dir: the first-run popup path) | -k dir]
# By default the data dir is a copy of build/np/B3/rundata-save (a save file exists: no first-run popup).
# configs (names): base pad4k pad1 sp00 spAA hp00 hpA5 hp5A noaslr perturb onecore load  (default: base pad4k sp00 spAA hp00 hpA5 noaslr perturb onecore load)
#   base   nothing        pad4k/pad1  environment 4096 / 1 bytes bigger     sp00/spAA  NP_STACK_POISON=0x00/0xAA  hp00/hpA5/hp5A  NP_HEAP_POISON
#   nan    NP_DET_NAN=1 and [watch] lines every 60 frames (ball=, score=): when does a position go non-finite?
#   flip   NP_HEAP_FLIP=1: every game-heap allocation comes from the other end of its heap (reverses the address order of most objects)
#   arena  both game arenas mapped elsewhere (MEM2 below MEM1; both stay inside 0x80000000-0x9FFFFFFF: the AX mixer rebuilds sample pointers as (addr & 0x1FFFFFFF) | 0x80000000: NP_MEM1_BASE=0x8A000000 NP_MEM2_BASE=0x84000000)
#   noaslr setarch -R (no address-space randomisation; qemu is not affected)  perturb  MALLOC_PERTURB_=165 (host malloc blocks: the non-game threads)
#   nppflags NPP's run flags (MSC_TASK_MARKS=1 MSC_QEMU_MARK=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=30 MSC_DUMP_MAPS=1): the layout dependence of wave2-NPP 7.1 showed up with them
#   padN    (N a number) an environment N bytes bigger, e.g. pad12 pad16
#   onecore taskset -c 0 (all threads on one cpu: different thread timing)   load  two busy loops running next to the game (thread timing again)
# Traces: build/np/B3/mx/<tag>-<cfg>.trace, logs <tag>-<cfg>.log; at the end tools/det/stepcmp.py base vs all. Exit 0 = all identical.
B=/home/claude/msc/build/np; NP=/home/claude/msc/np
EXE=$B/B3x/host32/np-host32-full-null; SCRIPT=/home/claude/msc/tools/scripts/nav_match60.txt; FR=4400; TAG=mx; Q=0
KEEP=$B/B3/rundata-save   # a data dir WITH a save file: the path every real run takes except the first (the fresh-data path hid the layout dependence of wave2-NPP 7.1)
CFGS="base pad4k sp00 spAA hpA5 arena noaslr perturb onecore load"
BUSY=""
while getopts "x:s:f:t:qc:Fk:" o; do case $o in F) KEEP=;; k) KEEP=$OPTARG;; x) EXE=$OPTARG;; s) SCRIPT=$OPTARG;; f) FR=$OPTARG;; t) TAG=$OPTARG;; q) Q=1;; c) CFGS=$OPTARG;; *) exit 2;; esac; done
OUT=$B/B3/mx; mkdir -p $OUT
ALL=""
for c in $CFGS; do
  D=$OUT/data-$TAG-$c; rm -rf $D; mkdir -p $D; [ -n "$KEEP" ] && cp -a $KEEP/. $D/
  T=$OUT/$TAG-$c.trace; L=$OUT/$TAG-$c.log; PRE=""
  (
    cd $D
    export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_MAX_FRAMES=$FR MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 MSC_WATCH_EVERY=100000 MSC_STATE_HASH=30 NP_DET_TRACE=$T
    case $c in
      pad4k) export NPB3PAD=$(head -c 4096 /dev/zero | tr '\0' 'x');;
      pad1) export NPB3PAD=x;;
      pad[0-9]*) export NPB3PAD=$(head -c ${c#pad} /dev/zero | tr '\0' 'x');;
      nppflags) export MSC_TASK_MARKS=1 MSC_QEMU_MARK=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=30 MSC_DUMP_MAPS=1;;
      nppflags+pad*) export MSC_TASK_MARKS=1 MSC_QEMU_MARK=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=30 MSC_DUMP_MAPS=1 NPB3PAD=$(head -c ${c#nppflags+pad} /dev/zero | tr '\0' 'x');;
      flip) export NP_HEAP_FLIP=1;;
      nan) export NP_DET_NAN=1 MSC_WATCH=1 MSC_WATCH_EVERY=60;;
      snap) export NP_DET_SNAP=${SNAP:-4140-4150:0:4};;
      sp00) export NP_STACK_POISON=0;; spAA) export NP_STACK_POISON=0xAA;;
      hp00) export NP_HEAP_POISON=0;; hpA5) export NP_HEAP_POISON=0xA5;; hp5A) export NP_HEAP_POISON=0x5A;;
      arena) export NP_MEM1_BASE=0x8A000000 NP_MEM2_BASE=0x84000000;;
      noaslr) PRE="setarch -R";;
      perturb) export MALLOC_PERTURB_=165;;
      onecore) PRE="taskset -c 0";;
      load) ( while :; do :; done ) & echo $! > $D/b1; ( while :; do :; done ) & echo $! > $D/b2;;
    esac
    if [ $Q = 1 ]; then timeout -k 3 500 $PRE /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf $EXE > $L 2>&1
    else timeout -k 3 300 $PRE $EXE > $L 2>&1; fi
    echo "rc=$?" >> $L
  )
  for b in $D/b1 $D/b2; do [ -f $b ] && kill $(cat $b) 2>/dev/null; done
  rm -rf $D
  echo "$c: $(tail -1 $L) steps=$(wc -l < $T) hashlines=$(grep -ac '^\[hash\]' $L)"
  ALL="$ALL $T"
done
python3 -I $NP/tools/det/stepcmp.py $ALL; RC=$?
[ -n "$KEEP" ] || { gzip -f $ALL; echo "(traces gzipped; KEEP=1 keeps them plain)"; }
exit $RC
