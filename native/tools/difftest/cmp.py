#!/usr/bin/env python3
import sys, math, collections
a = open(sys.argv[1]).read().split('\n'); b = open(sys.argv[2]).read().split('\n')
tol = float(sys.argv[3]) if len(sys.argv) > 3 else 2e-4
bad = collections.Counter(); tot = collections.Counter(); shown = collections.Counter()
for la, lb in zip(a, b):
    if not la.strip(): continue
    fa, fb = la.split(), lb.split()
    key = fa[0] + ' ' + fa[2]
    tot[key] += 1
    ok = fa[:4] == fb[:4]
    if ok:
        for x, y in zip(fa[4:], fb[4:]):
            x, y = float(x), float(y)
            if abs(x - y) > tol * max(1.0, abs(x), abs(y)): ok = False
    if not ok:
        bad[key] += 1
        if shown[key] < 3: shown[key] += 1; print('DIFF', la, '|', lb)
if len(a) != len(b): print('LENGTH', len(a), len(b))
for k in sorted(tot): print('%-18s %5d cases %4d differ' % (k, tot[k], bad[k]))
print('TOTAL differ', sum(bad.values()), 'of', sum(tot.values()))
