#!/bin/bash
# One whole-match run of the recompiled build (oracle, build/host-gl/msc_null_hle), virtual time, same script and seed convention as run_seed.sh.
#   heavy.sh -t 600 tools/stats/run_oracle.sh <name> <seed>   (log $OUT/<name>.log; ~150 s per match)
# Env: WATCHEVERY (default 60; goalie samples + per-player statistics), OUT.
NAME=$1; SEED=$2
NP=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-/home/claude/msc/build/np/NPG2/runs}; mkdir -p $OUT
D=$OUT/odata-$NAME; rm -rf $D; mkdir -p $D/MSCRecomp
printf '[video]\nframe_rate = 60\n[paths]\ndvd_root = "/home/claude/msc/game"\n' > $D/MSCRecomp/Config.toml
W="$(python3 $NP/tools/stats/mkwatch_goalie.py oracle --stats)"
( cd /home/claude/msc/wt/gx && XDG_DATA_HOME=$D MSC_INPUT_SCRIPT=$NP/tools/nav/nav_stats.txt MSC_WATCH="$W" MSC_WATCH_EVERY=${WATCHEVERY:-60} MSC_FRAME_LOG=100000 \
  MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_TICK_NS=$((1000 + 37 * SEED)) timeout -k 5 ${TMO:-400} /home/claude/msc/build/host-gl/msc_null_hle > $OUT/$NAME.log 2>&1 )
echo "rc=$? $(date +%T)" >> $OUT/$NAME.log
rm -rf $D
grep -o 'score=[0-9]*-[0-9]*' $OUT/$NAME.log | tail -1
