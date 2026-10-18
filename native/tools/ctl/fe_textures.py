#!/usr/bin/env python3
"""W5 CTL: list (and optionally decode to PNG) the textures of the game's front-end bundles, with their name hashes.

  tools/ctl/fe_textures.py <bundle files ...> [--out DIR] [--png]

Bundles: texture bundles (.rlt, magic 'PTLG': BundleHeader + glTexBundleDict entries) and BundleFiles (.res / .dmn: a header of
sector size / file count / directory sector / data sector, then {hash, block, length} entries; a texture entry is a GXTextureHeader
of 0x20 big-endian bytes + GX texel data). Formats (eGXTextureFormat): 0 RGB565, 1 RGB5A3, 2 CMPR, 3 RGBA8, 4 I8, 5 I4, 6 A8 (I8),
7 IA8, 8 CI8 (RGB5A3 palette after the texels). The pictures are the owner's game data: they stay in the build directory (never
committed). Prints one line per texture: bundle, hash, format, size, levels."""
import struct
import sys
import os

try:
    from PIL import Image
except ImportError:  # listing still works
    Image = None


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def rgb565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255)


def rgb5a3(v):
    if v & 0x8000:
        r = (v >> 10) & 31
        g = (v >> 5) & 31
        b = v & 31
        return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255)
    a = (v >> 12) & 7
    r = (v >> 8) & 15
    g = (v >> 4) & 15
    b = v & 15
    return (r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1)


def tex_size(fmt, w, h):
    bw, bh, bpp = {0: (4, 4, 16), 1: (4, 4, 16), 2: (8, 8, 4), 3: (4, 4, 32), 4: (8, 4, 8), 5: (8, 8, 4), 6: (8, 4, 8), 7: (4, 4, 16),
                   8: (8, 4, 8)}[fmt]
    return ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * bpp // 8


def decode(fmt, w, h, d, pal=None):
    px = [(0, 0, 0, 0)] * (w * h)

    def put(x, y, c):
        if x < w and y < h:
            px[y * w + x] = c
    o = 0
    if fmt in (0, 1, 7):
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for y in range(4):
                    for x in range(4):
                        v = struct.unpack_from('>H', d, o)[0]
                        o += 2
                        if fmt == 0:
                            c = rgb565(v)
                        elif fmt == 1:
                            c = rgb5a3(v)
                        else:
                            a, i = v >> 8, v & 255
                            c = (i, i, i, a)
                        put(tx + x, ty + y, c)
    elif fmt == 3:
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                ar = d[o:o + 32]
                gb = d[o + 32:o + 64]
                o += 64
                for i in range(16):
                    put(tx + i % 4, ty + i // 4, (ar[2 * i + 1], gb[2 * i], gb[2 * i + 1], ar[2 * i]))
    elif fmt in (4, 6, 8):
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for y in range(4):
                    for x in range(8):
                        v = d[o]
                        o += 1
                        if fmt == 8:
                            c = pal[v] if pal and v < len(pal) else (255, 0, 255, 255)
                        else:
                            c = (v, v, v, 255) if fmt == 4 else (255, 255, 255, v)
                        put(tx + x, ty + y, c)
    elif fmt == 5:
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for y in range(8):
                    for x in range(0, 8, 2):
                        v = d[o]
                        o += 1
                        for k, n in enumerate((v >> 4, v & 15)):
                            put(tx + x + k, ty + y, (n * 17, n * 17, n * 17, 255))
    elif fmt == 2:
        for ty in range(0, h, 8):
            for tx in range(0, w, 8):
                for sy in range(0, 8, 4):
                    for sx in range(0, 8, 4):
                        c0, c1 = struct.unpack_from('>HH', d, o)
                        bits = be32(d, o + 4)
                        o += 8
                        a = rgb565(c0)
                        b = rgb565(c1)
                        if c0 > c1:
                            cs = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                                  tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
                        else:
                            cs = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                        for i in range(16):
                            idx = (bits >> (30 - 2 * i)) & 3
                            put(tx + sx + i % 4, ty + sy + i // 4, cs[idx])
    return px


def textures(path):
    b = open(path, 'rb').read()
    out = []
    if b[:4] == b'PTLG':
        n = be32(b, 4)
        base = 16 + n * 16
        for i in range(n):
            h, off, size = struct.unpack_from('>III', b, 16 + i * 16)
            out.append((h, base + off, size))
    else:
        sector, n, dsec, _ = struct.unpack_from('>IIII', b, 0)
        for i in range(n):
            h, block, length = struct.unpack_from('>III', b, dsec * sector + i * 12)
            out.append((h, block * sector, length))
    return b, out


def main():
    args = sys.argv[1:]
    out = None
    png = '--png' in args
    maxdim = 4096
    if '--max' in args:
        maxdim = int(args[args.index('--max') + 1])
        args = args[:args.index('--max')] + args[args.index('--max') + 2:]
    if '--out' in args:
        out = args[args.index('--out') + 1]
        args = args[:args.index('--out')] + args[args.index('--out') + 2:]
    args = [a for a in args if a != '--png']
    for path in args:
        b, ents = textures(path)
        for h, off, size in ents:
            if size < 0x20 or off + 0x20 > len(b):
                continue
            levels, fmt = struct.unpack_from('>II', b, off)
            w, hh = struct.unpack_from('>HH', b, off + 0x0E)
            nent = be32(b, off + 0x14)
            if fmt > 8 or not (0 < w <= 2048 and 0 < hh <= 2048) or levels > 12:
                continue
            print('%s %08x fmt=%d %dx%d levels=%d pal=%d bytes=%d' % (os.path.basename(path), h, fmt, w, hh, levels, nent, size))
            if png and out and Image and w <= maxdim and hh <= maxdim:
                tsz = tex_size(fmt, w, hh)
                data = b[off + 0x20: off + 0x20 + tsz]
                pal = None
                if fmt == 8:
                    # the palette follows all the levels' texels
                    total = 0
                    lw, lh = w, hh
                    for _ in range(max(levels, 1)):
                        total += tex_size(fmt, lw, lh)
                        lw, lh = max(lw // 2, 1), max(lh // 2, 1)
                    p = b[off + 0x20 + total: off + 0x20 + total + nent * 2]
                    pal = [rgb5a3(struct.unpack_from('>H', p, 2 * i)[0]) for i in range(len(p) // 2)]
                if len(data) < tsz:
                    continue
                im = Image.new('RGBA', (w, hh))
                im.putdata(decode(fmt, w, hh, data, pal))
                os.makedirs(out, exist_ok=True)
                im.save(os.path.join(out, '%s_%08x_%dx%d_f%d.png' % (os.path.basename(path).split('.')[0], h, w, hh, fmt)))


if __name__ == '__main__':
    main()
