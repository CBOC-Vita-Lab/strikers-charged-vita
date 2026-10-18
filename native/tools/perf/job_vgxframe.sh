#!/bin/bash
# NPP2 job (one heavy slot): is the renderer's frame hash stable when the vgx player runs on the game thread at present (MSC_VGX_PLAY=frame: no render thread racing
# the game thread over memory the recorder refers to)? Same binary, three runs: nofork, nofork again, thread (frees deferred): against the first.
#   tools/heavy.sh -t 500 env V=a10 tools/perf/job_vgxframe.sh
cd /home/claude/msc/npw/NPP2 || exit 1
V=${V:-a10}
B=/home/claude/msc/build/np/NPP2
export MAXF=${MAXF:-3500}
BASE="MSC_VGX_PLAY=frame MSC_FS_FREEGATE=2 ${EXTRAV:-MSC_FS_STATES=0x2}"
echo "=== $V pfnof1 $(date +%T)"; V=$V TAG=pfnof1 FS=nofork XVARS="$BASE" FSTAIL=1 tools/perf/job_one.sh 2>&1 | tail -3 | cut -c1-200
echo "=== $V pfnof2"; V=$V TAG=pfnof2 FS=nofork XVARS="$BASE" REF=$B/runs/$V-pfnof1 FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|frame 2"
echo "=== $V pfthrd"; V=$V TAG=pfthrd FS=thread XVARS="$BASE" REF=$B/runs/$V-pfnof1 FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|frame 2"
