#!/usr/bin/env python3
"""Per-scene means of the GL back end's call census (MSC_VGX_GLSTATS=<csv>).

  gl_stats.py <run-log> <gl.csv> [--from F] [--to F] [--cols a,b,c]

Scenes come from the navigation harness log, as in scene_stats.py. One table: a row per
counter, a column per scene (mean per executed frame); --max adds the maxima.
"""
import csv, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scene_stats import timeline
from collections import OrderedDict, defaultdict

def main():
    a = sys.argv[1:]
    lo, hi, cols, showmax = 0, 1 << 60, None, False
    if '--from' in a: i = a.index('--from'); lo = int(a[i + 1]); del a[i:i + 2]
    if '--to' in a: i = a.index('--to'); hi = int(a[i + 1]); del a[i:i + 2]
    if '--cols' in a: i = a.index('--cols'); cols = a[i + 1].split(','); del a[i:i + 2]
    if '--max' in a: a.remove('--max'); showmax = True
    ev = timeline(a[0])
    rows = list(csv.DictReader(open(a[1])))
    names = [c for c in rows[0].keys() if c != 'frame']
    if cols: names = [c for c in names if c in cols]
    agg = OrderedDict(); k = 0; cur = 'boot'
    for r in rows:
        f = int(r['frame'])
        while k < len(ev) and ev[k][0] <= f: cur = ev[k][1]; k += 1
        if f < lo or f > hi: continue
        d = agg.setdefault(cur, {'n': 0, 'sum': defaultdict(float), 'max': defaultdict(float)})
        d['n'] += 1
        for c in names:
            v = float(r[c]); d['sum'][c] += v; d['max'][c] = max(d['max'][c], v)
    scenes = [s for s in agg if agg[s]['n'] >= 5]
    short = [s.replace('match: ', '').replace('fe: ', '')[:14] for s in scenes]
    print('%-16s' % 'per frame (mean)' + ''.join('%15s' % s for s in short))
    print('%-16s' % 'frames' + ''.join('%15d' % agg[s]['n'] for s in scenes))
    for c in names:
        print('%-16s' % c + ''.join('%15.1f' % (agg[s]['sum'][c] / agg[s]['n']) for s in scenes))
        if showmax: print('%-16s' % '  max' + ''.join('%15.0f' % agg[s]['max'][c] for s in scenes))

if __name__ == '__main__':
    main()
