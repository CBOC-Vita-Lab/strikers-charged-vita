#!/bin/bash
# FPS job C (ONE heavy slot, heavy.sh -t 1800): LAN pairs of two host32 processes in real time (tools/mp/native_pair.py), the two sides at different presentation rates:
#   p-30-auto  host MSC_FPS=30,  joiner MSC_FPS=auto (60 where its frames hold it: the null back end is light, so it goes to 60 after a second)
#   p-60-30    host MSC_FPS=60,  joiner MSC_FPS=30
# A pair passes when both exit 0 with the scripts finished and the game's lockstep checksums (every 100 ticks) and the per-tick parts agree: the presentation rate does not touch the lockstep.
R=${R:-/home/claude/msc/build/np/W5FPS}
EXE=$R/host32/np-host32-full-null
cd ${NP:-/home/claude/msc/npw/W5FPS} || exit 1
[ -x $EXE ] || { echo "no $EXE (job A did not build it)"; exit 1; }
mkdir -p $R/pairs
T00=$(date +%s)
pair() {  # pair <name> <host MSC_FPS> <joiner MSC_FPS> <port base>
  local name=$1
  [ -f $R/pairs/$name/result.json ] && { echo "$name: done before"; return; }
  [ $(( $(date +%s) - T00 )) -gt ${CBUDGET:-450} ] && { echo "$name: SKIPPED (budget)"; return; }
  rm -rf $R/pairs/$name
  python3 tools/mp/native_pair.py --binary $EXE --out $R/pairs/$name --timeout 620 --port-base $4 --env MSC_FPS_LOG=1 --env MSC_LAN_SYNC_LOG=100 --env0 MSC_FPS=$2 --env1 MSC_FPS=$3 > $R/pairs/$name.json.txt 2>&1
  echo "$name rc=$?"
  python3 - $R/pairs/$name <<'PY'
import json,sys
r=json.load(open(sys.argv[1]+"/result.json"))
print("passed=%s exit=%s sync_compared=%d within=%s between=%s zero_crc=%d parts_compared=%s parts_mismatch=%s" % (r["passed"], r["exit_codes"], r["sync_ticks_compared"], r["sync_mismatch_within_log"], r["sync_mismatch_between_logs"], r["sync_zero_crc_lines"], r.get("parts_compared"), r.get("parts_mismatch")))
for i,p in enumerate(r["peers"]):
    print("peer%d: lan_on=%s desync=%d sync_err=%d overflow=%d lost=%d malformed=%d timeout=%s crash=%s sync_lines=%d last_tick=%d scores=%s" % (i,p["lan_on"],len(p["desync"]),p["sync_error"],p["overflow"],p["connection_lost"],p["malformed"],p["script_timeout"],p["crash"],p["sync_lines"],p["last_sync_tick"],p["score_events"][-3:]))
PY
  for i in 0 1; do grep -a "^\[fps\]" $R/pairs/$name/peer$i/runtime.log | grep -a -E "auto:|summary|MSC_FPS:" | tail -6 | cut -c1-260; done
}
pair p-30-auto 30 auto 43100
pair p-60-30 60 30 43200
echo "== jobC done $(date +%T) after $(( $(date +%s) - T00 )) s"
