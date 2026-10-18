#!/usr/bin/env python3
"""imgcmp.py: compare two directories of dumped frames (frame_NNNNNN.png) from the native game and the recompiled build.

  imgcmp.py NATIVE_DIR RECOMP_DIR [--map auto|same|OFFSET] [--thr 8] [--out DIR] [--sheet N] [--only a-b]

--map same     frame n of NATIVE against frame n of RECOMP
--map OFFSET   native n against recomp n + OFFSET
--map auto     (default) for every native frame the recomp frame (any number) with the smallest difference, searched in
               a window of +-WIN frames around the previous match (scenes appear at slightly different frame numbers;
               movies and fades are time based). Reports the best pair, the difference measures and flags frames whose
               best match is still bad.

Measures: rmse (0..255, RGB), pct (percent of pixels with any channel differing by more than --thr), mx (max channel difference).
With --out a side-by-side picture (native | recomp | amplified difference) is written for every flagged pair.
Only reads PNGs: 8-bit RGB(A), same size (a differing size is reported and skipped).
"""
import argparse, os, re, sys
import numpy as np
from PIL import Image


def load(path):
    im = Image.open(path).convert('RGB')
    return np.asarray(im, dtype=np.int16)


def frames(d):
    out = {}
    for f in os.listdir(d):
        m = re.fullmatch(r'frame_(\d+)\.png', f)
        if m:
            out[int(m.group(1))] = os.path.join(d, f)
    return out


def measure(a, b, thr):
    d = np.abs(a - b)
    mx = int(d.max())
    rmse = float(np.sqrt(np.mean((a - b).astype(np.float64) ** 2)))
    pct = float(np.mean(d.max(axis=2) > thr) * 100.0)
    return rmse, pct, mx


def side(a, b, path):
    d = np.clip(np.abs(a - b) * 4, 0, 255)
    img = np.concatenate([a, b, d], axis=1).astype(np.uint8)
    Image.fromarray(img).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('native')
    ap.add_argument('recomp')
    ap.add_argument('--map', default='auto')
    ap.add_argument('--thr', type=int, default=8)
    ap.add_argument('--win', type=int, default=400)
    ap.add_argument('--out')
    ap.add_argument('--flag-rmse', type=float, default=3.0)
    ap.add_argument('--only')
    args = ap.parse_args()
    nat, rec = frames(args.native), frames(args.recomp)
    if not nat or not rec:
        print('no frames', len(nat), len(rec)); return 1
    lo, hi = 0, 1 << 30
    if args.only:
        a, b = args.only.split('-'); lo, hi = int(a), int(b)
    rkeys = sorted(rec)
    rcache = {}

    def rload(k):
        if k not in rcache:
            if len(rcache) > 64: rcache.clear()
            rcache[k] = load(rec[k])
        return rcache[k]

    last = rkeys[0]
    flagged = 0
    print('native recomp   rmse    pct   max')
    for n in sorted(nat):
        if n < lo or n > hi: continue
        a = load(nat[n])
        if args.map == 'same':
            cand = [n] if n in rec else []
        elif args.map != 'auto':
            cand = [n + int(args.map)] if (n + int(args.map)) in rec else []
        else:
            cand = [k for k in rkeys if abs(k - last) <= args.win] or rkeys
        best = None
        for k in cand:
            if k not in rec: continue
            b = rload(k)
            if b.shape != a.shape:
                print(n, k, 'size differs', a.shape, b.shape); continue
            # cheap pre-filter on a 16x subsample, exact only for the best few
            r = measure(a[::4, ::4], b[::4, ::4], args.thr)
            if best is None or r[0] < best[0][0]:
                best = (r, k)
        if best is None:
            print(n, '-', 'no recomp frame'); continue
        k = best[1]
        b = rload(k)
        rmse, pct, mx = measure(a, b, args.thr)
        last = k
        flag = rmse > args.flag_rmse
        flagged += flag
        print('%6d %6d  %6.2f %6.2f %4d %s' % (n, k, rmse, pct, mx, 'FLAG' if flag else ''))
        if flag and args.out:
            os.makedirs(args.out, exist_ok=True)
            side(a, b, os.path.join(args.out, 'cmp_%06d_%06d.png' % (n, k)))
    print('flagged:', flagged)
    return 0


if __name__ == '__main__':
    sys.exit(main())
