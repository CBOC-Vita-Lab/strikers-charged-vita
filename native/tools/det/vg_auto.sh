#!/bin/bash
# Find valgrind options that get through the SSE code of the host32 build (valgrind 3.22's x86 back end dies with "iselFltExpr_wrk" on some
# float selects), then run the long job. Tries each option set for the first frames; the first that does not die continues to MSC frames.
#   heavy.sh -t 590 tools/det/vg_auto.sh [frames]
FR=${1:-3400}; NP=/home/claude/msc/np; B=/home/claude/msc/build/np
for o in "--vex-guest-chase=no --vex-iropt-level=1" "--vex-guest-chase=no --vex-iropt-level=0"; do
  echo "== try: $o (900 frames)"
  timeout 100 $NP/tools/det/vg.sh -n try -f 900 -- $o | tail -2
  if ! grep -q "impossible" $B/B3/vg-try.log 2>/dev/null; then
    echo "== works: $o -> long run, $FR frames"
    timeout 420 $NP/tools/det/vg.sh -n k1 -f $FR -o yes -- $o | tail -3
    exit 0
  fi
done
echo "== no option set got through"
