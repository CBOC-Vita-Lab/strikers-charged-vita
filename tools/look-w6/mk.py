import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, sheet
P = '/home/claude/work/pics/'
OUT = '/home/claude/s13/w6/look/'
def variants(size, r, which=None):
    w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h)
    d = [('GC bilinear', pl.linear(r)), ('today comb k%.2f' % b, pl.combined(r, b, inset=1)), ('comb x1.4 k%.2f' % (1.4 * b), pl.combined(r, 1.4 * b, inset=1)),
         ('comb x0.6 k%.2f' % (0.6 * b), pl.combined(r, 0.6 * b, inset=1)), ('fast k%.2f' % b, pl.fast(r, b, inset=1)), ('noedge k%.2f' % (0.6 * b), pl.combined(r, 0.6 * b, edge=False, inset=1))]
    return d if which is None else [x for x in d if any(x[0].startswith(p) for p in which)]
def make(size, frame, box, name, z=2, cols=3, tag='b60', which=None):
    n = pl.load(P + 'look-r100/960x544/frame_%s.png' % frame)
    r = pl.load('%slook-%s-%s/%s/frame_%s.png' % (P, size, tag, size, frame))
    items = [('native 960x544', n)] + variants(size, r, which)
    sheet.sheet(OUT + name, items, box, z=z, cols=cols)
    print('wrote', name)
if __name__ == '__main__':
    size = sys.argv[1]
    S = {'wide-line': ('003600', (560, 185, 240, 150)), 'wide-players': ('003600', (430, 230, 240, 150)), 'zoom': ('002040', (180, 90, 240, 150)), 'closeup': ('002400', (330, 90, 240, 150)),
         'menu-text': ('001300', (500, 40, 240, 150)), 'goal-hud': ('003120', (0, 395, 240, 120))}
    for k, (f, box) in S.items():
        make(size, f, box, 'sheet-%s-%s.png' % (size, k))
