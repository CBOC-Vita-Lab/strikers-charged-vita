#!/bin/bash
# NPP: block profiles of chosen frame windows of one run, then the hot functions of each.   WIN=a-b[+c-d...] (frame numbers), V=variant, NAV=script
#   tools/heavy.sh -t 600 env WIN=3000-3090+8000-8090 tools/perf/job_title.sh
cd /home/claude/msc/npw/NPP2 || exit 1
B=/home/claude/msc/build/np/NPP2
V=${V:-tuned}; WIN=${WIN:-864-866}
last=$(echo "$WIN" | tr '+-' '  ' | awk '{print $NF}')
WINDOWS=$WIN TMO=${TMO:-500} tools/perf/run.sh $V win $((last + 2)) profile MSC_INPUT_SCRIPT=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt} MSC_VIRTUAL_TIME=1 ${EXTRA}
D=$B/runs/$V-win
i=0
for w in $(echo "$WIN" | tr '+' ' '); do
  a=${w%-*}; b=${w#*-}
  echo "=== window $i frames $a-$b"
  python3 -I tools/perf/prof_native.py $D/prof.txt.$i $D/err.log $B/$V/armhf/np-armhf-full $((b - a)) --map $B/$V/armhf/np-armhf-full.map --top ${TOP:-45} | cut -c1-180
  i=$((i + 1))
done
