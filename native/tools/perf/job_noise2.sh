#!/bin/bash
# NPP2 job (one heavy slot): is a frame hash of an existing build stable? Runs of the same binary as an earlier nofork run (REF, made by job_free.sh with
# XVARS "MSC_FS_FREEGATE=1 MSC_FS_FREEHIST=1"): again with the identical environment, with the environment longer (stack addresses move) and with every
# game-heap allocation filled with 0xA5. Which frames differ from REF.
#   tools/heavy.sh -t 600 env V=a8 tools/perf/job_noise2.sh
cd /home/claude/msc/npw/NPP2 || exit 1
V=${V:-a8}
B=/home/claude/msc/build/np/NPP2
REF=$B/runs/$V-nofork
BASE="MSC_FS_FREEGATE=1 MSC_FS_FREEHIST=1"
export MAXF=${MAXF:-3500}
echo "=== $V again1"; V=$V TAG=again1 FS=nofork XVARS="$BASE" REF=$REF FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|differing\|frame 2"
echo "=== $V padded"; V=$V TAG=padded FS=nofork XVARS="$BASE NPPPAD=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" REF=$REF FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|differing\|frame 2"
echo "=== $V heapA5"; V=$V TAG=heapA5 FS=nofork XVARS="$BASE NP_HEAP_POISON=0xA5" REF=$REF FSTAIL=1 tools/perf/job_one.sh 2>&1 | grep -a "equal\|differing\|frame 2"
