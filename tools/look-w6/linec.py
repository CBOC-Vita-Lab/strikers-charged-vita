import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, numpy as np
from scipy.ndimage import gaussian_filter
P = '/home/claude/work/pics/'
def lc(a, box=(540, 190, 200, 120)):
    x, y, w, h = box; l = pl.luma(a)
    e = (l - gaussian_filter(l, 6))[y:y + h, x:x + w]
    return float(np.percentile(e, 99.5))
n = pl.load(P + 'look-r100/960x544/frame_003600.png'); base = lc(n)
print('pitch-line contrast (99.5th percentile of luma minus its sigma-6 blur) in the centre-circle area of frame 3600; native = %.4f' % base)
for size, tag in [('826x468', 'b60'), ('749x424', 'b60'), ('691x392', 'b60'), ('622x436', 'b60'), ('595x337', 'b60'), ('595x337', 'msaa2'), ('499x283', 'b60'), ('442x250', 'b60')]:
    f = '%slook-%s-%s/%s/frame_003600.png' % (P, size, tag, size)
    if not os.path.exists(f): continue
    w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h); r = pl.load(f)
    print('%-8s %-6s bilinear %.2f  combined x0.6 %.2f  x1.0 %.2f  x1.4 %.2f  noedge x.6 %.2f   (fraction of native)' % (size, tag, lc(pl.linear(r)) / base, lc(pl.combined(r, .6 * b)) / base,
          lc(pl.combined(r, b)) / base, lc(pl.combined(r, 1.4 * b)) / base, lc(pl.combined(r, .6 * b, edge=False)) / base))
