#!/bin/bash
# Several seeds in one heavy slot:  heavy.sh -t 600 tools/stats/run_seeds.sh <prefix> "<seed list>" [exe]   (logs $OUT/<prefix>_<seed>.log)
P=$1; SEEDS=$2; EXE=${3:-}
D=$(cd "$(dirname "$0")" && pwd)
for s in $SEEDS; do
  echo -n "seed $s: "; $D/run_seed.sh ${P}_$s $s $EXE
done
