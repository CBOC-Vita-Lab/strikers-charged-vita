#!/usr/bin/env python3
"""SKIN: compare the PNG frame dumps of two runs (e.g. MSC_SKIN_GPU=0 vs 1).
   picdiff.py <dirA> <dirB> [--sheet <out dir> --frames a,b,c] [--thr N]
Per frame: max per-pixel channel difference, pixels that differ at all, pixels that differ by more than N (default 8) in some channel.
--sheet writes side-by-side images: A | B | |A-B| x8 (for the frames given, else the 8 with the largest differences)."""
import sys, os, re
from PIL import Image
import numpy as np

def main():
    a, b = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]; sheet = None; frames = None; thr = 8
    while rest:
        if rest[0] == '--sheet': sheet = rest[1]; rest = rest[2:]
        elif rest[0] == '--frames': frames = [int(x) for x in rest[1].split(',')]; rest = rest[2:]
        elif rest[0] == '--thr': thr = int(rest[1]); rest = rest[2:]
        else: break
    fa = {int(re.search(r'(\d+)', f).group(1)): f for f in os.listdir(a) if f.endswith('.png')}
    fb = {int(re.search(r'(\d+)', f).group(1)): f for f in os.listdir(b) if f.endswith('.png')}
    common = sorted(set(fa) & set(fb))
    rows = []
    for f in common:
        A = np.asarray(Image.open(os.path.join(a, fa[f])).convert('RGB')).astype(np.int16)
        B = np.asarray(Image.open(os.path.join(b, fb[f])).convert('RGB')).astype(np.int16)
        if A.shape != B.shape: rows.append((f, -1, -1, -1, A.shape[0] * A.shape[1])); continue
        d = np.abs(A - B).max(axis=2)
        rows.append((f, int(d.max()), int((d > 0).sum()), int((d > thr).sum()), d.size))
    print('%7s %6s %10s %10s %8s' % ('frame', 'maxdif', 'differ', 'over%d' % thr, 'pixels'))
    same = 0
    for f, mx, n, nt, tot in rows:
        print('%7d %6d %10d %10d %8d' % (f, mx, n, nt, tot))
        same += n == 0
    print('frames %d, identical %d, max of max %d, total differing pixels %d, over %d: %d' % (len(rows), same, max([r[1] for r in rows] or [0]),
          sum(r[2] for r in rows if r[2] > 0), thr, sum(r[3] for r in rows if r[3] > 0)))
    if sheet:
        os.makedirs(sheet, exist_ok=True)
        pick = frames or [r[0] for r in sorted(rows, key=lambda r: -r[2])[:8]]
        for f in pick:
            if f not in fa or f not in fb: continue
            A = np.asarray(Image.open(os.path.join(a, fa[f])).convert('RGB')).astype(np.int16)
            B = np.asarray(Image.open(os.path.join(b, fb[f])).convert('RGB')).astype(np.int16)
            D = np.clip(np.abs(A - B) * 8, 0, 255)
            out = np.concatenate([A, B, D], axis=1).astype(np.uint8)
            Image.fromarray(out).save(os.path.join(sheet, 'cmp_%06d.png' % f))
        print('sheet:', sheet, pick)

if __name__ == '__main__': main()
