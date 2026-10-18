#!/bin/bash
# NPP2 job (one heavy slot): build a snapshot (BUILD=1 SRC=<snapshot>), then the same binary on nav_match60 (4400 frames, so that the walk-out and the 3D menus are in it):
#   nofork, MSC_SKIN_LANE=0    (the reference)
#   nofork, MSC_SKIN_LANE=1    the software skinning of cut-scenes and 3D menus on the helper lane: equal pictures? how much off the game thread?
#   thread, MSC_SKIN_LANE=1    both second-core features at once
#   tools/heavy.sh -t 700 env V=a9 SRC=<snapshot> BUILD=1 tools/perf/job_skin.sh
cd /home/claude/msc/npw/NPP2 || exit 1
V=${V:-a9}
B=/home/claude/msc/build/np/NPP2
[ -n "$BUILD" ] && { TAILN=6 tools/perf/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1 LTO=none} || exit 1; }
export MAXF=${MAXF:-4400}
echo "=== $V nofork lane0 $(date +%T)"; V=$V TAG=nofork FS=nofork XVARS="MSC_SKIN_LANE=0" FSTAIL=2 tools/perf/job_one.sh
echo "=== $V lane1 (nofork + skin lane) $(date +%T)"; V=$V TAG=lane1x FS=nofork XVARS="MSC_SKIN_LANE=1" REF=$B/runs/$V-nofork FSTAIL=3 tools/perf/job_one.sh
echo "=== $V thread + lane $(date +%T)"; V=$V TAG=thrdln FS=thread XVARS="MSC_SKIN_LANE=1" REF=$B/runs/$V-nofork FSTAIL=8 tools/perf/job_one.sh
