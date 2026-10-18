#!/bin/bash
# GXD (wave 5): run an armhf native build under qemu-arm with the instruction-count plugin. ONE job per call; always via heavy.sh:
#   tools/heavy.sh -t 500 tools/gxd/run.sh <variant> <tag> <frames> [marks|profile] [VAR=value ...]
# variant: the executable $B/armhf/np-armhf-full-<variant> (tools/gxd/build.sh). Output: $B/runs/<variant>-<tag>/{run.log,err.log,prof.txt[.N],vgx_frames.csv}
# PERF=1 (default for profile mode): the renderer's instrumentation is OFF (no MSC_VGX_HASH, no MSC_VGX_FRAMESTATS: the hash costs ~1 M instructions per frame
#   on the render thread and the detailed per-draw statistics ~0.3 M, neither exists on the console). HASH=1: both ON (picture equality runs).
# WINDOWS=a-b+c-d : several profile windows in one run (frame numbers of the marks) -> prof.txt.0, prof.txt.1 ...
# TMO=<s> stops qemu after that long. NAV=<script> (default tools/scripts/nav_match60.txt: boot, menus, captains, sides, stadium, walk-out, match)
V=${1:?variant}; TAG=${2:?tag}; FR=${3:?frames}; MODE=${4:-marks}; shift 4 2>/dev/null
B=${GXDB:-/home/claude/msc/build/np/W5GXD}
SRC=${SRC:-/home/claude/msc/npw/W5GXD}
EXE=${EXE:-$B/b/armhf/np-armhf-full-$V}
D=$B/runs/$V-$TAG; mkdir -p $D
rm -rf $D/data; cp -a $B/rundata $D/data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
for kv in "$@"; do export "$kv"; done
if [ "${HASH:-0}" = 1 ]; then export MSC_VGX_HASH=1 MSC_VGX_FRAMESTATS=$D/vgx_frames.csv; fi
export MSC_STATE_HASH=${HASHEVERY:-120} MSC_MAX_FRAMES=$FR MSC_QEMU_MARK=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=${WATCH_EVERY:-30} MSC_DUMP_MAPS=1
export MSC_INPUT_SCRIPT=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt} MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000} MSC_FRAMESEND=${FS:-nofork} MSC_FS_MARKS=1
# The simulation depends on the process layout (docs/wave2-NPP.md 7.1: the stack position moves with the size of the environment and argv[0]); runs that are to be compared
# must have byte-identical environment sizes whatever the variant / tag names are: GXD_PAD fills the environment (and the executable path) up to PADTO bytes.
export GXD_PAD=
L0=$(( $(env | wc -c) + ${#EXE} ))
NPAD=$(( ${PADTO:-60000} - L0 - 9 ))
[ $NPAD -gt 0 ] || { echo "run.sh: environment already $L0 bytes, raise PADTO" >&2; exit 2; }
export GXD_PAD=$(head -c $NPAD /dev/zero | tr '\0' 'x')
echo "layout: env+argv0 bytes $(( $(env | wc -c) + ${#EXE} ))" | tee $D/cmd.txt
PLUG=$B/libframecount2.so
if [ "$MODE" = profile ]; then
  if [ -n "$WINDOWS" ]; then PLUG="$PLUG,profile=$D/prof.txt,windows=$WINDOWS"
  else PLUG="$PLUG,profile=$D/prof.txt,from=${PFROM:-1},to=${PTO:-$FR}"; fi
fi
echo "== run $V/$TAG frames=$FR mode=$MODE hash=${HASH:-0} $* $(date +%T)" | tee -a $D/cmd.txt
cd $SRC
${TMO:+timeout -k 3 $TMO} /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf \
  -plugin $PLUG -d plugin $EXE > $D/run.log 2> $D/err.log
echo "rc=$? $(date +%T)" | tee -a $D/cmd.txt
cat $D/err.log >> $D/run.log
grep -ac "insn\] mark" $D/run.log | sed 's/^/marks: /'
rm -rf $D/data
