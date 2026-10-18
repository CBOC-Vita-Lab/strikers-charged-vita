#!/usr/bin/env python3
"""fi_cmp.py <leave-one-out picture dir> [--heat <out-dir>]

Leave-one-out check of the interpolation (tools/fi_replay -l on a capture of the game at
twice the built rate): for every real frame n that was left out, the mean absolute pixel
difference (0..255, over RGB) between it (f<n>_t.png) and
  I : the in-between picture made from its neighbours (f<n+1>_i0.png),
  A : the neighbour before (f<n-1>.png)  = what a repeated frame would show,
  B : the neighbour after (f<n+1>.png)   = what "shown early" would be.
and the share of pixels that differ by more than 24 in any channel. --heat writes
|I - truth| x4 pictures. Workstream FI.
"""
import sys, os, re
import numpy as np
from PIL import Image

def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int16)

def main():
    d = sys.argv[1]
    heat = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == '--heat' else None
    if heat: os.makedirs(heat, exist_ok=True)
    ts = sorted(int(m.group(1)) for m in (re.match(r'f(\d+)_t\.png$', f) for f in os.listdir(d)) if m)
    rows = []
    for n in ts:
        pt = os.path.join(d, 'f%06d_t.png' % n)
        pi = os.path.join(d, 'f%06d_i0.png' % (n + 1))
        pa = os.path.join(d, 'f%06d.png' % (n - 1))
        pb = os.path.join(d, 'f%06d.png' % (n + 1))
        if not (os.path.exists(pa) and os.path.exists(pb)): continue
        t = load(pt); a = load(pa); b = load(pb)
        i = load(pi) if os.path.exists(pi) else None
        def err(x): 
            dd = np.abs(x - t)
            return dd.mean(), (dd.max(axis=2) > 24).mean() * 100
        ea, eb = err(a), err(b)
        ei = err(i) if i is not None else (float('nan'), float('nan'))
        rows.append((n, ei, ea, eb))
        if heat and i is not None:
            Image.fromarray(np.clip(np.abs(i - t) * 4, 0, 255).astype(np.uint8)).save(os.path.join(heat, 'heat_%06d.png' % n))
    print('frame   in-between      repeat(before)  early(after)    [mean abs diff / % pixels off by > 24]')
    for n, ei, ea, eb in rows:
        print('%6d  %5.2f / %5.2f   %5.2f / %5.2f   %5.2f / %5.2f' % (n, ei[0], ei[1], ea[0], ea[1], eb[0], eb[1]))
    if rows:
        import math
        ok = [r for r in rows if not math.isnan(r[1][0])]
        m = lambda k, j: sum(r[k][j] for r in ok) / max(1, len(ok))
        print('mean    %5.2f / %5.2f   %5.2f / %5.2f   %5.2f / %5.2f   (%d frames with an in-between of %d)' % (m(1, 0), m(1, 1), m(2, 0), m(2, 1), m(3, 0), m(3, 1), len(ok), len(rows)))

if __name__ == '__main__':
    main()
