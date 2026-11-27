#!/bin/bash
# price17_run.sh <tag> <frames> <quiet 0|1> [ENV=val ...]: host32 glnull run of the fast-boot Dump route at the Vita plan's defaults (kSpeed+kQuality) with the player's
# threads (frame-send thread, back-end thread), under tools/price17_shim.so (counts per thread). quiet=1 elides stdout/stderr log calls like NP_LOGGING=0.
# Output: /home/claude/s13/w8/price17-runs/<tag>/{run.log,shim.txt,exit.txt}.  Env WT/EXE override the worktree / binary.
TAG=$1; N=$2; Q=$3; shift 3
WT=${WT:-/home/claude/s13/wt/price17}; EXE=${EXE:-/home/claude/work/hbuild/price17/host32/np-host32-full-glnull}
OUT=/home/claude/s13/w8/price17-runs/$TAG; mkdir -p $OUT
D=$(mktemp -d /tmp/price17-run.XXXXXX); trap 'rm -rf "$D"' EXIT
PLAN=$(python3 /home/claude/s13/w8/fe8/plan_env.py $WT)
cd $D && flock /tmp/s13-heavy.lock nice -n 10 env NP_DISC_DIR=/home/claude/work/disc/extract-strikers NP_DATA_DIR=$D MSC_INPUT_SCRIPT=$WT/tools/console-1.2/fastboot/routes/fb-dump.txt $PLAN \
  MSC_FRAMESEND=thread MSC_VGX_BACKEND_THREAD=1 MSC_VITA_CORE_FRAMESEND=1 NP_VITA_CORE_THP=12 MSC_FS_PIPE=1 MSC_VIRTUAL_TIME=1 MSC_CLOCK_STATS=1 MSC_MAX_FRAMES=$N MSC_AI_BUDGET=off MSC_LAT=0 \
  SHIM_QUIET=$Q SHIM_OUT=$OUT/shim.txt LD_PRELOAD=/home/claude/work/price17_shim.so "$@" $EXE > $OUT/run.log 2>&1
echo "exit $?" > $OUT/exit.txt
