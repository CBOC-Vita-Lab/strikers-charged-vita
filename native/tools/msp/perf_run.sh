#!/bin/bash
# MSP: run an armhf native build under qemu-arm with the instruction-count plugin (NPP2's method, tools/perf/run.sh, parametrized for the MSP tree).
# ONE job per call; always via heavy.sh:
#   tools/heavy.sh -t 500 tools/msp/perf_run.sh <tag> <frames> [VAR=value ...]
# Output: $B/runs/<tag>/{run.log,err.log}. EXE overrides the executable (default $B/armhf/np-armhf-full). TMO=<seconds> stops qemu after that long.
TAG=${1:?tag}; FR=${2:?frames}; shift 2
NP=$(cd "$(dirname "$0")/../.." && pwd)
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
EXE=${EXE:-$B/armhf/np-armhf-full}
D=$B/runs/$TAG; mkdir -p $D
[ -d $B/rundata ] || cp -a /home/claude/msc/build/np/NPP2/rundata $B/rundata
rm -rf $D/data; cp -a $B/rundata $D/data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
export MSC_VGX_HASH=1 MSC_VGX_FRAMESTATS=$D/vgx_frames.csv
export MSC_STATE_HASH=${HASHEVERY:-120} MSC_MAX_FRAMES=$FR MSC_QEMU_MARK=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=${WATCH_EVERY:-30} MSC_DUMP_MAPS=1 MSC_TASK_MARKS=1
PLUG=/home/claude/msc/npw/NPP2/tools/perf/libframecount2.so
for kv in "$@"; do export "$kv"; done
echo "== run $TAG frames=$FR $* $(date +%T)" | tee $D/cmd.txt
cd $NP
${TMO:+timeout -k 3 $TMO} /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf \
  -plugin $PLUG -d plugin $EXE > $D/run.log 2> $D/err.log
echo "rc=$? $(date +%T)" | tee -a $D/cmd.txt
cat $D/err.log >> $D/run.log
grep -c "insn\] mark" $D/run.log | sed 's/^/marks: /'
