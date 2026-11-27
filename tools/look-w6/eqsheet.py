import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, sheet
P = '/home/claude/work/pics/'; OUT = '/home/claude/s13/w6/look/'
def L(size, tag, f, mode='comb'):
    w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h)
    r = pl.load('%slook-%s-%s/%s/frame_%s.png' % (P, size, tag, size, f))
    return pl.linear(r) if mode == 'linear' else pl.combined(r, b, inset=1)
for f, box, nm in [('002400', (330, 90, 240, 150), 'closeup'), ('002040', (180, 90, 240, 150), 'zoom'), ('003600', (430, 230, 240, 150), 'players'), ('002400', (560, 20, 240, 150), 'closeup2')]:
    n = pl.load(P + 'look-r100/960x544/frame_%s.png' % f)
    items = [('native 960x544 (no AA)', n), ('GC: 691x392 bilinear, no AA', L('691x392', 'b60', f, 'linear')), ('691x392 combined, no AA (270k px)', L('691x392', 'b60', f)),
             ('595x337 MSAA2 combined (~261k px-cost)', L('595x337', 'msaa2', f)), ('595x337 combined, no AA (200k px)', L('595x337', 'b60', f)), ('749x424 combined, no AA (318k px)', L('749x424', 'b60', f))]
    sheet.sheet(OUT + 'sheet-equalcost-%s.png' % nm, items, box, z=2, cols=3)
print('ok')
