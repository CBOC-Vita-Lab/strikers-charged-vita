#!/usr/bin/env python3
"""Contact sheet of the screen pictures: ux_sheet.py <dir with PNGs> <out.png> [columns]. Each tile is the picture at half size with its file name under it."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw
src, out = Path(sys.argv[1]), sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 3
files = sorted(p for p in src.glob("*.png") if not p.name.startswith("sheet"))
if not files:
    raise SystemExit("no pictures in %s" % src)
tw, th, pad, lab = 480, 272, 8, 18
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (tw + pad) + pad, rows * (th + lab + pad) + pad), (24, 24, 28))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB").resize((tw, th), Image.LANCZOS)
    x, y = pad + (i % cols) * (tw + pad), pad + (i // cols) * (th + lab + pad)
    sheet.paste(im, (x, y))
    d.text((x, y + th + 3), f.stem, fill=(230, 230, 230))
sheet.save(out)
print("wrote", out, len(files), "pictures")
