#!/usr/bin/env python3
"""Before/after contact sheet: ux_pairs.py <before dir> <after dir> <out.png>. Both dirs hold <case>/pics/<shot>.png (or <case>-<shot>.png flat).
Only shots whose picture changed by more than 0.8 % of the pixels are shown, as BEFORE | AFTER at half size with the case and shot name; shots that exist on one side only are marked NEW."""
import sys
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw
b, a, out = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
def shots(d):
    r = {}
    for p in sorted(d.glob("*/pics/*.png")):
        r[p.parts[-3] + "/" + p.stem] = p
    return r
B, A = shots(b), shots(a)
rows = []
SKIP = ("01-main-menu", "05-back", "12-host-cancelled", "21-join-after", "31-after-ok", "44-hub-after")
for k in sorted(A):
    if k.split("/")[1].startswith(SKIP):
        continue  # the main menu again: nothing to compare
    if k in B:
        x, y = Image.open(B[k]).convert("RGB"), Image.open(A[k]).convert("RGB")
        if x.size == y.size:
            g = ImageChops.difference(x, y).convert("L").point(lambda v: 255 if v > 40 else 0)
            if sum(g.point(lambda v: 1 if v else 0).tobytes()) < 0.008 * x.size[0] * x.size[1]:
                continue  # unchanged apart from animation (pointer hand, clock) 
        rows.append((k, B[k], A[k]))
    else:
        rows.append((k, None, A[k]))
tw, th, pad, lab = 480, 272, 8, 20
sheet = Image.new("RGB", (2 * tw + 3 * pad, len(rows) * (th + lab + pad) + pad), (24, 24, 28))
d = ImageDraw.Draw(sheet)
for i, (k, pb, pa) in enumerate(rows):
    y = pad + i * (th + lab + pad)
    if pb:
        sheet.paste(Image.open(pb).convert("RGB").resize((tw, th), Image.LANCZOS), (pad, y))
    else:
        d.rectangle([pad, y, pad + tw, y + th], outline=(90, 90, 90)); d.text((pad + 190, y + 130), "NEW SCREEN", fill=(200, 200, 200))
    sheet.paste(Image.open(pa).convert("RGB").resize((tw, th), Image.LANCZOS), (2 * pad + tw, y))
    d.text((pad, y + th + 4), "%s   BEFORE | AFTER" % k, fill=(235, 235, 235))
sheet.save(out)
print("wrote", out, len(rows), "changed pairs")
