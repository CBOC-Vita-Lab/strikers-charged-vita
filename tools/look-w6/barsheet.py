import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, sheet, hud
P = '/home/claude/work/pics/'; OUT = '/home/claude/s13/w6/look/'
def C(size, f, tag='b60', mode='comb'):
    w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h); r = pl.load('%slook-%s-%s/%s/frame_%s.png' % (P, size, tag, size, f))
    return pl.linear(r) if mode == 'lin' else pl.combined(r, b, inset=1)
for f, box, nm in [('003600', (560, 185, 240, 150), 'pitch-line'), ('003120', (0, 395, 240, 120), 'banner'), ('001300', (500, 40, 240, 150), 'menu-text'), ('002040', (180, 90, 240, 150), 'zoom'), ('002520', (280, 330, 300, 120), 'mega-text')]:
    n = pl.load(P + 'look-r100/960x544/frame_%s.png' % f)
    fl = C('448x314', f)
    items = [('native 960x544', n), ('GC port: 691x392 (271k px) bilinear', C('691x392', f, mode='lin')), ('Charged today, same cost: 622x436 combined (271k px)', C('622x436', f)),
             ('Charged 595x337 MSAA2 combined (cost ~261k)', C('595x337', f, 'msaa2')), ('Charged ladder 0.70: 448x314 combined (141k px, half the GC cost)', fl)]
    if f in hud.BOX and f != '001300': items.append(('ladder 0.70 + native HUD (emulated HUDNATIVE)', hud.hudnative(fl, n, f)))
    sheet.sheet(OUT + 'sheet-bar-%s.png' % nm, items, box, z=2, cols=2)
print('ok')
