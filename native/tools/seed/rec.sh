#!/bin/bash
# W5-SEED: one scripted host32 run that records its shader keys (MSC_VGX_SHADERLIST) into build/np/W5SEED/rec/<name>.shl
#   heavy.sh -t 300 tools/seed/rec.sh [-n name] [-s script] [-f maxframes] [-X exe] [-b buildroot] [-S savedir] [-k]  [VAR=value ...]
# The run starts from a private data dir (no save file, or a copy of -S <dir>), virtual time, the exe's own output in rec/<name>.log.
# Keys are produced by the front end, so the null back end is enough (default exe: the W5INT null build).
OUT=${SEED_OUT:-/home/claude/msc/build/np/W5SEED}
EXE=/home/claude/msc/build/np/W5INT/host32/np-host32-full-null
NAME=run; SCRIPT=; FR=; SAVE=; KEEP=0; TMO=280
while getopts "n:s:f:X:S:kt:" o; do case $o in n) NAME=$OPTARG;; s) SCRIPT=$OPTARG;; f) FR=$OPTARG;; X) EXE=$OPTARG;; S) SAVE=$OPTARG;; k) KEEP=1;; t) TMO=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
mkdir -p $OUT/rec $OUT/data
D=$OUT/data/$NAME
[ $KEEP = 1 ] || rm -rf $D
mkdir -p $D
[ -n "$SAVE" ] && [ ! -e $D/NAND ] && cp -a $SAVE/. $D/
SCRIPT=$(realpath $SCRIPT)
LOG=$OUT/rec/$NAME.log
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$SCRIPT MSC_VIRTUAL_TIME=1 MSC_WATCH=1 MSC_WATCH_EVERY=${WATCH_EVERY:-600} MSC_FRAME_LOG=100000
export MSC_VGX_SHADERLIST=$OUT/rec/$NAME.shl
[ -n "$FR" ] && export MSC_MAX_FRAMES=$FR
for kv in "$@"; do export "$kv"; done
echo "== rec $NAME $(date +%T) script=$SCRIPT exe=$EXE" > $LOG
T0=$(date +%s)
( cd $D && timeout -k 5 $TMO $EXE >> $LOG 2>&1; echo "rc=$?" >> $LOG )
echo "rec $NAME: $(tail -1 $LOG) $(( $(date +%s) - T0 )) s, list $(stat -c %s $MSC_VGX_SHADERLIST 2>/dev/null) bytes; $(grep -a -c '^\[vgx\] .*shader .* generated' $LOG) shaders generated"
