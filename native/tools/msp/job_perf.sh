#!/bin/bash
# MSP job (one heavy slot): Performance Mode A/B on the armhf build, the same binary twice on nav_match60 (menus, stadium intro, walk-out, open play),
# game thread instructions per presented frame per scene (report.py), simulation digests compared.
#   tools/heavy.sh -t 900 env FS=nofork tools/msp/job_perf.sh [tagprefix]
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
P=${1:-pm}
NAV=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
for mode in 0 1; do
  TMO=${RUNT:-330} tools/msp/perf_run.sh $P$mode ${MAXF:-4400} MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000} MSC_FRAMESEND=${FS:-nofork} MSC_PERFORMANCE_MODE=$mode $XVARS 2>&1 | tail -3
done
python3 -I tools/perf/cmp_runs.py $B/runs/${P}0 $B/runs/${P}1 --nav | cut -c1-200 | head -12
for mode in 0 1; do
  echo "=== report $P$mode"; python3 -I tools/perf/report.py $B/runs/$P$mode/err.log --tasks --json $B/runs/$P$mode/report.json 2>&1 | cut -c1-170 | head -70
done
