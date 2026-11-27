#!/usr/bin/env python3
"""Decodes the standard JPEG streams that test_thp_jpeg writes (MSC_THP_DUMP_DIR) with libjpeg (Pillow) and compares the
pictures with the software THP decoder's planes written next to them (.y/.u/.v, linear). Proves that the stream the console's JPEG
engine is given is a valid baseline JPEG of the same picture. Usage: check_bridge.py <dump dir>"""
import glob
import os
import sys

from PIL import Image

LIMIT_MEAN = 1.0       # mean absolute difference allowed (rounding of two different IDCTs)
LIMIT_MEAN_C = 2.5     # chroma: libjpeg upsamples, the THP planes are half size, so compare the 2x2 averages


def main(d):
    jpgs = sorted(glob.glob(os.path.join(d, '*.jpg')))
    if not jpgs:
        print('no .jpg files in', d)
        return 1
    bad = 0
    for jpg in jpgs:
        base = jpg[:-4]
        im = Image.open(jpg)
        im.draft('YCbCr', im.size)
        im.load()
        if im.mode != 'YCbCr':
            print(os.path.basename(jpg), 'decoded as', im.mode)
            bad += 1
            continue
        w, h = im.size
        y = im.getchannel(0).tobytes()
        planes = {}
        for ext in 'yuv':
            with open(base + '.' + ext, 'rb') as f:
                planes[ext] = f.read()
        if len(planes['y']) != w * h or len(planes['u']) != w * h // 4:
            print(os.path.basename(jpg), 'plane sizes do not match', w, h)
            bad += 1
            continue
        yd = sum(abs(a - b) for a, b in zip(y, planes['y'])) / (w * h)
        # chroma: average libjpeg's full-size planes over 2x2 and compare with the THP planes
        cb = im.getchannel(1).resize((w // 2, h // 2), Image.BOX)
        cr = im.getchannel(2).resize((w // 2, h // 2), Image.BOX)
        ud = sum(abs(a - b) for a, b in zip(cb.tobytes(), planes['u'])) / (w * h // 4)
        vd = sum(abs(a - b) for a, b in zip(cr.tobytes(), planes['v'])) / (w * h // 4)
        ok = yd <= LIMIT_MEAN and ud <= LIMIT_MEAN_C and vd <= LIMIT_MEAN_C
        print('%-22s %dx%d mean difference Y %.3f Cb %.3f Cr %.3f %s' % (os.path.basename(jpg), w, h, yd, ud, vd, 'ok' if ok else 'DIFFERENT'))
        bad += 0 if ok else 1
    print('check_bridge:', 'FAILED' if bad else 'ok', '(%d streams)' % len(jpgs))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
