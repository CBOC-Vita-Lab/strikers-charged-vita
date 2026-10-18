#!/usr/bin/env python3
"""cmpd.py a.txt b.txt [tol] [NAME,..] : compare two result files keyed by (seed, test name)"""
import sys, collections
tol = float(sys.argv[3]) if len(sys.argv) > 3 else 2e-4
only = sys.argv[4].split(',') if len(sys.argv) > 4 else None
def load(p):
    d = {}
    for l in open(p).read().split('\n'):
        f = l.split()
        if len(f) >= 3 and (only is None or f[2] in only): d[(f[1], f[2])] = f
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
bad = collections.Counter(); tot = collections.Counter(); shown = collections.Counter()
for k, fa in sorted(a.items(), key=lambda kv: (kv[0][1], int(kv[0][0]))):
    tot[k[1]] += 1
    fb = b.get(k)
    ok = fb is not None and len(fa) == len(fb)
    if ok:
        for x, y in zip(fa[3:], fb[3:]):
            x, y = float(x), float(y)
            if abs(x - y) > tol * max(1.0, abs(x), abs(y)): ok = False
    if not ok:
        bad[k[1]] += 1
        if shown[k[1]] < 2:
            shown[k[1]] += 1; print('A:', ' '.join(fa)[:400]); print('B:', ' '.join(fb)[:400] if fb else '(missing)')
for k in sorted(tot): print('%-16s %5d cases %4d differ' % (k, tot[k], bad[k]))
print('TOTAL differ', sum(bad.values()), 'of', sum(tot.values()))
