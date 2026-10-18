#!/bin/bash
# One short heavy.sh slot: run np-host32-sdk under gdb until the first crash and print where it is.
B=/home/claude/msc/build/np
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$B/rundata
cd /home/claude/msc/np
MSC_NO_CRASH_HANDLER=1 timeout 100 gdb -q -batch -x tools/diag_crash.gdb $B/host32/np-host32-sdk > $B/diag-crash.log 2>&1
grep -v "^NP STUB" $B/diag-crash.log | head -60
