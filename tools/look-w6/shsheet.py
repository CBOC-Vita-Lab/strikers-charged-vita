import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, sheet
P = '/home/claude/work/pics/'; OUT = '/home/claude/s13/w6/look/'
def L(size, f, tag='b60'):
    w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h)
    return pl.combined(pl.load('%slook-%s-%s/%s/frame_%s.png' % (P, size, tag, size, f)), b, inset=1)
pairs = [('691x392', '622x436', '691'), ('595x337', '535x375', '595'), ('499x283', '448x314', '499')]
for a, b, nm in pairs:
    if not os.path.exists('%slook-%s-b60/%s/frame_003601.png' % (P, b, b)): print('missing', b); continue
    for f, box, k in [('001300', (500, 40, 240, 150), 'menu-text'), ('003600', (560, 185, 240, 150), 'pitch-line'), ('003120', (0, 395, 240, 120), 'banner-text')]:
        n = pl.load(P + 'look-r100/960x544/frame_%s.png' % f)
        sheet.sheet(OUT + 'sheet-shape-%s-%s.png' % (nm, k), [('native 960x544', n), ('16:9 shape %s (SHAPE=1)' % a, L(a, f)), ('game shape %s (today)' % b, L(b, f))], box, z=2, cols=3)
print('ok')
