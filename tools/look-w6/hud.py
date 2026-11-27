import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pl, sheet, numpy as np
P = '/home/claude/work/pics/'; OUT = '/home/claude/s13/w6/look/'
# HUD rectangles by hand, in 960x544 pixels, read off the native frames (a stand-in for the real mask: the host harness has no HUDNATIVE)
BOX = {'003600': [(30, 15, 195, 85), (320, 35, 55, 50), (440, 45, 100, 40), (595, 35, 50, 50), (730, 8, 215, 90)],
       '003120': [(225, 160, 140, 170), (595, 160, 140, 170), (0, 400, 960, 100)],
       '002520': [(290, 275, 390, 140), (430, 420, 110, 60)],
       '002760': [(455, 225, 60, 70), (240, 448, 480, 80)],
       '001300': [(0, 0, 960, 544)]}
def hudnative(up, nat, frame):
    out = up.copy()
    for x, y, w, h in BOX[frame]: out[y:y + h, x:x + w] = nat[y:y + h, x:x + w]
    return out
def make(size, frame, box, name, z=2, cols=3, tag='b60'):
    w, h = [int(v) for v in size.split('x')]; b = pl.sharpen_for(w, h)
    n = pl.load(P + 'look-r100/960x544/frame_%s.png' % frame); r = pl.load('%slook-%s-%s/%s/frame_%s.png' % (P, size, tag, size, frame))
    u = pl.combined(r, b, inset=1)
    items = [('native 960x544', n), ('today: HUD drawn at %dx%d, upscaled' % (w, h), u), ('HUDNATIVE (emulated): HUD at 960x544', hudnative(u, n, frame))]
    sheet.sheet(OUT + name, items, box, z=z, cols=cols); print('wrote', name)
if __name__ == '__main__':
    size = sys.argv[1]
    make(size, '003600', (0, 0, 240, 100), 'hud-%s-match-topleft.png' % size)
    make(size, '003600', (300, 20, 360, 80), 'hud-%s-match-score.png' % size, cols=1)
    make(size, '003120', (200, 150, 280, 120), 'hud-%s-goal-digits.png' % size)
    make(size, '002520', (280, 330, 300, 120), 'hud-%s-mega-text.png' % size)
