#!/bin/bash
# NPP job B (one heavy slot): block profiles of the heaviest scenes of job A's run (same variant, same nav script, virtual time = same frames).
#   tools/heavy.sh -t 700 tools/perf/job_b.sh      env: V=tuned NSCENES=4 WLEN=60 TMO=420
cd /home/claude/msc/npw/NPP2 || exit 1
B=/home/claude/msc/build/np/NPP2
V=${V:-tuned}
J=$B/runs/$V-nav/report.json
[ -f $J ] || { echo "no $J (run job A first)"; exit 1; }
SEL=$(python3 -I - $J ${NSCENES:-4} ${WLEN:-60} <<'PY'
import json, sys
rows = json.load(open(sys.argv[1])); n = int(sys.argv[2]); wl = int(sys.argv[3])
cand = [r for r in rows if r['frames'] >= 20 and r['mean'] >= 3e6 and 'Movie' not in r['label'] and 'Boot' not in r['label']]
# one window per label, in frame order; prefer the heaviest scenes when there are more than n
cand.sort(key=lambda r: -r['mean']); cand = cand[:n]; cand.sort(key=lambda r: r['first'])
def span(r):   # frames costing > 300 M each (a broken scene) get 2 frames: the profile run must stay within its time limit
    n = 2 if r['mean'] > 3e8 else min(wl, r['frames'] - 20)
    o = 1 if r['mean'] > 3e8 else 15
    return '%d-%d' % (r['first'] + o, r['first'] + o + n)
print('+'.join(span(r) for r in cand))
PY
)
echo "windows: $SEL"
[ -n "$SEL" ] || { echo "no scene heavy enough"; exit 0; }
last=$(echo "$SEL" | tr '+-' '  ' | awk '{print $NF}')
WINDOWS=$SEL TMO=${TMO:-420} tools/perf/run.sh $V prof $((last + 5)) profile MSC_INPUT_SCRIPT=${NAV:-/home/claude/msc/tools/scripts/nav_kickoff.txt} MSC_VIRTUAL_TIME=1
D=$B/runs/$V-prof
i=0
for w in $(echo "$SEL" | tr '+' ' '); do
  a=${w%-*}; b=${w#*-}
  label=$(python3 -I - $J $a <<'PY'
import json, sys
rows = json.load(open(sys.argv[1])); a = int(sys.argv[2])
for r in rows:
    if r['first'] <= a <= r['last']: print(r['label']); break
PY
)
  echo "=== window $i frames $a-$b : $label"
  python3 -I tools/perf/prof_native.py $D/prof.txt.$i $D/err.log $B/$V/armhf/np-armhf-full $((b - a)) --map $B/$V/armhf/np-armhf-full.map --top ${TOP:-35} | cut -c1-170
  i=$((i + 1))
done
