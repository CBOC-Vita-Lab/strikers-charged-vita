#!/usr/bin/env python3
"""Short summary of a native_pair.py result directory."""
import json, sys
from pathlib import Path
d = Path(sys.argv[1])
try:
    r = json.load(open(d / "result.json"))
except Exception as e:
    print("no result.json:", e); sys.exit(1)
print("passed=%s exit=%s sync_ticks_compared=%d mismatch_within=%s mismatch_between=%s zero_crc_lines=%d" % (
    r["passed"], r["exit_codes"], r["sync_ticks_compared"], r["sync_mismatch_within_log"], r["sync_mismatch_between_logs"], r["sync_zero_crc_lines"]))
for i, p in enumerate(r["peers"]):
    print("  peer%d: lan_on=%s desync=%d sync_err=%d overflow=%d lost=%d malformed=%d timeout=%s crash=%s sync_lines=%d last_tick=%d matches=%d" % (
        i, p["lan_on"], len(p["desync"]), p["sync_error"], p["overflow"], p["connection_lost"], p["malformed"], p["script_timeout"], p["crash"], p["sync_lines"], p["last_sync_tick"], p.get("matches_seen", 0)))
    print("     scores: %s" % p["score_events"][-4:])
    print("     script: %s" % p["script_logs"][-5:])
    if p["desync"]:
        print("     DESYNC: %s" % p["desync"][:3])
