#!/bin/bash
# GXD: run a host32 (x86, null back end) executable on a nav script with the renderer hash on; output in runs/<tag>/{err.log,vgx_frames.csv}
#   tools/gxd/runh.sh <exe> <tag> <nav script> <max frames> [KEY=VAL ...]
# Compare two runs with: python3 -I tools/perf/cmp_runs.py runs/<a> runs/<b> --nav
# The environment is padded to a fixed size (as run.sh does) so that runs of different executables have the same process layout.
EXE=${1:?exe}; TAG=${2:?tag}; NAV=${3:?script}; FR=${4:?frames}; shift 4
B=${GXDB:-/home/claude/msc/build/np/W5GXD}
D=$B/runs/$TAG; rm -rf $D; mkdir -p $D
cp -a $B/rundata $D/data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
for kv in "$@"; do export "$kv"; done
export MSC_VGX_HASH=${HASHMODE:-1} MSC_VGX_FRAMESTATS=$D/vgx_frames.csv MSC_STATE_HASH=${HASHEVERY:-120} MSC_MAX_FRAMES=$FR MSC_FRAME_LOG=100000 MSC_WATCH=1 MSC_WATCH_EVERY=100000 NP_DET_NAN=1
export MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000} MSC_FRAMESEND=${FS:-nofork}
export GXD_PAD=
L0=$(( $(env | wc -c) + ${#EXE} )); NPAD=$(( ${PADTO:-60000} - L0 - 9 ))
[ $NPAD -gt 0 ] || { echo "runh.sh: environment already $L0 bytes" >&2; exit 2; }
export GXD_PAD=$(head -c $NPAD /dev/zero | tr '\0' 'x')
echo "== runh $TAG exe=$EXE script=$NAV frames=$FR $* $(date +%T)" | tee $D/cmd.txt
cd /home/claude/msc/npw/W5GXD
# NOASLR=1: the address space layout is not randomised (setarch -R): data that depends on addresses (stale stack words) is then the same in every run
${TMO:+timeout -k 3 $TMO} ${NOASLR:+setarch -R} $EXE > $D/run.log 2> $D/err.log
echo "rc=$? $(date +%T) frames in csv: $(($(wc -l < $D/vgx_frames.csv 2>/dev/null || echo 1) - 1))" | tee -a $D/cmd.txt
rm -rf $D/data
