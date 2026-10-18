#!/bin/bash
# disc.sh part A with the post-fix binary: plain qemu, data dir with a save file, nav_match60, 3800 frames, pad 0 vs 16.  tuned2 (00:51) diverged at f=3240.
#   heavy.sh -t 400 tools/det/disc2.sh <exe>
B=/home/claude/msc/build/np; NP=/home/claude/msc/np; EXE=${1:-$B/NPP/tuned3/armhf/np-armhf-full}; NAV=/home/claude/msc/tools/scripts/nav_match60.txt
cd $NP
for P in 0 16; do tools/det/qrun.sh -x $EXE -s $NAV -f 3800 -H 30 -n disc2-pad$P -p $P -k $B/B3/rundata-save | tail -1; done
echo "== plain qemu, save file, $(basename $(dirname $(dirname $EXE))): pad 0 vs 16"; diff <(grep -a '^\[hash\]' $B/B3/disc2-pad0.log) <(grep -a '^\[hash\]' $B/B3/disc2-pad16.log) | head -4; echo "(end of diff)"
