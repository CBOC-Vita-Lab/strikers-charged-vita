#!/bin/bash
# NPP2 job (one heavy slot): build a snapshot (BUILD=1 SRC=<snapshot>), then the same binary three times on nav_match60 (3500 frames):
#   nofork                         (the reference)
#   thread, MSC_FS_FREEGATE=1      a free of an old block joins the send: with the call stacks of those frees (MSC_FS_FREEHIST=1)
#   thread, MSC_FS_FREEGATE=2      such a free is kept until the join
# each compared with the reference (simulation digests, renderer frame hashes, [nav] lines) and reported per scene (main thread, job, critical path).
#   tools/heavy.sh -t 650 env V=a7 SRC=<snapshot> BUILD=1 tools/pipe/job_free.sh
cd /home/claude/msc/npw/W5PIPE || exit 1
V=${V:-a7}
B=/home/claude/msc/build/np/W5PIPE
[ -n "$BUILD" ] && { TAILN=6 tools/pipe/build.sh $V ${BUILDVARS:-OPT=-O2 NOPIC=1 LTO=none} || exit 1; }
export MAXF=${MAXF:-3500}
H="MSC_FS_FREEHIST=1"
echo "=== $V nofork $(date +%T)"; V=$V TAG=nofork FS=nofork XVARS="MSC_FS_FREEGATE=1 $H" FSTAIL=2 tools/pipe/job_one.sh
if [ "${FGATE1:-1}" = 1 ]; then   # FGATE1=0 skips the run with joining frees (and its call stacks)
echo "=== $V fgate1 (thread, old-block frees join) $(date +%T)"; V=$V TAG=fgate1 FS=thread XVARS="MSC_FS_FREEGATE=1 $H" REF=$B/runs/$V-nofork FSTAIL=40 tools/pipe/job_one.sh

fi
echo "=== $V fgate2 (thread, old-block frees deferred) $(date +%T)"; V=$V TAG=fgate2 FS=thread XVARS="MSC_FS_FREEGATE=2 $H" REF=$B/runs/$V-nofork FSTAIL=12 tools/pipe/job_one.sh
