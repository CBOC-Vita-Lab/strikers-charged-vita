#!/bin/bash
# Which factor of NPP's configuration exposes the environment-size dependence? (nppcfg.sh: plugin + marks + persistent save dir + tuned2 binary)
#   heavy.sh -t 590 tools/det/disc.sh
# A: plain qemu (no plugin), tuned2 binary, a data dir WITH a save file (copy of NPP's rundata), nav_match60, pads 0 and 16
# B: qemu PLUGIN + marks (tools/perf/run.sh), tuned2, a FRESH data dir (popup path), pads 0 and 16
B=/home/claude/msc/build/np; NP=/home/claude/msc/np; T2=$B/NPP/tuned2/armhf/np-armhf-full; NAV=/home/claude/msc/tools/scripts/nav_match60.txt
cd $NP
for P in 0 16; do tools/det/qrun.sh -x $T2 -s $NAV -f 3800 -H 30 -n discA-pad$P -p $P -k $B/B3/rundata-save | tail -1; done
echo "== A (plain qemu, save file, tuned2): pad 0 vs 16"; diff <(grep -a '^\[hash\]' $B/B3/discA-pad0.log) <(grep -a '^\[hash\]' $B/B3/discA-pad16.log) | head -4
for P in 0 16; do
  X=NPB3NONE=1; [ $P != 0 ] && X=NPB3PAD=$(head -c $P /dev/zero | tr '\0' 'x')
  D=$B/B3/data-discB-$P; rm -rf $D; mkdir -p $D
  HASHEVERY=30 TMO=300 tools/perf/run.sh tuned2 discB$P 3800 marks MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 NP_DATA_DIR=$D $X | tail -1; rm -rf $D
done
echo "== B (plugin + marks, fresh data, tuned2): pad 0 vs 16"; diff <(grep -a '^\[hash\]' $B/NPP/runs/tuned2-discB0/err.log) <(grep -a '^\[hash\]' $B/NPP/runs/tuned2-discB16/err.log) | head -4
