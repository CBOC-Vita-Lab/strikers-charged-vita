#!/bin/bash
# NPP's exact run configuration (qemu plugin, marks, watch, persistent data dir) with a dummy environment variable of N bytes, to see
# whether the layout dependence of wave2-NPP 7.1 shows up there (it does not in a plain qemu run, tools/det/qsweep.sh).
#   heavy.sh -t 900 tools/det/nppcfg.sh <variant> <frames> pad1 pad2 ...      (hash every 30 frames; runs/<variant>-b3pad<N>/err.log)
V=$1; FR=$2; shift 2
cd /home/claude/msc/np
for P in "$@"; do
  if [ "$P" = 0 ]; then X=NPB3NONE=1; else X=NPB3PAD=$(head -c $P /dev/zero | tr '\0' 'x'); fi
  HASHEVERY=30 TMO=300 tools/perf/run.sh $V b3pad$P $FR marks MSC_INPUT_SCRIPT=/home/claude/msc/tools/scripts/nav_match60.txt MSC_VIRTUAL_TIME=1 $X | tail -2
done
F=$1
for P in "$@"; do [ "$P" = "$F" ] && continue
  echo "== $V pad $F vs $P"; diff <(grep -a '^\[hash\]' /home/claude/msc/build/np/NPP/runs/$V-b3pad$F/err.log) <(grep -a '^\[hash\]' /home/claude/msc/build/np/NPP/runs/$V-b3pad$P/err.log) | head -6
done
