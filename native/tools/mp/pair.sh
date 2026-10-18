#!/bin/bash
# One LAN pair run: tools/mp/pair.sh NAME [native_pair.py options...]   (through heavy.sh; result build/np/NPM2/pairs/NAME/{result.json,peer0,peer1})
#   heavy.sh -t 900 tools/mp/pair.sh p1 --env MSC_LAN_SYNC_LOG=100
cd "$(dirname "$0")/../.."
NAME=$1; shift
BIN=${BIN:-/home/claude/msc/build/np/NPM2/host32/np-host32-full-null}
OUT=/home/claude/msc/build/np/NPM2/pairs/$NAME
rm -rf "$OUT"
python3 tools/mp/native_pair.py --binary $BIN --out $OUT --timeout ${PAIR_TIMEOUT:-840} "$@" > $OUT.json.txt 2>&1
rc=$?
python3 - "$OUT" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]+"/result.json"))
print("passed=%s exit=%s sync_compared=%d within=%s between=%s zero_crc=%d" % (r["passed"], r["exit_codes"], r["sync_ticks_compared"], r["sync_mismatch_within_log"], r["sync_mismatch_between_logs"], r["sync_zero_crc_lines"]))
for i,p in enumerate(r["peers"]):
    print("peer%d: lan_on=%s desync=%d sync_err=%d overflow=%d lost=%d malformed=%d timeout=%s crash=%s sync_lines=%d last_tick=%d scores=%s" % (i,p["lan_on"],len(p["desync"]),p["sync_error"],p["overflow"],p["connection_lost"],p["malformed"],p["script_timeout"],p["crash"],p["sync_lines"],p["last_sync_tick"],p["score_events"][-3:]))
    print("   script:", p["script_logs"][-6:])
PY
echo "pair rc=$rc"
