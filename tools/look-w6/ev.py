"""ev.py: scores of upscaled low-size renders against the native 960x544 picture. python3 -I ev.py <size> [more sizes]. Writes tsv rows to stdout."""
import sys, os, glob
sys.path.insert(0, os.path.dirname(__file__))
import pl, numpy as np
P = '/home/claude/work/pics'
NAT = P + '/look-r100/960x544'
SCENES = {'wide': ('003600', '003601'), 'zoom': ('002040', '002041'), 'closeup': ('002400', None), 'menu': ('001300', None),
          'sky': ('000720', None), 'mega': ('002520', None), 'mega2': ('002760', None), 'goal': ('003120', None)}
def frames(tag, size, f):
    p = '%s/look-%s-%s/%s/frame_%s.png' % (P, size, tag, size, f)
    return p if os.path.exists(p) else None
def score(U, n):
    return dict(psnr=pl.psnr(U, n), ssim=pl.ssim(U, n), grad=pl.gradE(U) / pl.gradE(n), hf=pl.hf_ratio(U, n))
def run(size, tag, mode, k=None, edge=True, outdir=None, kfix=None):
    w, h = [int(v) for v in size.split('x')]
    kk = pl.sharpen_for(w, h, 1.0 if k is None else k) if kfix is None else kfix
    rows = {}
    for sc, (f0, f1) in SCENES.items():
        p0 = frames(tag, size, f0)
        if not p0: continue
        n0 = pl.load('%s/frame_%s.png' % (NAT, f0))
        def mk(path):
            im = pl.load(path)
            return pl.linear(im) if mode == 'linear' else (pl.combined(im, kk, edge=edge) if mode == 'combined' else pl.fast(im, kk))
        U0 = mk(p0)
        r = score(U0, n0)
        if f1 and frames(tag, size, f1):
            n1 = pl.load('%s/frame_%s.png' % (NAT, f1)); U1 = mk(frames(tag, size, f1))
            sv, sn, fr = pl.shimmer(U0, U1, n0, n1)
            r['shim'] = sv / max(sn, 1e-6) if sn > 1e-6 else float('nan'); r['shim_abs'] = sv * 255
        rows[sc] = r
        if outdir:
            os.makedirs(outdir, exist_ok=True); pl.save(U0, '%s/%s.png' % (outdir, f0))
    return rows
if __name__ == '__main__':
    pass
