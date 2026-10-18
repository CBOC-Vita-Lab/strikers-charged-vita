#!/usr/bin/env python3
"""fi_loo.py <dir> <k> : error of the in-between pictures made by `fi_replay -g k [-x]` against the real frames.
Per position j (1..k-1 frames after A): mean absolute difference (0..255, mean of the three channels over all pixels) and the share of
pixels differing by more than 24 in some channel, for
  interp   f<B>_g<j>.png  against the real frame A+j  (B = A+k)
  repeat   f<A>.png       against the real frame A+j  (what a display of the built frames as they come shows)
  extrap   f<B>_x<j>.png  against the real frame B+j  (only with -x)
  hold     f<B>.png       against the real frame B+j
Frames shown without an in-between (cut, scene change: f<B>_same.txt) are counted apart."""
import sys, os, re
import numpy as np
from PIL import Image
d, k = sys.argv[1], int(sys.argv[2])
files = set(os.listdir(d))
def load(n):
    return np.asarray(Image.open(os.path.join(d, n)).convert('RGB')).astype(np.int16)
def err(a, b):
    x = np.abs(a - b)
    return x.mean(), 100.0 * (x.max(axis=2) > 24).mean()
fed = sorted(int(m.group(1)) for f in files for m in [re.match(r'f(\d+)\.png$', f)] if m)
rows = {'interp': {}, 'repeat': {}, 'extrap': {}, 'hold': {}}
same = 0; n = 0
for B in fed:
    A = B - k
    if A not in fed: continue
    if ('f%06d_same.txt' % B) in files: same += 1; continue
    n += 1
    a = load('f%06d.png' % A); b = load('f%06d.png' % B)
    for j in range(1, k):
        tn = 'f%06d_t.png' % (A + j)
        if tn in files and ('f%06d_g%d.png' % (B, j)) in files:
            t = load(tn)
            rows['interp'].setdefault(j, []).append(err(load('f%06d_g%d.png' % (B, j)), t))
            rows['repeat'].setdefault(j, []).append(err(a, t))
        tn = 'f%06d_t.png' % (B + j)
        if tn in files:
            t = load(tn)
            rows['hold'].setdefault(j, []).append(err(b, t))
            if ('f%06d_x%d.png' % (B, j)) in files:
                rows['extrap'].setdefault(j, []).append(err(load('f%06d_x%d.png' % (B, j)), t))
print('%s: gap %d (built at 1/%d of the captured rate), %d fed pairs measured, %d fed frames shown without in-between (not counted)' % (d, k, k, n, same))
print('%-8s' % 'j/k' + ''.join('%14s' % ('%d/%d' % (j, k)) for j in range(1, k)) + '%14s' % 'mean')
for kind in ('repeat', 'interp', 'hold', 'extrap'):
    r = rows[kind]
    if not r: continue
    cells = []; allv = []
    for j in range(1, k):
        v = r.get(j, [])
        if v:
            m = np.mean([x[0] for x in v]); p = np.mean([x[1] for x in v]); cells.append('%5.2f/%5.1f%%' % (m, p)); allv += v
        else: cells.append('%14s' % '-')
    mm = np.mean([x[0] for x in allv]) if allv else 0; pp = np.mean([x[1] for x in allv]) if allv else 0
    print('%-8s' % kind + ''.join('%14s' % c for c in cells) + '%14s' % ('%5.2f/%5.1f%%' % (mm, pp)))
print('(each cell: mean absolute pixel difference / share of pixels off by more than 24)')
