#!/bin/bash
# Instruction cost of an armhf variant against the reference (NPP's counting plugin, nav_match60, per scene), e.g. AUTOINIT=zero against the plain build.
#   heavy.sh -t 800 tools/det/cost.sh <refExe> <varExe>
# Output: report.py tables of both runs (game thread, M instructions per presented frame) -- compare the 'match' / open-play rows.
NP=/home/claude/msc/np; B=/home/claude/msc/build/np/NPP; NAV=/home/claude/msc/tools/scripts/nav_match60.txt
for pair in "ref:$1" "var:$2"; do
  tag=${pair%%:*}; exe=${pair#*:}
  rm -rf /home/claude/msc/build/np/B3/data-cost$tag; mkdir -p /home/claude/msc/build/np/B3/data-cost$tag
  EXE=$exe HASHEVERY=0 TMO=300 $NP/tools/perf/run.sh x b3cost$tag 4400 marks MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 NP_DATA_DIR=/home/claude/msc/build/np/B3/data-cost$tag | tail -1
done
for tag in ref var; do echo "=== $tag"; python3 -I $NP/tools/perf/report.py $B/runs/x-b3cost$tag/err.log | cut -c1-150 | head -40; done
