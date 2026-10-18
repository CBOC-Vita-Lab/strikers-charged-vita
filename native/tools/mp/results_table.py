#!/usr/bin/env python3
"""Markdown table of native pair runs: results_table.py build/np/NPM2/pairs [name:description ...] (names in the order given)."""
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
rows = []
for spec in sys.argv[2:]:
    name, _, what = spec.partition(":")
    f = root / name / "result.json"
    if not f.exists():
        rows.append("| %s | %s | no result | | | |" % (name, what)); continue
    r = json.loads(f.read_text())
    ps = r["peers"]
    bad = lambda k: sum((len(p[k]) if isinstance(p[k], list) else p[k]) for p in ps)
    problems = "desync/sync error/overflow/malformed: %d; connection lost: %s" % (
        bad("desync") + bad("sync_error") + bad("overflow") + bad("malformed"), "/".join(str(p["connection_lost"]) for p in ps))
    done = "scripts done" if not any(p["script_timeout"] for p in ps) else "script TIMEOUT"
    verdict = "PASS" if r["passed"] else "FAIL"
    if what.startswith("[control]"):   # a negative control: the failure is the expected outcome
        what = what[len("[control]"):].strip()
        verdict = "control: connection lost on both consoles, as intended" if all(p["connection_lost"] >= 1 for p in ps) else "control FAILED to fail"
    rows.append("| %s | %s | %s | %d lines, last tick %d / %d | exit %s, %s | %s |" % (
        name, what, verdict, r["sync_ticks_compared"], ps[0]["last_sync_tick"], ps[1]["last_sync_tick"], r["exit_codes"], done, problems))
print("| run | what | result | game checksum + seed lines equal on both consoles (2 per logged tick; matches of a series count separately) | exits | game-reported problems |")
print("|---|---|---|---|---|---|")
print("\n".join(rows))
