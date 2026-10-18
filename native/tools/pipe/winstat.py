#!/usr/bin/env python3
"""winstat.py run1/err.log run2/err.log ... [--win a-b ...]: mean game-thread M instructions per presented frame over fixed frame windows
(the same frames of deterministic virtual-time runs), one column per run; ratios against the first run."""
import re, sys, statistics
args = sys.argv[1:]; wins = []; logs = []
i = 0
while i < len(args):
    if args[i] == '--win': wins.append(tuple(int(x) for x in args[i + 1].split('-'))); i += 2
    else: logs.append(args[i]); i += 1
if not wins: wins = [(70, 800), (864, 950), (1485, 1999), (2202, 2370), (2374, 2622), (2626, 2765), (3000, 3600)]
mark = re.compile(r'\[insn\] mark frame=(-?\d+) caller=\d+((?: t\d+=\d+)+)')
def load(p):
    d = {}
    for line in open(p, errors='replace'):
        m = mark.search(line)
        if m and 0 <= int(m.group(1)) < 0x20000000:
            c = [int(x.split('=')[1]) for x in m.group(2).split()]
            d[int(m.group(1))] = c
    return d
runs = [load(p) for p in logs]
names = [p.rstrip("/").split("/")[-2] if "/" in p else p for p in logs]
print('%-14s' % 'frames' + ''.join('%24s' % n for n in names))
for a, b in wins:
    row = '%-14s' % ('%d-%d' % (a, b)); base = None
    for r in runs:
        fs = [f for f in range(a, b + 1) if f in r and f - 1 in r]
        if not fs: row += '%24s' % '-'; continue
        xs = [r[f][0] - r[f - 1][0] for f in fs]; mo = [sum(r[f]) - r[f][0] - sum(r[f - 1]) + r[f - 1][0] for f in fs]
        m = statistics.mean(xs) / 1e6
        cell = '%.3f (oth %.2f)' % (m, statistics.mean(mo) / 1e6)
        if base is None: base = m
        else: cell += ' x%.3f' % (m / base)
        row += '%24s' % cell
    print(row)
