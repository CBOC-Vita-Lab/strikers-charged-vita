#!/bin/bash
# (copied into the repo from /home/claude/msc/npw/SKIN.seed1.sh; one heavy.sh turn per batch: tools/heavy.sh -t 900 tools/skin/seed_keys.sh)
# SKIN: shader keys of GPU skinning + soft instancing: W5SEED scenarios (lg, cap, ft, ft2, menu, dm, hu, cv, mt, pause, movie) with the switches on.
# One heavy turn (batch budget); rerun until 'done in all' reaches 172.
cd /home/claude/msc/npw/W5SKIN
export MAXBAD=1000 SEED_OUT=/home/claude/msc/build/np/W5SKIN/seed MSC_SKIN_GPU=1 MSC_VGX_SOFTINST=16
tools/seed/batch.sh -b ${BUDGET:-540} -X /home/claude/msc/build/np/W5SKIN/host32/np-host32-full-null /home/claude/msc/build/np/W5SKIN/seed/runs_skin.txt 2>&1 | tail -4
