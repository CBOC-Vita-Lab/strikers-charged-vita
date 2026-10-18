#!/usr/bin/env python3
"""Per-scene renderer statistics.

  scene_stats.py <run-log> <vgx_frames.csv> [--from F] [--to F]

Joins the navigation harness log (MSC_WATCH=1: "[nav] f=N scenes: [...]", "match state a -> b",
"overlays: [...]") with the renderer's per-frame CSV (MSC_VGX_FRAMESTATS) and prints, per scene,
the mean and maximum of each per-frame counter.
"""
import csv, re, sys
from collections import OrderedDict, defaultdict

GS = {-1: 'match: loading', 0: 'match: walk-out cutscene', 1: 'match: kick-off', 2: 'match: goal / replay',
      5: 'match: open play', 6: 'match: open play (6)'}

def timeline(logpath):
    """-> sorted list of (frame, label)"""
    ev = []
    scene, gs, ingame, pause, mega = 'boot', None, False, False, False
    def label():
        if ingame:
            l = GS.get(gs, 'match: state %s' % gs)
            if pause: l = 'match: pause menu'
            elif mega and gs == 5: l = 'match: mega strike (defensive_play overlay)'
            return l
        return scene
    for line in open(logpath, errors='replace'):
        if not line.startswith('[nav]'): continue
        m = re.match(r'\[nav\] f=(\d+) (.*)', line)
        if not m: continue
        f, rest = int(m.group(1)), m.group(2)
        if rest.startswith('scenes:'):
            ids = re.findall(r'(\d+):([A-Za-z0-9_]+)', rest)
            names = [n for _, n in ids if n not in ('fe_overlay',)]
            scene = 'fe: ' + (names[-1] if names else '(none)')
        elif rest.startswith('match object created'):
            ingame, gs = True, -1
        elif rest.startswith('match state'):
            mm = re.match(r'match state (-?\d+) -> (-?\d+)', rest)
            gs = int(mm.group(2)); ingame = True
        elif rest.startswith('overlays:'):
            pause = 'pausemenu' in rest
            mega = 'defensive_play' in rest
        else:
            continue
        ev.append((f, label()))
    return ev

def main():
    a = sys.argv[1:]
    lo, hi = 0, 1 << 60
    if '--from' in a: i = a.index('--from'); lo = int(a[i + 1]); del a[i:i + 2]
    if '--to' in a: i = a.index('--to'); hi = int(a[i + 1]); del a[i:i + 2]
    ev = timeline(a[0])
    rows = list(csv.DictReader(open(a[1])))
    cols = [c for c in rows[0].keys() if c not in ('frame', 'hash')]
    agg = OrderedDict()
    k = 0; cur = 'boot'
    for r in rows:
        f = int(r['frame'])
        while k < len(ev) and ev[k][0] <= f: cur = ev[k][1]; k += 1
        if f < lo or f > hi: continue
        d = agg.setdefault(cur, {'n': 0, 'sum': defaultdict(float), 'max': defaultdict(float), 'first': f, 'last': f})
        d['n'] += 1; d['last'] = f
        for c in cols:
            v = float(r[c]); d['sum'][c] += v; d['max'][c] = max(d['max'][c], v)
    show = [('prims', 'GX prims'), ('primsDL', 'of which DL'), ('drawCalls', 'draw calls'), ('verts', 'vertices'), ('tris', 'triangles'),
            ('texBound', 'tex bound'), ('texBoundBytes', 'bound MB'), ('texUploads', 'uploads'), ('texUploadBytes', 'upload KB'),
            ('texLiveBytes', 'live tex MB'), ('copies', 'EFB copies'), ('copyPixels', 'copy kpx'), ('programs', 'programs'),
            ('vtxBytes', 'vtx KB'), ('uniBytes', 'uniform KB')]
    scale = {'texBoundBytes': 1 / 1048576, 'texUploadBytes': 1 / 1024, 'texLiveBytes': 1 / 1048576, 'copyPixels': 1 / 1000, 'vtxBytes': 1 / 1024,
             'uniBytes': 1 / 1024}
    for name, d in agg.items():
        print('== %s  (frames %d..%d, %d frames)' % (name, d['first'], d['last'], d['n']))
        print('   %-14s %12s %12s' % ('per frame', 'mean', 'max'))
        for c, t in show:
            if c not in d['sum']: continue
            s = scale.get(c, 1)
            print('   %-14s %12.1f %12.1f' % (t, d['sum'][c] / d['n'] * s, d['max'][c] * s))
        print('   shaders generated in this scene: %d vertex, %d fragment; programs in use at its end (total so far): %d' %
              (d['sum']['shadersVs'], d['sum']['shadersFs'], d['max']['programsTotal']))

if __name__ == '__main__':
    main()
