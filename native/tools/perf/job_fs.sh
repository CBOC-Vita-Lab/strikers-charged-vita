#!/bin/bash
# NPP2 job (one heavy slot): the same armhf binary in several frame-send modes on one nav script (30 fps virtual pacing), then the
# per-scene reports and the equality checks against the first mode.
#   tools/heavy.sh -t 900 env V=a1 MODES="off inline" tools/perf/job_fs.sh      (BUILD=1 SRC=<snapshot> BUILDVARS=".." builds first)
# Output: build/np/NPP2/runs/<V>-<tag>/{err.log,run.log,vgx_frames.csv}; the tables go to stdout. The tag is the mode, except off = "nofork".
# The mode value and the tag have six characters in every mode: the process layout (the stack position through the size of the environment,
# which holds the value and the output path) changes the open-play simulation (docs/wave2-NPP.md 7.1), and runs of one binary that are
# to be compared must have the same layout.
cd /home/claude/msc/npw/NPP2 || exit 1
B=/home/claude/msc/build/np/NPP2
V=${V:-a1}; MODES=${MODES:-off thread}
tools/perf/plugin_selftest.sh 2>&1 | tail -6   # (seconds: the race-check plugin on the unit test, before the long runs)
[ -n "$BUILD" ] && { TAILN=6 tools/perf/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1 LTO=none} || exit 1; }
NAV=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
dirs=
for m in $MODES; do
  t=$m; [ $m = off ] && t=nofork
  echo "=== $V $t $(date +%T)"
  TMO=${RUNT:-300} tools/perf/run.sh $V $t ${MAXF:-4400} marks MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000} MSC_FRAMESEND=$t MSC_FS_MARKS=1 $XVARS 2>&1 | tail -3
  grep -a "^\[fs\]" $B/runs/$V-$t/err.log | tail -8
  python3 -I tools/perf/report.py $B/runs/$V-$t/err.log --json $B/runs/$V-$t/report.json | cut -c1-200 | tail -${REPT:-40}
  python3 -I tools/perf/fsreport.py $B/runs/$V-$t/err.log --json $B/runs/$V-$t/fs.json | cut -c1-230
  dirs="$dirs $B/runs/$V-$t"
done
python3 -I tools/perf/cmp_runs.py $dirs ${CMPEXTRA} --nav
