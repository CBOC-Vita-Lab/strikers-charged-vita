#!/bin/bash
# PIPE (W5): ONE nav run of the host32 null-back-end executable (native speed, the pictures are hashes), output /home/claude/msc/build/np/W5PIPE/hruns/<tag>/{err.log,vgx_frames.csv}
#   tools/heavy.sh -p -t 300 tools/pipe/h32run.sh <tag> <frames> [VAR=value ...]       env: EXE=<exe> NAV=<script> REF=<hruns dir to compare with> TMO=<seconds> NOASLR=1
# Tags are six characters (the length of the data path is part of the process layout; keep them equal in a comparison).
TAG=${1:?tag}; FR=${2:?frames}; shift 2
B=/home/claude/msc/build/np/W5PIPE
EXE=${EXE:-$B/h32/host32/np-host32-full-null}
NAV=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
D=$B/hruns/$TAG; rm -rf $D; mkdir -p $D $B/rundata
cp -a $B/rundata $D/data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
export MSC_VGX_HASH=1 MSC_VGX_FRAMESTATS=$D/vgx_frames.csv MSC_STATE_HASH=${HASHEVERY:-120} MSC_MAX_FRAMES=$FR MSC_WATCH=1 MSC_WATCH_EVERY=${WATCH_EVERY:-100000} MSC_FRAME_LOG=100000
export MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000}
for kv in "$@"; do export "$kv"; done
echo "== h32run $TAG frames=$FR $* $(date +%T)" | tee $D/cmd.txt
T0=$(date +%s)
cd /home/claude/msc/npw/W5PIPE
# NOASLR=1: setarch -R (no address space randomisation): stale stack words that reach a uniform block (a lit mesh with a singular position matrix, frame 8215 of the full-match script) are then the same in every run
timeout -k 3 ${TMO:-280} ${NOASLR:+setarch -R} $EXE > $D/out.log 2> $D/err.log
echo "rc=$? wall=$(( $(date +%s) - T0 )) s" | tee -a $D/cmd.txt
rm -rf $D/data
grep -a "^\[fs\]\|corrupt\|SIG\|Abort" $D/err.log | tail -${FSTAIL:-12} | cut -c1-220
[ -n "$REF" ] && python3 -I tools/pipe/cmp_runs.py $REF $D --nav
true
