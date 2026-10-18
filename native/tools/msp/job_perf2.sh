#!/bin/bash
# MSP job (one heavy slot): instructions per presented frame (armhf, qemu-arm + counting plug-in) of nav_match60 at the 30 fps setting, THREE runs of the same binary:
#   pp1  default (frame pacing on, Performance Mode off)       pp0  MSC_FRAME_PACING=0 (what the pacing hooks cost)       pm1f  Performance Mode on
#   tools/heavy.sh -t 900 env FS=nofork tools/msp/job_perf2.sh
NP=$(cd "$(dirname "$0")/../.." && pwd); cd $NP || exit 1
B=${BUILD_ROOT:-/home/claude/msc/build/np/MSP}
NAV=${NAV:-/home/claude/msc/tools/scripts/nav_match60.txt}
run() {  # run <tag> [VAR=value ...]
  local t=$1; shift
  TMO=${RUNT:-330} tools/msp/perf_run.sh $t ${MAXF:-4400} MSC_INPUT_SCRIPT=$NAV MSC_VIRTUAL_TIME=1 MSC_VIRTUAL_FRAME_US=${FRAME_US:-33000} MSC_FRAMESEND=${FS:-nofork} "$@" 2>&1 | tail -3
}
run pp1
run pp0 MSC_FRAME_PACING=0
run pm1f MSC_PERFORMANCE_MODE=1
for t in pp1 pp0 pm1f; do python3 -I tools/perf/report.py $B/runs/$t/err.log --json $B/runs/$t/report.json 2>&1 | cut -c1-170 > $B/runs/$t/scenes.txt; done
for t in pp0 pm1f; do echo "=== $t against pp1"; python3 -I tools/perf/cmp_runs.py $B/runs/pp1 $B/runs/$t --nav | cut -c1-200 | head -4; done
python3 - <<PY
import re
B='$B/runs'
def rows(t):
    out=[]
    for l in open('%s/%s/scenes.txt'%(B,t)):
        m=re.match(r'^(.*?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+\((frames [\d-]+)\)',l)
        if m: out.append((m.group(1).strip(),int(m.group(2)),float(m.group(3)),float(m.group(7)),float(m.group(6)),m.group(9)))
    return out
r={t:rows(t) for t in ('pp1','pp0','pm1f')}
print('%-34s %6s | %-31s | %-31s | %-31s' % ('scene','frames','pp1 pacing on (game / other M)','pp0 pacing off','pm1f Performance Mode'))
for i,x in enumerate(r['pp1']):
    cols=[]
    for t in ('pp1','pp0','pm1f'):
        y=r[t][i] if i<len(r[t]) and r[t][i][0]==x[0] and r[t][i][1]==x[1] else None
        cols.append('%7.3f / %7.3f  p95 %6.3f'%(y[2],y[3],y[4]) if y else 'n/a')
    print('%-34s %6d | %s | %s | %s' % (x[0],x[1],cols[0],cols[1],cols[2]))
PY
