#!/bin/bash
# NPP2 job: build a snapshot (BUILD=1 SRC=<snapshot>), run the frame-send modes (job_fs.sh: equality + per-scene tables), and if a fork
# mode crashed, run the first crashing one again under qemu's gdb stub (backtrace + the job's own trace, MSC_FS_TRACE=1).
#   tools/heavy.sh -t 800 env V=a6 SRC=<snap> BUILD=1 MODES="off inline serial thread" tools/perf/job_fsdbg.sh
cd /home/claude/msc/npw/NPP2 || exit 1
V=${V:-a6}
B=/home/claude/msc/build/np/NPP2
MODES="${FSMODES:-off inline serial thread}" MAXF=${MAXF:-3500} tools/perf/job_fs.sh   # (MODES in the environment of a queued job is ignored on purpose: FSMODES)
for m in ${GDBMODES:-inline serial thread}; do
  if grep -aq "rc=139" $B/runs/$V-$m/cmd.txt 2>/dev/null; then
    echo "=== gdb $V $m $(date +%T)"
    V=$V FS=$m MAXF=${GDBMAXF:-2500} TMO=${TMO:-150} XVARS="MSC_FS_TRACE=1" tools/perf/job_gdb.sh
    break
  fi
done
