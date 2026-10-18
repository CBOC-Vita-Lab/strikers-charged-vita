#!/bin/bash
# NPP2: the race-check plugin on a frame-send run (one heavy slot; qemu with memory callbacks is about 5-10 times slower than the plain run).
#   tools/heavy.sh -t 900 tools/pipe/run_race.sh <variant> <tag> <frames> <from-frame> <to-frame> [VAR=value ...]
# Output: build/np/NPP2/runs/<variant>-<tag>/{err.log,run.log,race.txt}; race.txt is racereport.py's table.
V=${1:?variant}; TAG=${2:?tag}; FR=${3:?frames}; FROM=${4:?from}; TO=${5:?to}; shift 5
B=/home/claude/msc/build/np/W5PIPE
EXE=${EXE:-$B/$V/armhf/np-armhf-full}
D=$B/runs/$V-$TAG; mkdir -p $D $B/rundata
rm -rf $D/data; cp -a $B/rundata $D/data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
export MSC_MAX_FRAMES=$FR MSC_QEMU_MARK=1 MSC_FS_MARKS=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=60 MSC_DUMP_MAPS=1 MSC_STATE_HASH=${HASHEVERY:-120}
export MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000} MSC_FRAMESEND=${MODE:-thread}
export MSC_INPUT_SCRIPT=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
for kv in "$@"; do export "$kv"; done
cd /home/claude/msc/npw/W5PIPE
echo "== race $V/$TAG frames=$FR window $FROM-$TO mode=$MSC_FRAMESEND $(date +%T)" | tee $D/cmd.txt
${TMO:+timeout -k 3 $TMO} /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf \
  -plugin /home/claude/msc/build/np/W5PIPE/plug/libracecheck${RACELIB:-}.so,from=$FROM,to=$TO -d plugin $EXE > $D/run.log 2> $D/err.log
echo "rc=$? $(date +%T)" | tee -a $D/cmd.txt
cat $D/err.log >> $D/run.log
python3 -I tools/pipe/racereport.py $EXE $D/err.log --top ${TOP:-80} --benign | tee $D/race.txt | cut -c1-260
