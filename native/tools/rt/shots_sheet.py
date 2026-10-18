#!/usr/bin/env python3
"""W5 RT: side-by-side sheet of two runs' screenshots (Vita3K game shots shot_NNNNNN.png, or host PNG dumps frame_NNNNNN.png) and a per-pair
difference figure (mean absolute difference per channel, share of pixels that differ by more than 8). Real-time runs do not show the same game
frame at the same present number: the figures say how alike the scenes are, the sheet is for the eye.

  python3 -I tools/rt/shots_sheet.py <dir A> <dir B> <out.png> [label A] [label B]
"""
import os, sys
from PIL import Image, ImageChops, ImageDraw
import numpy as np


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    a, b, out = sys.argv[1], sys.argv[2], sys.argv[3]
    la = sys.argv[4] if len(sys.argv) > 4 else os.path.basename(a.rstrip('/'))
    lb = sys.argv[5] if len(sys.argv) > 5 else os.path.basename(b.rstrip('/'))
    names = sorted(set(f for f in os.listdir(a) if f.endswith('.png')) & set(f for f in os.listdir(b) if f.endswith('.png')))
    if not names:
        print('no common pictures')
        return 1
    rows = []
    for n in names:
        ia = Image.open(os.path.join(a, n)).convert('RGB')
        ib = Image.open(os.path.join(b, n)).convert('RGB').resize(ia.size)
        d = np.abs(np.asarray(ia, dtype=np.int16) - np.asarray(ib, dtype=np.int16))
        print('%s: mean |diff| %.2f, %.1f %% of pixels differ by more than 8' % (n, d.mean(), 100.0 * (d.max(axis=2) > 8).mean()))
        rows.append((n, ia, ib))
    w, h = rows[0][1].size
    sheet = Image.new('RGB', (2 * w + 30, len(rows) * (h + 18) + 18), (20, 20, 20))
    dr = ImageDraw.Draw(sheet)
    dr.text((10, 2), la, fill=(255, 255, 255))
    dr.text((w + 20, 2), lb, fill=(255, 255, 255))
    for i, (n, ia, ib) in enumerate(rows):
        y = 18 + i * (h + 18)
        dr.text((10, y), n, fill=(200, 200, 200))
        sheet.paste(ia, (10, y + 14))
        sheet.paste(ib, (w + 20, y + 14))
    sheet.save(out)
    print('sheet: %s (%d pairs)' % (out, len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
