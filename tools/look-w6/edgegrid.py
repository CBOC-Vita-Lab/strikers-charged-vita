import sys, os, itertools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, numpy as np
from table import SC, NAT, P
size = sys.argv[1]; tag = 'b60'
w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h)
scenes = ['wide', 'zoom', 'menu', 'goal', 'mega', 'closeup']
data = {}
for sc in scenes:
    f0 = SC[sc][0]
    data[sc] = (pl.load(NAT + 'frame_%s.png' % f0), pl.load('%slook-%s-%s/%s/frame_%s.png' % (P, size, tag, size, f0)))
print('size', size, 'base k', round(b, 3), 'E O kmult | ssim grad hf overshoot  (mean of %s)' % scenes)
def ev(fn):
    s = []; g = []; hf = []; o = []
    for sc in scenes:
        n, r = data[sc]; u = fn(r)
        s.append(pl.ssim(u, n)); g.append(pl.gradE(u) / pl.gradE(n)); hf.append(pl.hf_ratio(u, n)); o.append(pl.overshoot(u, n))
    return np.mean(s), np.mean(g), np.mean(hf), np.mean(o)
r = ev(pl.linear); print('linear            | %.4f %.3f %.3f %.3f' % r); sys.stdout.flush()
for E, O, m in [(8, 0.3, 1.0), (8, 0.3, 1.4), (6, 0.3, 1.0), (6, 0.3, 1.4), (4, 0.3, 1.0), (4, 0.3, 0.6), (4, 0, 1.0), (4, 0, 1.4), (3, 0, 1.0), (2, 0, 1.0), (2, 0, 0.6), (12, 0, 1.4), (8, 0, 1.4)]:
    r = ev(lambda im: pl.combined(im, m * b, E=E, O=O)); print('E%-3s O%-4s k x%.1f | %.4f %.3f %.3f %.3f' % (E, O, m, *r)); sys.stdout.flush()
