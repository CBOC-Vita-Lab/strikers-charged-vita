#!/bin/bash
# run_live.sh: one heavy slot, several live differential runs of the goalie (see docs/wave3-NPG2.md).
#   heavy.sh -t 600 tools/dtlive/run_live.sh [seed] [exe]
# logs: build/np/NPG2/runs/dtl_<variant>_<seed>.log ; grep '\[dtl\]' for the results
NP=$(cd "$(dirname "$0")/../.." && pwd)
SEED=${1:-1}; EXE=${2:-/home/claude/msc/build/np/NPG2/host32/np-host32-dtl}
export NP_MEM1_BASE=80700000 NP_DTL=1 NP_DTL_EVERY=${NP_DTL_EVERY:-25} NP_DTL_MAX=${NP_DTL_MAX:-60} MSC_MAX_FRAMES=${FRAMES:-6500}
export OUT=/home/claude/msc/build/np/NPG2/runs
run() {  # name, extra env...
  local n=$1; shift
  env "$@" TMO=${TMO:-120} $NP/tools/stats/run_seed.sh dtl_${n}_$SEED $SEED $EXE
  grep -E '\[dtl\] (init|SUMMARY|test)' $OUT/dtl_${n}_$SEED.log | cut -c1-260 | tail -3
}
run control NP_DTL_CONTROL=1 NP_DTL_MAX=10
run mode1   NP_DTL_MODE=1 NP_DTL_MAX=70
run mode1b  NP_DTL_MODE=1 NP_DTL_NONIDLE=1 NP_DTL_EVERY=3 NP_DTL_MAX=120
run mode2   NP_DTL_MODE=2 NP_DTL_MAX=30
run mode4   NP_DTL_MODE=4 NP_DTL_EVERY=40 NP_DTL_MAX=24 NP_DTL_K=2
TMO=160 run mode3 NP_DTL_MODE=3 NP_DTL_EVERY=70 NP_DTL_MAX=360 NP_DTL_FNS=8 NP_DTL_K=2
