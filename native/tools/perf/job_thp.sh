#!/bin/bash
# NPP2 job (one heavy slot): the THP decode-ahead lane A/B on the intro movie (movie_end script: the movie plays, no input): the same binary
# with MSC_THP_LANE=0 and 1; the game thread's instruction counts per frame in the movie, the lane's statistics, and whether the pictures
# (vgx per-frame hash) and the simulation digests are the same.
#   tools/heavy.sh -t 900 env V=a2 SRC=<snapshot> BUILD=1 BUILDVARS="OPT=-O2 NOPIC=1 LTO=none" tools/perf/job_thp.sh
cd /home/claude/msc/npw/NPP2 || exit 1
B=/home/claude/msc/build/np/NPP2
V=${V:-a2}
[ -n "$BUILD" ] && { TAILN=6 tools/perf/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1} || exit 1; }
NAV=${NAV:-/home/claude/msc/npw/NPP2/tools/nav/movie_end.txt}
dirs=
for lane in 0 1; do
  echo "=== $V thp lane=$lane $(date +%T)"
  TMO=${RUNT:-280} tools/perf/run.sh $V thp$lane ${MAXF:-2500} marks MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-16667} MSC_THP_LANE=$lane MSC_FRAMESEND=off $XVARS 2>&1 | tail -2
  grep -a "^\[thp\]" $B/runs/$V-thp$lane/err.log | tail -3
  python3 -I tools/perf/report.py $B/runs/$V-thp$lane/err.log --json $B/runs/$V-thp$lane/report.json --min-frames 30 | cut -c1-200 | tail -${REPT:-12}
  dirs="$dirs $B/runs/$V-thp$lane"
done
python3 -I tools/perf/cmp_runs.py $dirs
