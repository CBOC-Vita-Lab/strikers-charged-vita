#!/usr/bin/env python3
"""Sum the per-player statistics of the LAST [watch] line of one or more harness logs per team.
usage: cmp.py label=log [label=log ...]  -> a table, one column per run, rows team0/team1 x statistic"""
import re, sys, collections
def parse(path):
    last = None
    for line in open(path, errors="replace"):
        if line.startswith("[watch]") and "st0_0_" in line: last = line
    if not last: return None
    tot = collections.OrderedDict()
    for m in re.finditer(r"st(\d)_(\d)_(\w+)=(-?\d+)", last):
        t, p, name, v = m.groups()
        tot[(int(t), name)] = tot.get((int(t), name), 0) + int(v)
    sc = re.search(r"score=(\d+)-(\d+)", last)
    return tot, (sc.group(0) if sc else "")
runs = [a.split("=", 1) for a in sys.argv[1:]]
res = [(l, parse(p)) for l, p in runs]
names = []
for l, r in res:
    if r:
        for k in r[0]:
            if k not in names: names.append(k)
print("%-22s" % "stat" + "".join("%14s" % l for l, _ in res))
print("%-22s" % "score" + "".join("%14s" % (r[1] if r else "-") for _, r in res))
for t, n in names:
    print("%-22s" % ("team%d %s" % (t, n)) + "".join("%14s" % (r[0].get((t, n), "-") if r else "-") for _, r in res))
