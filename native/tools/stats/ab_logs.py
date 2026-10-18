#!/usr/bin/env python3
"""A/B of two stats runs of the same seed (tools/stats/run_seed.sh logs): compares every [watch] line (f= frame, sim step, score, ball, goalie
   samples, per-player statistics) except the wall-clock fields (vt, rt, ret).
   tools/stats/ab_logs.py A_PREFIX B_PREFIX seed [seed ...]     e.g.  ab_logs.py runs/main runs/dec 1 2 3   (A_PREFIX_<seed>.log vs B_PREFIX_<seed>.log)
   Prints per seed: IDENTICAL (n watch lines, final score) or the first differing line (frame, fields) and how many lines differ."""
import sys
SKIP = {"vt", "rt", "ret"}
def load(p):
    out = []
    with open(p, errors="replace") as f:
        for line in f:
            if line.startswith("[watch]"):
                d = {}
                for w in line.split()[1:]:
                    k, _, v = w.partition("=")
                    if k not in SKIP:
                        d[k] = v
                out.append(d)
    return out
def main(a):
    if len(a) < 4:
        print(__doc__); return 2
    bad = 0
    for s in a[3:]:
        try:
            x, y = load("%s_%s.log" % (a[1], s)), load("%s_%s.log" % (a[2], s))
        except OSError as e:
            print("seed %s: %s" % (s, e)); bad += 1; continue
        n = min(len(x), len(y)); diff = [i for i in range(n) if x[i] != y[i]]
        fin = x[-1].get("score", "?") if x else "?"
        if not diff and len(x) == len(y):
            print("seed %s: IDENTICAL (%d watch lines, final score %s)" % (s, n, fin))
        else:
            bad += 1
            i = diff[0] if diff else n
            ks = sorted(k for k in (set(x[i]) | set(y[i])) if x[i].get(k) != y[i].get(k)) if diff else []
            print("seed %s: DIFFERENT: %d lines vs %d, %d differ, first at frame %s: %s (A score %s, B score %s)" % (
                s, len(x), len(y), len(diff), x[i].get("f") if i < len(x) else "-", ",".join(ks[:8]), x[-1].get("score") if x else "?", y[-1].get("score") if y else "?"))
    return 1 if bad else 0
sys.exit(main(sys.argv))
