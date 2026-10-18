#!/usr/bin/env python3
"""Compare the PNG frame dumps of two runs pixel by pixel.
  cmp_frames.py <dirA> <dirB>
For every frame_*.png present in both: maximum channel difference and number of differing pixels.
Exit code 1 if any pixel differs."""
import sys, os
import numpy as np
from PIL import Image
a, b = sys.argv[1], sys.argv[2]
names = sorted(set(f for f in os.listdir(a) if f.startswith('frame_') and f.endswith('.png')) & set(os.listdir(b)))
bad = 0
for n in names:
    x = np.asarray(Image.open(os.path.join(a, n)).convert('RGB')).astype(int)
    y = np.asarray(Image.open(os.path.join(b, n)).convert('RGB')).astype(int)
    if x.shape != y.shape:
        print('%s: sizes differ %s %s' % (n, x.shape, y.shape)); bad += 1; continue
    d = np.abs(x - y)
    npx = int((d.max(axis=2) > 0).sum())
    if npx:
        bad += 1
        print('%s: %d pixels differ (%.2f%%), max channel difference %d' % (n, npx, 100.0 * npx / (x.shape[0] * x.shape[1]), d.max()))
print('%d frames compared, %d differ' % (len(names), bad))
sys.exit(1 if bad or not names else 0)
