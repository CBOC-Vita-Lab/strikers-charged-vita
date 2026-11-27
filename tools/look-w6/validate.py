import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, numpy as np
P = '/home/claude/work/pics/'
G = P + 'look-gl691/691x392/'; R = P + 'look-691x392-b60/691x392/'
k = 0.62
print('frame  mode  : psnr(sim, GL)  max abs level diff  mean abs level diff   [GL = real present shader on llvmpipe, sim = tools/pl.py]')
for f in ['003600', '002040', '001300', '002760', '003120']:
    raw = pl.load(R + 'frame_%s.png' % f)
    for mode, sim, gl in [('combined', pl.combined(raw, k, inset=1), G + 'frame_%s.png' % f), ('linear', pl.linear(raw), G + 'frame_%s_linear.png' % f),
                          ('fast', pl.fast(raw, k, inset=1), G + 'frame_%s_fast.png' % f), ('smooth', pl.smooth(raw, k), G + 'frame_%s_smooth.png' % f)]:
        g = pl.load(gl); d = np.abs(np.rint(sim * 255) - g * 255)
        print('%s %-9s: %.2f dB  max %d  mean %.3f  (>2 levels: %.3f%%)' % (f, mode, pl.psnr(sim, g), d.max(), d.mean(), (d.max(axis=2) > 2).mean() * 100))
# is the raw render of the validation run the same scene as the plain run? (LOD bias and everything else equal)
