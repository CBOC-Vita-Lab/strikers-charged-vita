#!/usr/bin/env python3
"""W5-ITEM: contact sheets of the item pictures.
   sheet.py <runs dir> <out dir> <scenario>... [--frames frames.json]
For each scenario: <out>/<scenario>.jpg = one row per dumped moment: C (Vita defaults on) | D (rendering defaults off, same camera, same match) |
|C-D| x8, half size, with the frame number and the phase; and the numbers (pixels that differ, > 8, max) in <out>/<scenario>.txt.
<out>/overview-<first>..<last>.jpg: the C pictures of every scenario, one row each (before / use / effect / later of the open-play phase and the keeper phase)."""
import json, os, re, sys
from PIL import Image, ImageDraw
import numpy as np


def load(d, f):
    p = os.path.join(d, 'frame_%06d.png' % f)
    if not os.path.exists(p):
        cands = [x for x in os.listdir(d) if x.endswith('.png') and int(re.search(r'(\d+)', x).group(1)) == f] if os.path.isdir(d) else []
        if not cands:
            return None
        p = os.path.join(d, cands[0])
    return Image.open(p).convert('RGB')


def frames_of(d):
    if not os.path.isdir(d):
        return []
    return sorted(int(re.search(r'(\d+)', x).group(1)) for x in os.listdir(d) if x.endswith('.png'))


def label(img, text):
    dr = ImageDraw.Draw(img)
    dr.rectangle([0, 0, 8 + 7 * len(text), 16], fill=(0, 0, 0))
    dr.text((4, 2), text, fill=(255, 255, 0))
    return img


def main():
    args = sys.argv[1:]
    fj = None
    if '--frames' in args:
        i = args.index('--frames')
        fj = json.load(open(args[i + 1]))
        args = args[:i] + args[i + 2:]
    runs, out = args[0], args[1]
    names = args[2:]
    os.makedirs(out, exist_ok=True)
    W, H = 480, 272
    overview = []
    for n in names:
        dc, dd = os.path.join(runs, n + '_Cg'), os.path.join(runs, n + '_Dg')
        fr = frames_of(dc)
        rows, txt = [], []
        for f in fr:
            c, d = load(dc, f), load(dd, f)
            if c is None:
                continue
            row = Image.new('RGB', (W * 3, H), (40, 40, 40))
            row.paste(label(c.resize((W, H)), '%s f=%d C on' % (n, f)), (0, 0))
            if d is not None and d.size == c.size:
                A = np.asarray(c).astype(np.int16)
                B = np.asarray(d).astype(np.int16)
                diff = np.abs(A - B).max(axis=2)
                txt.append('f=%d differ %d >8 %d max %d' % (f, int((diff > 0).sum()), int((diff > 8).sum()), int(diff.max())))
                row.paste(label(d.resize((W, H)), 'D off'), (W, 0))
                dimg = Image.fromarray(np.clip(diff * 8, 0, 255).astype(np.uint8)).convert('RGB').resize((W, H))
                row.paste(label(dimg, '|C-D| x8'), (W * 2, 0))
            else:
                txt.append('f=%d D missing' % f)
            rows.append(row)
            overview.append((n, f, c))
        if rows:
            sheet = Image.new('RGB', (W * 3, H * len(rows)))
            for i, r in enumerate(rows):
                sheet.paste(r, (0, i * H))
            sheet.save(os.path.join(out, n + '.jpg'), quality=88)
        with open(os.path.join(out, n + '.txt'), 'w') as f:
            f.write('\n'.join(txt) + '\n')
        print('%s: %d frames; %s' % (n, len(rows), '; '.join(t for t in txt if ' >8 0 ' not in t)[:300] or 'all within 8 levels'))
    # overview: up to 6 C pictures per scenario, quarter size
    by = {}
    for n, f, c in overview:
        by.setdefault(n, []).append((f, c))
    w, h = 320, 181
    cols = 6
    ov = Image.new('RGB', (w * cols, h * len(by)), (30, 30, 30))
    for i, (n, L) in enumerate(sorted(by.items())):
        pick = L if len(L) <= cols else [L[int(k * (len(L) - 1) / (cols - 1))] for k in range(cols)]
        for j, (f, c) in enumerate(pick):
            ov.paste(label(c.resize((w, h)), '%s %d' % (n, f)), (j * w, i * h))
    if by:
        ov.save(os.path.join(out, 'overview-%s.jpg' % (names[0] if len(names) == 1 else '%s..%s' % (names[0], names[-1]))), quality=85)


if __name__ == '__main__':
    main()
