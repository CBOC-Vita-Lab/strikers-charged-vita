import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, numpy as np
from table import SC, NAT, P
def agg(size, tag, mode='comb'):
    w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h)
    d = '%slook-%s-%s/%s/' % (P, size, tag, size)
    out = {k: [] for k in ('ssim', 'psnr', 'grad', 'hf', 'os', 'shim')}
    per = {}
    for sc, (f0, f1) in SC.items():
        if not os.path.exists(d + 'frame_%s.png' % f0): return None
        fn = (lambda im: pl.linear(im)) if mode == 'linear' else (lambda im: pl.combined(im, b, inset=0))
        n0 = pl.load(NAT + 'frame_%s.png' % f0); u0 = fn(pl.load(d + 'frame_%s.png' % f0))
        r = dict(ssim=pl.ssim(u0, n0), psnr=pl.psnr(u0, n0), grad=pl.gradE(u0) / pl.gradE(n0), hf=pl.hf_ratio(u0, n0), os=pl.overshoot(u0, n0))
        if f1:
            n1 = pl.load(NAT + 'frame_%s.png' % f1); u1 = fn(pl.load(d + 'frame_%s.png' % f1)); r['shim'] = pl.shimmer(u0, u1, n0, n1)[0] * 255
        for k, v in r.items(): out[k].append(v)
        per[sc] = r
    return {k: float(np.mean(v)) for k, v in out.items()}, per
if __name__ == '__main__':
    pairs = [a.split(':') for a in sys.argv[1:]]
    for size, tag in pairs:
        r = agg(size, tag)
        if r: print('%-8s %-7s px %6d  ssim %.4f psnr %.2f grad %.3f hf %.3f os %.3f shim %.3f' % ((size, tag, int(size.split('x')[0]) * int(size.split('x')[1])) + tuple(r[0][k] for k in ('ssim', 'psnr', 'grad', 'hf', 'os', 'shim'))))
