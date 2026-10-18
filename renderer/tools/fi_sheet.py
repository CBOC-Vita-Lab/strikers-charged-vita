#!/usr/bin/env python3
"""fi_sheet.py <picture-dir> <first-frame> <frames> <out.png> [--crop X,Y,W,H] [--scale S] [--cols N] [--prefix f|frame_]

Contact sheet of consecutive PRESENTED pictures of an interpolated run (tools/fi_replay or
the game with MSC_VGX_INTERP): for every built frame n its in-between picture(s)
(f<n>_i<slot>.png, label "n i") and the frame itself (f<n>.png, label "n"); a frame that was
shown without an in-between has "n =" (the picture stays). With --truth also f<n>_t.png
(leave-one-out reference, label "n T"). Workstream FI.
"""
import sys, os
from PIL import Image, ImageDraw

def main():
    a = sys.argv[1:]
    opts = {'--crop': None, '--scale': '0.5', '--cols': '4', '--prefix': 'f', '--truth': None}
    pos = []
    i = 0
    while i < len(a):
        if a[i] == '--truth': opts['--truth'] = True; i += 1
        elif a[i] in opts: opts[a[i]] = a[i + 1]; i += 2
        else: pos.append(a[i]); i += 1
    d, first, count, out = pos[0], int(pos[1]), int(pos[2]), pos[3]
    crop = [int(v) for v in opts['--crop'].split(',')] if opts['--crop'] else None
    scale = float(opts['--scale']); cols = int(opts['--cols']); pre = opts['--prefix']
    tiles = []
    for n in range(first, first + count):
        base = os.path.join(d, '%s%06d' % (pre, n))
        got = False
        if opts['--truth'] and os.path.exists(base + '_t.png'):
            tiles.append((base + '_t.png', '%d T (real frame)' % n))
        for s in range(3):
            p = '%s_i%d.png' % (base, s)
            if os.path.exists(p): tiles.append((p, '%d i%d (in-between)' % (n, s))); got = True
        if os.path.exists(base + '.png'):
            tiles.append((base + '.png', '%d%s' % (n, '' if got or opts['--truth'] else ' = (no in-between)')))
    if not tiles:
        print('no pictures'); return 1
    ims = []
    for p, label in tiles:
        im = Image.open(p).convert('RGB')
        if crop: im = im.crop((crop[0], crop[1], crop[0] + crop[2], crop[1] + crop[3]))
        if scale != 1.0: im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.LANCZOS)
        ImageDraw.Draw(im).rectangle((0, 0, 8 + 7 * len(label), 13), fill=(0, 0, 0))
        ImageDraw.Draw(im).text((3, 1), label, fill=(255, 255, 0))
        ims.append(im)
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * (w + 2), rows * (h + 2)), (40, 40, 40))
    for k, im in enumerate(ims):
        sheet.paste(im, ((k % cols) * (w + 2), (k // cols) * (h + 2)))
    sheet.save(out)
    print('%s: %d pictures, %dx%d' % (out, len(ims), sheet.width, sheet.height))

if __name__ == '__main__':
    sys.exit(main() or 0)
