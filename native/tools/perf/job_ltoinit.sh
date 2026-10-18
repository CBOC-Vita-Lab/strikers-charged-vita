#!/bin/bash
# NPP2 job (one heavy slot): is the ThinLTO build's frame hash stable with -ftrivial-auto-var-init=zero (NPB3's AUTOINIT=zero)? Builds the snapshot as LTO (the default)
# with AUTOINIT=zero, then: nofork, thread (frees deferred), nofork again, nofork with every game-heap allocation filled with 0xA5: frame 2996 (the frame
# whose hash differs between runs of the plain LTO build) and 1622-1919 (the 3D menus: they differ under NP_HEAP_POISON) against the first nofork run.
#   tools/heavy.sh -t 800 env V=a10 SRC=<snapshot> BUILD=1 tools/perf/job_ltoinit.sh
cd /home/claude/msc/npw/NPP2 || exit 1
V=${V:-a10}
B=/home/claude/msc/build/np/NPP2
[ -n "$BUILD" ] && { TAILN=6 tools/perf/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1 AUTOINIT=zero} || exit 1; }
export MAXF=${MAXF:-3500}
BASE="MSC_FS_FREEGATE=2 MSC_FS_FREEHIST=1"
echo "=== $V nofork $(date +%T)"; V=$V TAG=nofork FS=nofork XVARS="$BASE" FSTAIL=1 tools/perf/job_one.sh 2>&1 | tail -3 | cut -c1-200
echo "=== $V thread"; V=$V TAG=thread FS=thread XVARS="$BASE" REF=$B/runs/$V-nofork FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|frame 2"
echo "=== $V again1"; V=$V TAG=again1 FS=nofork XVARS="$BASE" REF=$B/runs/$V-nofork FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|frame 2"
echo "=== $V heapA5"; V=$V TAG=heapA5 FS=nofork XVARS="$BASE NP_HEAP_POISON=0xA5" REF=$B/runs/$V-nofork FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|frame 2"
