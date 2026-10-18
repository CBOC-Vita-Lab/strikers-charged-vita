#!/bin/bash
# SKIN (W5): run the armhf executable under qemu-arm with the per-thread instruction-count plug-in (after tools/gxd/run.sh). One heavy turn.
#   tools/heavy.sh -t 3000 tools/skin/qrun.sh <exe> <tag> <frames> [VAR=value ...]
# Output build/np/W5SKIN/qruns/<tag>/{run.log,err.log}; per-scene per-thread numbers: python3 tools/gxd/threads.py <err.log>.
# The renderer's instrumentation is off (no hash, no census: neither exists on the console). NAV=<script> (default tools/nav/survey_full.txt).
EXE=${1:?exe}; TAG=${2:?tag}; FR=${3:?frames}; shift 3
B=/home/claude/msc/build/np/W5SKIN
D=$B/qruns/$TAG; rm -rf $D; mkdir -p $D/data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
for kv in "$@"; do export "$kv"; done
export MSC_STATE_HASH=${HASHEVERY:-120} MSC_MAX_FRAMES=$FR MSC_QEMU_MARK=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=${WATCH_EVERY:-30} MSC_DUMP_MAPS=1
export MSC_INPUT_SCRIPT=${NAV:-/home/claude/msc/npw/W5SKIN/tools/nav/survey_full.txt} MSC_VIRTUAL_TIME=1 MSC_FRAMESEND=${FS:-nofork} MSC_FS_MARKS=1
export SKIN_PAD=
L0=$(( $(env | wc -c) + ${#EXE} )); NPAD=$(( ${PADTO:-60000} - L0 - 9 ))
[ $NPAD -gt 0 ] || { echo "qrun.sh: environment already $L0 bytes" >&2; exit 2; }
export SKIN_PAD=$(head -c $NPAD /dev/zero | tr '\0' 'x')
echo "== qrun $TAG exe=$EXE frames=$FR $* $(date +%T)" | tee $D/cmd.txt
cd $D
${TMO:+timeout -k 3 $TMO} /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf \
  -plugin /home/claude/msc/build/np/W5GXD/libframecount2.so -d plugin $EXE > $D/run.log 2> $D/err.log
echo "rc=$? $(date +%T)" | tee -a $D/cmd.txt
grep -ac "insn\] mark" $D/err.log | sed 's/^/marks: /'
rm -rf $D/data
