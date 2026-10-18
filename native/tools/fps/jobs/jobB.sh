#!/bin/bash
# FPS job B (ONE heavy slot, heavy.sh -t 1500): the regression suite on the W5FPS host32 null executable (default MSC_FPS=30 = every scene presented at a steady 30).
R=${R:-/home/claude/msc/build/np/W5FPS}
EXE=$R/host32/np-host32-full-null
cd ${NP:-/home/claude/msc/npw/W5FPS} || exit 1
[ -x $EXE ] || { echo "no $EXE (job A did not build it)"; exit 1; }
echo "== regress $(date +%T) np $(git rev-parse --short HEAD)"
BUILD_ROOT=$R tools/regress.sh -x $EXE -t fps -T "boot kickoff fulltime fulltime30 survey movie det detfresh" 2>&1 | tail -20
echo "== jobB done $(date +%T)"
