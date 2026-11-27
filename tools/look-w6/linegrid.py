import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, numpy as np
from linec import lc
from table import SC, NAT, P
size = sys.argv[1]; w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h)
scenes = ['wide', 'zoom', 'menu', 'goal', 'mega', 'closeup']
data = {sc: (pl.load(NAT + 'frame_%s.png' % SC[sc][0]), pl.load('%slook-%s-b60/%s/frame_%s.png' % (P, size, size, SC[sc][0]))) for sc in scenes}
base = lc(data['wide'][0])
def ev(fn):
    s = []; g = []; o = []
    for sc in scenes:
        n, r = data[sc]; u = fn(r); s.append(pl.ssim(u, n)); g.append(pl.gradE(u) / pl.gradE(n)); o.append(pl.overshoot(u, n))
    return np.mean(s), np.mean(g), np.mean(o), lc(fn(data['wide'][1])) / base
print(size, 'base k %.2f: ssim grad overshoot linecontrast' % b)
print('bilinear      | %.4f %.3f %.3f %.2f' % ev(pl.linear))
for E, O, m in [(8, .3, 1.0), (8, .3, 1.4), (16, 1.0, 1.0), (16, 1.0, 1.4), (16, 1.4, 1.4), (24, 1.6, 1.4), (24, 2.2, 1.4), (12, 1.0, 1.4)]:
    print('E%-3s O%-4s x%.1f | %.4f %.3f %.3f %.2f' % ((E, O, m) + ev(lambda im: pl.combined(im, m * b, E=E, O=O)))); sys.stdout.flush()
