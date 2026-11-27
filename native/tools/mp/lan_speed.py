#!/usr/bin/env python3
"""Do the two consoles of a pair run play at the same speed? (lockstep: a session must never run in slow motion on one side only)

Reads the "[mp-lan] net" lines of both consoles of a native_pair.py run folder (peer0, peer1) and compares simulation steps per second of active
match time (50 = full speed). The two must agree: a lockstep match runs at the pace of its slowest console on BOTH. Also prints what result.json says about
sync (compared ticks, mismatches) so one line says whether the run proves anything.

  python3 tools/mp/lan_speed.py RUN_DIR [--tolerance 0.06] [--expect-slow RATIO_MAX] [--expect-full]
exit 0 = equal speeds within the tolerance (and the optional expectations hold); 1 = not; 2 = INVALID (no net lines / too short to say).
"""
import argparse
import json
import re
import sys
from pathlib import Path

NET = re.compile(r"^\[mp-lan\] net( match)?: t=[\d.]+ s active=([\d.]+) s steps=(\d+) ", re.M)


def ratio(text, skip_s=10.0):
    """(median window steps/s / 50, active seconds) of one console, leaving out the first skip_s seconds (the load of the match) and the windows with no
    step at all (a dead link or a finished match is not a speed). The median, not the mean: the two consoles cut their windows at different moments, and
    a slowdown that starts mid-run (MSC_LAN_TEST_SLOWSTEP fromTick) must not make the shares differ because one console logged a window more of the fast part."""
    rows = [(float(m.group(2)), int(m.group(3))) for m in NET.finditer(text) if not m.group(1)]
    t = 0.0
    use = []
    for active, steps in rows:
        if t >= skip_s and steps > 0 and active > 0:
            use.append(steps / active)
        t += active
    act = sum(a for a, s in rows[len(rows) - len(use):]) if use else 0.0
    if not use:
        return None
    use.sort()
    mid = use[len(use) // 2] if len(use) % 2 else (use[len(use) // 2 - 1] + use[len(use) // 2]) / 2
    return mid / 50.0, act


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--tolerance", type=float, default=0.06)
    ap.add_argument("--expect-slow", type=float, default=None, help="both must be at most this share of full speed (a run that was made slow on purpose)")
    ap.add_argument("--expect-full", action="store_true", help="both must be at least 0.94")
    ap.add_argument("--min-active", type=float, default=20.0)
    a = ap.parse_args()
    run = Path(a.run)
    rs = []
    for i in (0, 1):
        r = ratio((run / ("peer%d" % i) / "runtime.log").read_text(errors="replace"))
        rs.append(r)
    if any(r is None or r[1] < a.min_active for r in rs):
        print("INVALID: a console logged under %.0f s of active match windows: %s" % (a.min_active, rs))
        return 2
    speeds = [r[0] for r in rs]
    res = {}
    try:
        res = json.loads((run / "result.json").read_text())
    except (OSError, ValueError):
        pass
    print("speed host %.3f joiner %.3f of full speed (active %.0f s / %.0f s); sync ticks compared %s, mismatches %s/%s, passed %s" % (
        speeds[0], speeds[1], rs[0][1], rs[1][1], res.get("sync_ticks_compared"), len(res.get("sync_mismatch_within_log", [])),
        len(res.get("sync_mismatch_between_logs", [])), res.get("passed")))
    ok = abs(speeds[0] - speeds[1]) <= a.tolerance
    if not ok:
        print("FAIL: the consoles ran at different speeds (more than %.2f apart)" % a.tolerance)
    if a.expect_slow is not None and not all(s <= a.expect_slow for s in speeds):
        print("INVALID: the run was meant to be slow (<= %.2f) and was not" % a.expect_slow)
        return 2
    if a.expect_full and not all(s >= 0.94 for s in speeds):
        print("FAIL: not at full speed")
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
