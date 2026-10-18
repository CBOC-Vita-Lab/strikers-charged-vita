#!/bin/bash
# NPP2 job (one heavy slot): the merged tree as handed over (ThinLTO is the armhf default): build, then the same binary on nav_match60 (4,400 frames):
#   nofork                    the original path
#   thread                    frame send on the second core (MSC_FS_FREEGATE=2, open play)
#   thread + skin lane        both second-core features (cut-scene skinning on the helper lane)
# each against nofork: simulation digests, renderer frame hashes, [nav] lines; per-scene numbers.
#   tools/heavy.sh -t 800 env V=a11 SRC=<snapshot> BUILD=1 BUILDVARS="OPT=-O2 NOPIC=1" tools/perf/job_final.sh
cd /home/claude/msc/npw/NPP2 || exit 1
V=${V:-a11}
B=/home/claude/msc/build/np/NPP2
[ -n "$BUILD" ] && { TAILN=6 tools/perf/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1} || exit 1; }
export MAXF=${MAXF:-4400}
echo "=== $V nofork $(date +%T)"; V=$V TAG=nofork FS=nofork XVARS="MSC_SKIN_LANE=0" FSTAIL=2 tools/perf/job_one.sh
echo "=== $V thread $(date +%T)"; V=$V TAG=thread FS=thread XVARS="MSC_SKIN_LANE=0" REF=$B/runs/$V-nofork FSTAIL=8 tools/perf/job_one.sh
echo "=== $V thread + skin lane $(date +%T)"; V=$V TAG=thrdln FS=thread XVARS="MSC_SKIN_LANE=1" REF=$B/runs/$V-nofork FSTAIL=8 tools/perf/job_one.sh
