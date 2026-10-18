#!/bin/bash
# NPP: run an armhf native build under qemu-arm with the instruction-count plugin. ONE job per call; always via heavy.sh:
#   tools/heavy.sh -t 500 tools/pipe/run.sh <variant> <tag> <frames> [mode] [VAR=value ...]
# mode: marks   (default) per-frame marks + per-task marks + [ft] frame trace      -> task_budget.py / tools/pipe/report.py
#       profile  block profile of frames FROM..TO (env PFROM PTO, default 1/FRAMES) -> tools/pipe/prof_native.py
# TMO=<seconds> stops qemu after that long (the marks are streamed, so a cut run is still usable). Output: /home/claude/msc/build/np/W5PIPE/runs/<variant>-<tag>/{run.log,prof.txt}. Extra VAR=value arguments go to the game's environment
# (MSC_INPUT_SCRIPT=..., MSC_VIRTUAL_TIME=1, ...). EXE overrides the executable.
V=${1:?variant}; TAG=${2:?tag}; FR=${3:?frames}; MODE=${4:-marks}; shift 4 2>/dev/null
B=/home/claude/msc/build/np/W5PIPE
EXE=${EXE:-$B/$V/armhf/np-armhf-full}
D=$B/runs/$V-$TAG; mkdir -p $D $B/rundata
# a private copy of the save directory per run (the runs of one binary must start from the same state); the copy lives under the run directory,
# so its path adds the same number of characters to the environment in every run of equal-length tags
rm -rf $D/data; cp -a $B/rundata $D/data
export NP_DISC_DIR=/home/claude/msc/game NP_DATA_DIR=$D/data
export MSC_VGX_HASH=${VGXHASH:-1} MSC_VGX_FRAMESTATS=$D/vgx_frames.csv   # per-frame hash of what the renderer is handed (vgx NOTES: MSC_VGX_HASH)
export MSC_STATE_HASH=${HASHEVERY:-120} MSC_MAX_FRAMES=$FR MSC_QEMU_MARK=1 MSC_FRAME_TRACE=1 MSC_WATCH=1 MSC_WATCH_EVERY=${WATCH_EVERY:-30} MSC_DUMP_MAPS=1
PLUG=/home/claude/msc/build/np/W5PIPE/plug/libframecount2.so
if [ "$MODE" = profile ]; then
  if [ -n "$WINDOWS" ]; then PLUG="$PLUG,profile=$D/prof.txt,windows=$WINDOWS"   # several windows in one run: a-b+c-d -> prof.txt.0, .1 ...
  else PLUG="$PLUG,profile=$D/prof.txt,from=${PFROM:-1},to=${PTO:-$FR}"; fi
else
  export MSC_TASK_MARKS=1
fi
PLUG="$PLUG${PLUGARGS:+,$PLUGARGS}"   # e.g. PLUGARGS=aiupd (per-update AI script lines of the plugin, MSC_AI_MARKS=1)
for kv in "$@"; do export "$kv"; done
echo "== run $V/$TAG frames=$FR mode=$MODE $* $(date +%T)" | tee $D/cmd.txt
cd /home/claude/msc/npw/W5PIPE
${TMO:+timeout -k 3 $TMO} /home/claude/research/qemu-full/build/qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf \
  -plugin $PLUG -d plugin $EXE > $D/run.log 2> $D/err.log
echo "rc=$? $(date +%T)" | tee -a $D/cmd.txt
cat $D/err.log >> $D/run.log; 
grep -c "insn\] mark" $D/run.log | sed 's/^/marks: /'
