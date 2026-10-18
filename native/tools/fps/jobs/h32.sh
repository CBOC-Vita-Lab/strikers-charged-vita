#!/bin/bash
# The W5 FPS proof turn, as ONE heavy.sh turn:  /home/claude/msc/tools/heavy.sh -t 4800 tools/fps/jobs/h32.sh
#   (then, as a second turn, tools/fps/jobs/jobD.sh -t 3600 for the Vita lab package + Vita3K runs).
# Environment: NP (the worktree to build, default the W5FPS one), GX_VGX (the vgx tree), R (build/output root, default build/np/W5FPS).
# A = host32 null build + virtual-clock proof runs (value order; BUDGET s incl. the build), C = LAN pairs at different rates, B = regression suite (only if time is left).
# Each part skips what is finished, so a cut turn can be repeated. Read the results with tools/fps/analyse.py (see docs/wave5-FPS.md section 6).
HERE=$(cd "$(dirname "$0")" && pwd)
export BUDGET=${BUDGET:-1800} RUNT=${RUNT:-240}
T0=$(date +%s)
echo "##### A $(date +%T)"; bash $HERE/jobA.sh
echo "##### C $(date +%T) (after $(( $(date +%s) - T0 )) s)"; bash $HERE/jobC.sh
if [ $(( $(date +%s) - T0 )) -lt 3000 ]; then
  echo "##### B $(date +%T) (after $(( $(date +%s) - T0 )) s)"; bash $HERE/jobB.sh
else echo "##### B SKIPPED (after $(( $(date +%s) - T0 )) s)"; fi
echo "##### all done $(date +%T) after $(( $(date +%s) - T0 )) s"
