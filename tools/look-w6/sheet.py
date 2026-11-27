"""sheet.py: contact sheets of 1:1 crops (nearest-neighbour zoom z) for several pictures of the same frame."""
import numpy as np
from PIL import Image, ImageDraw
def tile(a, box, z, label, amp=None):
    x, y, w, h = box
    c = a[y:y + h, x:x + w]
    if amp:  # contrast stretch about the crop's mean (banding)
        m = c.mean(axis=(0, 1), keepdims=True); c = np.clip((c - m) * amp + 0.5, 0, 1)
    im = Image.fromarray(np.clip(np.rint(c * 255), 0, 255).astype(np.uint8)).resize((w * z, h * z), Image.NEAREST)
    d = ImageDraw.Draw(im); d.rectangle((0, 0, 8 * len(label) + 6, 14), fill=(0, 0, 0)); d.text((3, 1), label, fill=(255, 255, 0))
    return im
def sheet(path, items, box, z=2, cols=3, amp=None, pad=4):
    tiles = [tile(a, box, z, l, amp) for l, a in items]
    tw, th = tiles[0].size; rows = (len(tiles) + cols - 1) // cols
    S = Image.new('RGB', (cols * (tw + pad) - pad, rows * (th + pad) - pad), (40, 40, 40))
    for i, t in enumerate(tiles): S.paste(t, ((i % cols) * (tw + pad), (i // cols) * (th + pad)))
    S.save(path)
