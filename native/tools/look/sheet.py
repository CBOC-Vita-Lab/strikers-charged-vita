#!/usr/bin/env python3
"""sheet.py: side-by-side picture sheets and difference numbers for before/after pictures (python3 -I, needs PIL + numpy).

  sheet.py OUT.png [--w W] [--diff] LABEL_A=A.png LABEL_B=B.png [LABEL_C=C.png ...]    one row, pictures scaled to width W (default 480)
  sheet.py OUT.png --rows [--w W] [--diff] A1.png B1.png -- A2.png B2.png                several rows (pairs), '--' separates the rows
  sheet.py --crop X,Y,W,H ...   crop (in pixels of the first picture's size, scaled for the others) before scaling: use it for HUD / text zooms
  --diff adds a third column, the absolute difference of the first two (x4, so that small differences show), and prints rmse / percent of pixels > 8/255.
"""
import sys
from PIL import Image, ImageDraw
import numpy as np

def load(p, size=None, crop=None):
    im = Image.open(p).convert('RGB')
    if crop:
        sx = im.width / float(crop[4]) if len(crop) > 4 else 1.0
        x, y, w, h = crop[:4]
        im = im.crop((int(x * im.width / crop[4]), int(y * im.height / crop[5]), int((x + w) * im.width / crop[4]), int((y + h) * im.height / crop[5]))) if len(crop) > 4 else im.crop((x, y, x + w, y + h))
    return im

def diff(a, b):
    if a.size != b.size: b = b.resize(a.size, Image.BILINEAR)
    A = np.asarray(a).astype(int); B = np.asarray(b).astype(int)
    d = np.abs(A - B)
    rmse = float(np.sqrt(((A - B) ** 2).mean()))
    pct = float((d.max(axis=2) > 8).mean() * 100)
    img = Image.fromarray(np.clip(d * 4, 0, 255).astype('uint8'))
    return img, rmse, pct

def main():
    av = sys.argv[1:]
    out = av.pop(0)
    W = 480; dodiff = False; rows = False; crop = None
    while av and av[0].startswith('--') and av[0] != '--':
        o = av.pop(0)
        if o == '--w': W = int(av.pop(0))
        elif o == '--diff': dodiff = True
        elif o == '--rows': rows = True
        elif o == '--crop': crop = [int(v) for v in av.pop(0).split(',')]
    groups = []
    cur = []
    for a in av:
        if a == '--': groups.append(cur); cur = []
        else: cur.append(a)
    groups.append(cur)
    if not rows: groups = [sum(groups, [])]
    sheet_rows = []
    for g in groups:
        ims = []; labels = []
        for a in g:
            if '=' in a and not a.startswith('/'): l, p = a.split('=', 1)
            else: l, p = a.rsplit('/', 1)[-1], a
            im = load(p)
            if crop:
                x, y, w, h = crop
                # crop is given in the coordinates of a 960x544 picture; scale to this picture
                fx, fy = im.width / 960.0, im.height / 544.0
                im = im.crop((int(x * fx), int(y * fy), int((x + w) * fx), int((y + h) * fy)))
            ims.append(im); labels.append(l)
        if dodiff and len(ims) >= 2:
            di, rmse, pct = diff(ims[0], ims[1])
            print('%s vs %s: rmse %.2f, %.2f %% of pixels differ by more than 8/255' % (labels[0], labels[1], rmse, pct))
            ims.append(di); labels.append('diff x4')
        H = int(W * ims[0].height / ims[0].width)
        row = Image.new('RGB', (W * len(ims) + 4 * (len(ims) - 1), H + 16), (40, 40, 40))
        dr = ImageDraw.Draw(row)
        for i, (im, l) in enumerate(zip(ims, labels)):
            row.paste(im.resize((W, H), Image.LANCZOS if im.width > W else Image.NEAREST if im.width * 2 <= W else Image.BILINEAR), (i * (W + 4), 16))
            dr.text((i * (W + 4) + 2, 2), l, fill=(255, 255, 160))
        sheet_rows.append(row)
    Wt = max(r.width for r in sheet_rows)
    sheet = Image.new('RGB', (Wt, sum(r.height + 2 for r in sheet_rows)), (40, 40, 40))
    y = 0
    for r in sheet_rows:
        sheet.paste(r, (0, y)); y += r.height + 2
    sheet.save(out)
    print('wrote', out, sheet.size)

main()
