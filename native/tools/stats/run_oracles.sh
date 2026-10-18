#!/bin/bash
# several oracle matches in one heavy slot (~150 s each):  heavy.sh -t 590 tools/stats/run_oracles.sh <prefix> "<seed list>"   (logs $OUT/<prefix>_<seed>.log)
P=$1; SEEDS=$2
D=$(cd "$(dirname "$0")" && pwd)
for s in $SEEDS; do echo -n "oracle seed $s: "; TMO=${TMO:-190} $D/run_oracle.sh ${P}_$s $s; done
