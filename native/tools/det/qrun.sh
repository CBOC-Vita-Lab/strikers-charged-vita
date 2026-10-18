#!/bin/bash
# One armhf run under qemu-arm (no plugin), with a dummy environment variable of N bytes, digests in the log.
#   heavy.sh -t 600 tools/det/qrun.sh -x exe -s script -f frames -H hashEvery -n name [-p padBytes] [-T seconds] [-k dataDirToCopy (e.g. one with a save file)] [VAR=val ...]
# Log: build/np/B3/<name>.log (stderr+stdout of the game). Private data dir, recreated (no save file).
B=/home/claude/msc/build/np
EXE=$B/armhf/np-armhf-full; SCRIPT=/home/claude/msc/tools/scripts/nav_kickoff.txt; FR=4500; HE=30; NAME=q; PAD=0; TMO=550
KEEP=; while getopts "x:s:f:H:n:p:T:k:" o; do case $o in k) KEEP=$OPTARG;; x) EXE=$OPTARG;; s) SCRIPT=$OPTARG;; f) FR=$OPTARG;; H) HE=$OPTARG;; n) NAME=$OPTARG;; p) PAD=$OPTARG;; T) TMO=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
D=$B/B3/data-$NAME; rm -rf $D; mkdir -p $D; [ -n "$KEEP" ] && cp -a $KEEP/. $D/
for kv in "$@"; do export "$kv"; done
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D
export MSC_INPUT_SCRIPT=$(realpath $SCRIPT) MSC_STATE_HASH=$HE MSC_MAX_FRAMES=$FR MSC_VIRTUAL_TIME=1 MSC_FRAME_LOG=100000 MSC_WATCH_EVERY=100000
[ "$PAD" != 0 ] && export NPB3PAD=$(head -c $PAD /dev/zero | tr '\0' 'x')
cd $D
T0=$(date +%s)
timeout -k 3 $TMO /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf $EXE > $B/B3/$NAME.log 2>&1
echo "rc=$? wall=$(( $(date +%s) - T0 )) s hashes=$(grep -ac '^\[hash\]' $B/B3/$NAME.log)" | tee -a $B/B3/$NAME.log
rm -rf $D
