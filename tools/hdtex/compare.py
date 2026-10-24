#!/usr/bin/env python3
"""Close-up before/after pictures from a finished pack: for each named texture, the original
(decoded from the disc's .rlt) and the pack's level 0 (decoded from its DXT blocks), both
magnified bilinearly to the same size, as the GPU shows them in a close-up. Also checks every
index entry of the pack (offsets, level sizes, key and original format/size against the disc).

    python3 -E -P tools/hdtex/compare.py <pack> <out.png> [texture name ...]
A texture name is the game's (e.g. mario/mariohead, birdo_bowser/birdo_bowser): an exact name
of the entry (any of its deduplicated names), else a substring of its first name.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_pack  # noqa: E402


def read_pack(path):
    b = open(path, 'rb').read()
    magic, ver, n, io, no, do, size, _ = struct.unpack_from('<4sIIIIIII', b, 0)
    assert magic == b'HDTX' and ver == 1 and size == len(b), 'bad header'
    out = {}
    last = -1
    for i in range(n):
        k, off, sz, w, h, ow, oh, fmt, lv, ofmt, flags, nameoff = struct.unpack_from('<QIIHHHHBBBBI', b, io + 32 * i)
        assert k > last, 'index not sorted'
        last = k
        bs = 16 if fmt == 3 else 8
        total, lw, lh = 0, w, h
        for _ in range(lv):
            total += max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bs
            lw, lh = max(1, lw // 2), max(1, lh // 2)
        assert total == sz and off + sz <= len(b) and off % 64 == 0, 'entry %016x sizes' % k
        name = b[no + nameoff:b.index(b'\0', no + nameoff)].decode()
        out[k] = dict(w=w, h=h, ow=ow, oh=oh, ofmt=ofmt, fmt=fmt, levels=lv, data=b[off:off + max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bs], name=name)
    return out


def all_candidates():
    """Every character texture the build could select ({key: entry}), with no size cap."""
    return build_pack.collect(build_pack.DATA, None, 4, 1 << 16)


def decode_dxt(data, w, h, fmt):
    import quicktex
    import quicktex.s3tc.bc1 as bc1
    import quicktex.s3tc.bc3 as bc3
    if fmt == 1:
        return build_pack.decode_bc1(data, w, h)  # (honours the 1-bit alpha quicktex's decoder ignores)
    else:
        tex = bc3.BC3Texture.from_bytes(data, w, h)
        raw = bc3.BC3Decoder().decode(tex)
    return np.frombuffer(raw.tobytes(), np.uint8).reshape(h, w, 4)


def main():
    pack, outp, wants = sys.argv[1], sys.argv[2], sys.argv[3:]
    entries = read_pack(pack)
    texs = all_candidates()
    unknown = [k for k in entries if k not in texs]
    bad = [k for k in entries if k in texs and (texs[k]['t'].w, texs[k]['t'].h, texs[k]['t'].fmt) != (entries[k]['ow'], entries[k]['oh'], entries[k]['ofmt'])]
    print('%d pack entries checked; %d keys not found among the disc\'s character textures, %d with another original format/size; '
          '%d candidates not in this pack' % (len(entries), len(unknown), len(bad), len(set(texs) - set(entries))))
    import gxdecode
    rows = []
    for want in wants:
        hit = [k for k, e in entries.items() if want in e['name'].split(' ')] or [k for k, e in entries.items() if want in e['name'].split(' ')[0]]
        for k in sorted(hit, key=lambda k: entries[k]['name'])[:1]:
            e = entries[k]
            if True:
                t = texs[k]['t']
                orig = gxdecode.decode(t.fmt, t.w, t.h, t.level0, t.palette, t.palFmt)
                hd = decode_dxt(e['data'], e['w'], e['h'], e['fmt'])
                rows.append(('%s  %dx%d -> %dx%d %s' % (want, t.w, t.h, e['w'], e['h'], 'DXT5' if e['fmt'] == 3 else 'DXT1'), orig, hd))
    if len(rows) == 0:
        sys.exit('no texture matched')
    S = 384
    im = Image.new('RGB', (2 * S + 12, len(rows) * (S + 16) + 16), (24, 24, 24))
    dr = ImageDraw.Draw(im)
    dr.text((4, 2), 'original (bilinear)', fill=(255, 255, 255))
    dr.text((S + 16, 2), 'HD pack level 0 (bilinear)', fill=(255, 255, 255))
    for r, (name, orig, hd) in enumerate(rows):
        sc = S / max(hd.shape[:2])
        size = (int(hd.shape[1] * sc), int(hd.shape[0] * sc))
        y = 16 + r * (S + 16)
        dr.text((4, y), name, fill=(255, 255, 0))
        for c, a in enumerate((orig, hd)):
            p = Image.fromarray(a, 'RGBA').resize(size, Image.BILINEAR)
            bg = Image.new('RGBA', size, (90, 90, 90, 255))
            im.paste(Image.alpha_composite(bg, p).convert('RGB'), (c * (S + 12), y + 12))
    im.save(outp)
    print('wrote', outp, len(rows), 'rows')


if __name__ == '__main__':
    main()
