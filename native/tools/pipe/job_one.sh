#!/bin/bash
# NPP2 job (one heavy slot): ONE nav_match60 run of an existing build under a tag (the tag must have six characters like the others when the run is
# to be compared with them), then the equality check against REF (a run directory).
#   tools/heavy.sh -t 400 env V=a3 TAG=nofor2 REF=/home/claude/msc/build/np/W5PIPE/runs/a3-nofork FS=nofork tools/pipe/job_one.sh
cd /home/claude/msc/npw/W5PIPE || exit 1
B=/home/claude/msc/build/np/W5PIPE
V=${V:-a3}; TAG=${TAG:?tag}; NAV=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
TMO=${RUNT:-300} tools/pipe/run.sh $V $TAG ${MAXF:-4400} marks MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000} MSC_FRAMESEND=${FS:-nofork} MSC_FS_MARKS=1 $XVARS 2>&1 | tail -3
grep -a "^\[fs\]\|^\[thp\]" $B/runs/$V-$TAG/err.log | tail -${FSTAIL:-8}
python3 -I tools/pipe/fsreport.py $B/runs/$V-$TAG/err.log | cut -c1-230 | tail -6
[ -n "$REF" ] && python3 -I tools/pipe/cmp_runs.py $REF $B/runs/$V-$TAG --nav
