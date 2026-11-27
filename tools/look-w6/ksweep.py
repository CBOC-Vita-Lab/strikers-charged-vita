import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ev, pl, numpy as np
size, tag = sys.argv[1:3]
w0, h0 = [int(v) for v in size.split('x')]; base = pl.sharpen_for(w0, h0)
ks = [float(v) * base for v in sys.argv[3].split(',')]
modes = [('linear', None, True)] + [('combined', k, True) for k in ks] + [('combined-noedge', k, False) for k in ks[1:]]
print('size', size, tag, 'auto k = %.3f' % pl.sharpen_for(*[int(v) for v in size.split('x')]))
for name, k, edge in modes:
    mode = 'linear' if name == 'linear' else 'combined'
    R = ev.run(size, tag, mode, kfix=k if k is not None else 0, edge=edge)
    def m(key, scenes=None): 
        v = [r[key] for s, r in R.items() if key in r and (scenes is None or s in scenes)]
        return float(np.mean(v)) if v else float('nan')
    print('%-16s k=%-5s psnr %.2f ssim %.4f grad %.3f hf %.3f | wide ssim %.4f grad %.3f | menu/goal ssim %.4f grad %.3f | shim %.3f' % (name, k, m('psnr'), m('ssim'), m('grad'), m('hf'),
          R['wide']['ssim'], R['wide']['grad'], np.mean([R[s]['ssim'] for s in ('menu', 'goal')]), np.mean([R[s]['grad'] for s in ('menu', 'goal')]), m('shim_abs')))
