#!/usr/bin/env python3
"""Host test of MSC_VGX_PASSDOWN's picture bound: the glow chain's six 2:1 steps drawn the game's way (pass at twice the destination size, four bilinear taps per
pixel, 2x2 mean) against drawn at the destination size (four taps at its pixel centres). Synthetic glow pictures (soft blobs, and hard-edged bars for the worst case);
prints the largest and mean 8-bit difference of the final 320x224 picture per step set. Usage: passdown_error.py [--max LEVELS]   (exit 1 when the soft case exceeds LEVELS)."""
import sys
import numpy as np

def bil(tex, u, v):
    h, w = tex.shape
    x = np.clip(u * w - 0.5, 0, w - 1); y = np.clip(v * h - 0.5, 0, h - 1)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    x1 = np.minimum(x0 + 1, w - 1); y1 = np.minimum(y0 + 1, h - 1)
    fx = x - x0; fy = y - y0
    return (tex[y0, x0] * (1 - fx) + tex[y0, x1] * fx) * (1 - fy) + (tex[y1, x0] * (1 - fx) + tex[y1, x1] * fx) * fy

def q(a):  # 8-bit store
    return np.floor(np.clip(a, 0, 1) * 255 + 0.5) / 255

def step(tex, pw, ph, off, down):
    w = pw // (1 if down else 1)
    dw, dh = pw // 2, ph // 2
    if down:
        ow, oh = dw, dh
    else:
        ow, oh = pw, ph
    u = (np.arange(ow) + 0.5) / ow; v = (np.arange(oh) + 0.5) / oh
    U, V = np.meshgrid(u, v)
    o = off / 640.0
    out = 0.25 * (bil(tex, U - o, V) + bil(tex, U + o, V) + bil(tex, U, V - o) + bil(tex, U, V + o))
    out = q(out)
    if not down:
        out = q(out.reshape(dh, 2, dw, 2).mean(axis=(1, 3)))
    return out

CHAIN = [(320, 224), (160, 112), (80, 56), (160, 112), (320, 224), (640, 448)]  # pass sizes (EFB units)

def chain(src, offs, down_from):
    t = src
    for i, (pw, ph) in enumerate(CHAIN):
        t = step(t, pw, ph, offs, i >= down_from)
    return t

def picture(kind):
    rng = np.random.RandomState(5)
    img = np.zeros((224, 320))
    yy, xx = np.mgrid[0:224, 0:320]
    if kind == 'soft':
        for _ in range(14):
            cx, cy, r = rng.uniform(0, 320), rng.uniform(0, 224), rng.uniform(6, 30)
            img += rng.uniform(0.3, 1) * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * r * r))
    else:
        for _ in range(10):
            x, y = rng.randint(0, 300), rng.randint(0, 200)
            img[y:y + rng.randint(2, 20), x:x + rng.randint(2, 40)] = 1.0
    return q(img)

def main():
    lim = None
    if '--max' in sys.argv: lim = float(sys.argv[sys.argv.index('--max') + 1])
    bad = False
    for kind in ('soft', 'bars'):
        src = picture(kind)
        for offs in (8.75, 17.5, 26.25):
            ref = chain(src, offs, 99)
            for first in (0, 1):
                alt = chain(src, offs, first)
                d = np.abs(ref - alt) * 255
                print('%-4s offset %5.2f px  down from step %d: max %5.2f levels  mean %.3f  pixels over 2: %d' % (kind, offs, first + 1, d.max(), d.mean(), int((d > 2).sum())))
                if kind == 'soft' and first == 1 and lim is not None and d.max() > lim: bad = True
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
