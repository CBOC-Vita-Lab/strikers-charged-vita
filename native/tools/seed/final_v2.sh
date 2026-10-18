#!/bin/bash
# W5-SEED: the second Vita3K turn: the control (the OLD seed, same stadium script: what the old seed does in a stadium it never saw) and the CORE list
# (only the eager tier is warmed at the first frame; all GXPs are in the seed: first use of anything else is a link from the cache). Needs final_v1.sh's packages.
cd "$(dirname "$0")/../.."
R=/home/claude/msc/build/np/W5SEED
LAB=/home/claude/msc/build/np/W5INT/lto/vita/pkg/np/mscharged-np.vpk
OLDSEED=/home/claude/msc/build/np/W5INT/seed/shadercache
V=; [ "${NOVITA:-0}" = 1 ] && V=echo
tools/seed/pack_vpk.sh $LAB $OLDSEED $R/pkg/lab-old.vpk
echo "== control, old seed ($(date +%T))"
$V tools/seed/v3k_proof.sh -s 420 -v $R/pkg/lab-old.vpk -S $R/nav/stad_kong6.txt old_kong6
echo "== core list, full GXP seed ($(date +%T), used $SECONDS s)"
if [ $SECONDS -lt 700 ]; then $V tools/seed/v3k_proof.sh -s 420 -v $R/pkg/lab-core.vpk -S $R/nav/stad_kong6.txt core_kong6; else echo "no time left: core_kong6 goes to the next turn"; fi
echo "== done ($(date +%T), used $SECONDS s)"
