"""table.py <tag> <size...>: per-scene metrics of every picture variant for each size -> stdout TSV (size, cfg, scene, psnr, ssim, grad, hf, os, shim)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, numpy as np
P = '/home/claude/work/pics/'
NAT = P + 'look-r100/960x544/'
SC = {'wide': ('003600', '003601'), 'zoom': ('002040', '002041'), 'closeup': ('002400', None), 'menu': ('001300', None), 'sky': ('000720', None),
      'mega': ('002520', None), 'mega2': ('002760', None), 'goal': ('003120', None)}
def cfgs(w, h):
    b = pl.sharpen_for(w, h)
    c = [('gc-bilinear', lambda im: pl.linear(im))]
    for m in (0.6, 1.0, 1.4):
        c.append(('comb-k%.1f' % m, (lambda m: lambda im: pl.combined(im, m * b))(m)))
    c.append(('fast-k1.0', lambda im: pl.fast(im, b)))
    c.append(('noedge-k0.6', lambda im: pl.combined(im, 0.6 * b, edge=False)))
    return c
def main(tag, sizes):
    print('size\tcfg\tscene\tpsnr\tssim\tgrad\thf\tos\tshim')
    for size in sizes:
        w, h = [int(v) for v in size.split('x')]
        d = '%slook-%s-%s/%s/' % (P, size, tag, size)
        for name, fn in cfgs(w, h):
            for sc, (f0, f1) in SC.items():
                if not os.path.exists(d + 'frame_%s.png' % f0): continue
                n0 = pl.load(NAT + 'frame_%s.png' % f0); u0 = fn(pl.load(d + 'frame_%s.png' % f0))
                sh = float('nan')
                if f1:
                    n1 = pl.load(NAT + 'frame_%s.png' % f1); u1 = fn(pl.load(d + 'frame_%s.png' % f1)); sh = pl.shimmer(u0, u1, n0, n1)[0] * 255
                print('%s\t%s\t%s\t%.2f\t%.4f\t%.3f\t%.3f\t%.3f\t%.3f' % (size, name, sc, pl.psnr(u0, n0), pl.ssim(u0, n0), pl.gradE(u0) / pl.gradE(n0), pl.hf_ratio(u0, n0), pl.overshoot(u0, n0), sh))
                sys.stdout.flush()
if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
