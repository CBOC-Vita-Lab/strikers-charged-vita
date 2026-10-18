#!/bin/bash
# The recompiled build must not change: build the null + HLE host of THIS checkout and run the two regression references
# (tools/refcheck.sh: compat against build/host-gl/s6, def against checks/r6/ref_def; 8457 frames each, hashes of everything handed to
# the back end). Adapted from build/one/vgx_check.sh. Run through the heavy lock:
#   /home/claude/msc/tools/heavy.sh -t 2700 native/test/refcheck.sh
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
B=${B:-/home/claude/msc/build/np/NPR/refcheck}
mkdir -p $B
make null HLE=1 BUILD=$B -j2 > $B/build.log 2>&1 || { tail -30 $B/build.log; exit 1; }
echo "build ok"
tools/vita_syntax.sh
export REFCHECK_REFS=/home/claude/msc/build/host-gl/checks REFCHECK_OUT=$B/checks
tools/refcheck.sh np $B/msc_null_hle; echo "refcheck rc=$?"
