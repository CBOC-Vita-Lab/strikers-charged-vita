#!/bin/bash
# NPP2 job (one heavy slot): the ThinLTO-default build (variant V, built from SRC), the nav_match60 run with the frame send off (per-scene
# costs), the skin-kernel A/B (MSC_SKIN_FUSE=1 / 0: pictures must be identical, the walk-out cut-scene and the 3D menus cheaper), the
# THP decode-ahead A/B on the intro movie (MSC_THP_LANE=1 / 0).
#   tools/heavy.sh -t 900 env V=a4 SRC=<snapshot> tools/perf/job_lto.sh
cd /home/claude/msc/npw/NPP2 || exit 1
B=/home/claude/msc/build/np/NPP2
V=${V:-a4}
TAILN=6 tools/perf/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1} || exit 1
echo "link flags: $(grep -c 'flto=thin' $B/$V/make.log) lines with flto=thin in the build log"
NAV=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
dirs=
for f in 1 0; do
  echo "=== $V skin fuse=$f $(date +%T)"
  TMO=${RUNT:-300} tools/perf/run.sh $V fuse$f 4400 marks MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=33000 MSC_FRAMESEND=nofork MSC_SKIN_FUSE=$f 2>&1 | tail -2
  python3 -I tools/perf/report.py $B/runs/$V-fuse$f/err.log --json $B/runs/$V-fuse$f/report.json --tasks | cut -c1-200 | tail -${REPT:-90}
  dirs="$dirs $B/runs/$V-fuse$f"
done
python3 -I tools/perf/cmp_runs.py $dirs --nav
dirs=
for lane in 0 1; do
  echo "=== $V thp lane=$lane $(date +%T)"
  TMO=${RUNT:-280} tools/perf/run.sh $V thp$lane 2500 marks MSC_INPUT_SCRIPT=/home/claude/msc/npw/NPP2/tools/nav/movie_end.txt MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=16667 MSC_THP_LANE=$lane MSC_FRAMESEND=nofork $XVARS 2>&1 | tail -2
  grep -a "^\[thp\]" $B/runs/$V-thp$lane/err.log | tail -3
  python3 -I tools/perf/report.py $B/runs/$V-thp$lane/err.log --json $B/runs/$V-thp$lane/report.json --min-frames 30 | cut -c1-200 | tail -6
  dirs="$dirs $B/runs/$V-thp$lane"
done
python3 -I tools/perf/cmp_runs.py $dirs
