#!/usr/bin/env python3
"""Mean / standard deviation of the per-team match statistics over many runs.
usage: summary.py label=glob [label=glob ...]   e.g. summary.py oracle='stats/oracle_*.log' native='stats/native_*.log'"""
import glob, re, sys, math, collections
def parse(path):
    last = None
    for line in open(path, errors="replace"):
        if line.startswith("[watch]") and "st0_0_" in line: last = line
    if not last: return None
    tot = collections.defaultdict(int)
    for m in re.finditer(r"st(\d)_(\d)_(\w+)=(-?\d+)", last):
        t, p, name, v = m.groups()
        tot[(int(t), name)] += int(v)
    sc = re.search(r"score=(\d+)-(\d+)", last)
    if sc:
        tot[(0, "SCORE")] = int(sc.group(1)); tot[(1, "SCORE")] = int(sc.group(2))
    return tot
groups = []
for a in sys.argv[1:]:
    label, pat = a.split("=", 1)
    runs = [r for r in (parse(p) for p in sorted(glob.glob(pat))) if r]
    groups.append((label, runs))
keys = []
for _, runs in groups:
    for r in runs:
        for k in r:
            if k not in keys: keys.append(k)
keys.sort(key=lambda k: (k[0], k[1] != "SCORE", k[1]))
def ms(vals):
    n = len(vals)
    if not n: return "-"
    m = sum(vals) / n
    sd = math.sqrt(sum((v - m) ** 2 for v in vals) / (n - 1)) if n > 1 else 0.0
    return "%.1f +-%.1f" % (m, sd)
print("%-24s" % "stat (mean +- sd)" + "".join("%18s" % ("%s (n=%d)" % (l, len(r))) for l, r in groups))
for k in keys:
    print("%-24s" % ("team%d %s" % k) + "".join("%18s" % ms([r.get(k, 0) for r in runs]) for _, runs in groups))
