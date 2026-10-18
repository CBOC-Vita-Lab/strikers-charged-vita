#!/bin/bash
# NPP job A (one heavy slot): build the tuned variant, run the oracle's nav script on it (marks), per-scene report.
#   tools/heavy.sh -t 700 tools/perf/job_a.sh
cd /home/claude/msc/npw/NPP2 || exit 1
B=/home/claude/msc/build/np/NPP2
V=${V:-tuned}
TAILN=6 tools/perf/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1} || exit 1
TMO=${RUNT:-330} tools/perf/run.sh $V ${TAG:-nav} ${MAXF:-9000} marks MSC_INPUT_SCRIPT=${NAV:-/home/claude/msc/tools/scripts/nav_kickoff.txt} MSC_VIRTUAL_TIME=1 ${XVARS}
python3 -I tools/perf/report.py $B/runs/$V-${TAG:-nav}/err.log --tasks --json $B/runs/$V-${TAG:-nav}/report.json | cut -c1-170 | tail -90
grep -a "^\[nav\]\|^\[script\]" $B/runs/$V-${TAG:-nav}/err.log | cut -c1-200 | tail -40
