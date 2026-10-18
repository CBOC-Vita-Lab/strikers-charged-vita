#!/usr/bin/env python3
"""compare.py a.json b.json [c.json ...] [--names o2,lto,...]: per-scene mean/p95 game-thread M instructions per presented frame of several
runs (report.py --json), scenes matched by label; ratios are against the first file."""
import json, sys
args = [x for x in sys.argv[1:] if not x.startswith('--names')]
names = next((x.split('=', 1)[1].split(',') for x in sys.argv[1:] if x.startswith('--names=')), [a.rsplit('/', 1)[-1] for a in args])
runs = [{r['label']: r for r in json.load(open(a))} for a in args]
print('%-40s' % 'scene' + ''.join('%22s' % n for n in names))
for lab in runs[0]:
    cells = []
    for i, r in enumerate(runs):
        e = r.get(lab)
        if not e: cells.append('%22s' % '-'); continue
        c = '%.2f / %.2f' % (e['mean'] / 1e6, e['p95'] / 1e6)
        if i: c += ' (x%.2f)' % (e['mean'] / runs[0][lab]['mean'])
        cells.append('%22s' % c)
    print('%-40s' % lab[:40] + ''.join(cells))
