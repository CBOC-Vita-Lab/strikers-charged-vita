#!/bin/bash
# SKIN (W5): run a host32 executable (null or GL back end) on a nav script with the renderer census on; output in build/np/W5SKIN/runs/<tag>/
#   tools/skin/runh.sh <exe> <tag> <nav script> <max frames> [KEY=VAL ...]
# (after tools/gxd/runh.sh). HASHMODE=0 switches the frame hash off; NOASLR=1 fixes the address space; DUMP="f1,f2,..." writes PNGs (GL exe).
# The environment is padded to a fixed size so that runs of different executables / switches have the same process layout.
EXE=${1:?exe}; TAG=${2:?tag}; NAV=${3:?script}; FR=${4:?frames}; shift 4
B=${SKINB:-/home/claude/msc/build/np/W5SKIN}
D=$B/runs/$TAG; rm -rf $D; mkdir -p $D/data
[ -n "$SAVE" ] && cp -a /home/claude/msc/build/np/B3/rundata-save/. $D/data/
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
for kv in "$@"; do export "$kv"; done
[ "${HASHMODE:-1}" != 0 ] && export MSC_VGX_HASH=${HASHMODE:-1}
export MSC_VGX_FRAMESTATS=$D/vgx_frames.csv MSC_STATE_HASH=${HASHEVERY:-120} MSC_MAX_FRAMES=$FR MSC_FRAME_LOG=100000 MSC_WATCH=1 MSC_WATCH_EVERY=100000 NP_DET_NAN=1
export MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_FRAMESEND=${FS:-nofork}
[ -n "$FRAME_US" ] && export MSC_VIRTUAL_FRAME_US=$FRAME_US
if [ -n "$DUMP" ]; then mkdir -p $D/png; export MSC_DUMP_DIR=$D/png MSC_DUMP_LIST=$DUMP; fi
export SKIN_PAD=
L0=$(( $(env | wc -c) + ${#EXE} )); NPAD=$(( ${PADTO:-60000} - L0 - 9 ))
[ $NPAD -gt 0 ] || { echo "runh.sh: environment already $L0 bytes" >&2; exit 2; }
export SKIN_PAD=$(head -c $NPAD /dev/zero | tr '\0' 'x')
echo "== runh $TAG exe=$EXE script=$NAV frames=$FR $* $(date +%T)" | tee $D/cmd.txt
cd $D
RUNNER=; [ -n "$GLRUN" ] && RUNNER=$GLRUN
${TMO:+timeout -k 3 $TMO} ${NOASLR:+setarch -R} $RUNNER $EXE > $D/run.log 2> $D/err.log
echo "rc=$? $(date +%T) frames in csv: $(($(wc -l < $D/vgx_frames.csv 2>/dev/null || echo 1) - 1))" | tee -a $D/cmd.txt
rm -rf $D/data
