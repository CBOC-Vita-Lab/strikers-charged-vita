#!/usr/bin/env python3
"""Builds the HD texture pack (hdtex.pack) of the character textures (captains, sidekicks,
goalies and their team-colour variants) from the owner's Mario Strikers Charged disc files.

    python3 -E -P tools/hdtex/build_pack.py [--data game/files]
        [--out hdtex-build] [--model .../realesr-general-x4v3.pth]
        [--scale 2] [--max-side 512] [--only REGEX] [--list]

Reads Art/characters/<name>/*.rlt (all folders except crowd* and npcs) and Art/hashid.bin (the
names of the bundle hashes) in place; never copies game data anywhere else. Writes
<out>/hdtex.pack, <out>/manifest.tsv, <out>/memory.txt and <out>/sheets/*.png (before/after
contact sheets), and caches the network's output in <out>/cache/ (a rerun only re-encodes).
Nothing it writes belongs in git. Pack format and key rule: tools/hdtex/README.md (the same
version 1 as the Galaxy port's).

Per texture: decode the GX level 0 -> fill the colour of fully transparent texels from their
neighbours (so neither the network nor the filtering drags black into the edges) -> Real-ESRGAN
4x (realesr-general-x4v3; alpha upscaled the same way, as grey) -> Lanczos down to the HD size
-> fidelity pass -> mip chain by area averaging down to 1x1 -> BC1 (opaque) or BC3 (any alpha
below 255).
Needs: numpy, Pillow, xxhash, torch (CPU), quicktex.
"""
import argparse
import collections
import os
import re
import struct
import sys
import time

import numpy as np
import xxhash
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gxdecode  # noqa: E402
import rlt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = 'game/files'
OUT = 'hdtex-build'
# GX formats (hardware numbers): RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR. The disc's character
# bundles only hold CMPR and RGB5A3 (no CI8, no intensity formats).
COLOUR_FORMATS = {0x4, 0x5, 0x6, 0x8, 0x9, 0xA, 0xE}
# Texture names (the part after '/', from Art/hashid.bin) that are not the character's skin:
# specular / environment maps and masks (spec, specular, specmask, env, env_spec), the shared
# status overlays (dirt, ice: splats on a transparent field, the same bytes for all characters),
# the flat 16x16 eyegray, and the Mega Strike / stadium / HUD art of extratextures.rlt
# (mega_hand*, mega_cone_colour, mega_gameplay_bg, <name>_banners*, <name>_logo).
EXCLUDE_NAMES = re.compile(r'(?i)(spec|env|mask|noise|^dirt$|_dirt$|^ice$|gr[ae]y|banners|logo|mega_hand|mega_cone|mega_gameplay_bg)')
SKIP_FOLDERS = re.compile(r'(?i)^(crowd|npcs$)')
# The ball, outside Art/characters: (bundle under Art/, names taken from it).
#   objects/gameplay/ball.rlt   gameplay/low_ball: the match ball (art/objects/gameplay/ball, StadiumLoading.cpp)
#   characters/npcs/nis_ball/   nis_ball/low_ball: the ball of the Mega Strike cutscenes (same bytes)
#   transitions/transitions.rlt transitions/ball_hires (same bytes again) and ball_hires_1: the
#                               ball flying across the screen in the scene transitions
#   global.rlt                  global/the_ball: the Mega Strike ball indicators (MegaBallIndicators.cpp)
# Left out: gameplay/charge0 and nis_ball/charge4 (charge glow), the *_spec maps, gameplay/env,
# global/ball_shadow, global/the_ball_exploded (explosion sprite), fe/screens/images/the_ball
# (loading-screen icon, loadingui.res) and fe/screens/images/luigi_ball (HUD icon, ingameui.dmn).
EXTRA_BUNDLES = [
    ('objects/gameplay/ball.rlt', r'^gameplay/low_ball$'),
    ('characters/npcs/nis_ball/nis_ball.rlt', r'^nis_ball/low_ball$'),
    ('transitions/transitions.rlt', r'^transitions/ball_hires(_1)?$'),
    ('global.rlt', r'^global/the_ball$'),
]
MIN_DIM = 16
PACK_DXT1, PACK_DXT5 = 1, 3


def tex_key(fmt, w, h, level0, palette):
    """Must match HdKey() in the runtime (renderer/src/vgx_hdtex.cpp).
    fmt: the GX hardware texture format (rlt.GAME_TO_GX of the header's format)."""
    seed = fmt | (w << 8) | (h << 20)
    k = xxhash.xxh3_64_intdigest(level0, seed=seed)
    if palette:
        k = xxhash.xxh3_64_intdigest(palette, seed=k)
    return k


def texture_names(data_root):
    """{bundle hash: texture name} from Art/hashid.bin (nlStringLowerHash of every name)."""
    path = os.path.join(data_root, 'Art', 'hashid.bin')
    if not os.path.exists(path):
        return {}
    b = open(path, 'rb').read()
    return {rlt.string_lower_hash(s.decode()): s.decode() for s in re.findall(rb'([\x20-\x7e]{2,})\x00', b)}


def bundles(data_root):
    """[(folder, file name, path, name regex or None)] of the character texture bundles and the
    ball's (EXTRA_BUNDLES: only the textures whose name matches)."""
    root = os.path.join(data_root, 'Art', 'characters')
    out = []
    for d in sorted(os.listdir(root)):
        if SKIP_FOLDERS.search(d) or not os.path.isdir(os.path.join(root, d)):
            continue
        for f in sorted(os.listdir(os.path.join(root, d))):
            if f.lower().endswith('.rlt'):
                out.append((d, f, os.path.join(root, d, f), None))
    for rel, rx in EXTRA_BUNDLES:
        path = os.path.join(data_root, 'Art', rel)
        if os.path.exists(path):
            folder, fname = os.path.split(rel)
            out.append((folder or '.', fname, path, re.compile(rx)))
    return out


def hd_size(t, scale, max_side):
    """The replacement's level-0 size: `scale` x the original, halved while the long side is over
    `max_side`; None when that leaves less than 2x (not worth its memory)."""
    s = scale
    while s >= 2 and max(t.w, t.h) * s > max_side:
        s //= 2
    return (t.w * s, t.h * s) if s >= 2 else None


def collect(data_root=DATA, only=None, scale=2, max_side=512, stats=None):
    """{key: dict} of the selected textures, deduplicated by content. `stats` (a dict) receives
    the histograms of all textures read and of the reasons for leaving textures out."""
    names = texture_names(data_root)
    texs = {}
    allfmt = collections.Counter()
    why_out = collections.Counter()
    for folder, fname, path, name_rx in bundles(data_root):
        rel = fname if folder in ('', '.') else '%s/%s' % (folder, fname)
        for t in rlt.read_rlt(open(path, 'rb').read(), rel):
            t.name = names.get(t.bhash, '')
            t.folder = folder
            t.file = fname
            label = '%s %s' % (t.name or '?', t.label)
            why = None
            if name_rx and not name_rx.search(t.name):
                continue  # not the ball (EXTRA_BUNDLES)
            if t.anim:
                why = 'texture animation'
            elif not t.ok:
                why = 'unreadable'
            else:
                allfmt['%s %dx%d' % (rlt.GAME_FMT_NAME[t.gameFmt], t.w, t.h)] += 1
                if t.fmt not in COLOUR_FORMATS:
                    why = 'intensity format'
                elif t.gameFmt == 8 and not t.palette:
                    why = 'CI8 without palette'
                elif t.w < MIN_DIM or t.h < MIN_DIM:
                    why = 'small'
                elif (t.w & (t.w - 1)) or (t.h & (t.h - 1)):
                    why = 'not a power of two'
                elif EXCLUDE_NAMES.search(t.name.split('/')[-1]):
                    why = 'name rule'
                elif only and not re.search(only, label):
                    why = '--only'
                elif hd_size(t, scale, max_side) is None:
                    why = 'over --max-side'
            if why:
                why_out[why] += 1
                continue
            k = tex_key(t.fmt, t.w, t.h, t.level0, t.palette)
            if k in texs:
                texs[k]['names'].append(label)
                texs[k]['files'].append(rel)
                why_out['duplicate'] += 1
                continue
            texs[k] = dict(key=k, names=[label], files=[rel], t=t, hd=hd_size(t, scale, max_side))
    if stats is not None:
        stats['formats'] = allfmt
        stats['excluded'] = why_out
    return texs


def bleed(rgb, valid, iters=64):
    """Gives fully transparent texels the average colour of their valid neighbours, growing outwards."""
    rgb = rgb.copy()
    valid = valid.copy()
    if valid.all() or not valid.any():
        return rgb
    h, w = valid.shape
    for _ in range(iters):
        if valid.all():
            break
        pr = np.pad(rgb * valid[..., None], ((1, 1), (1, 1), (0, 0)), mode='edge')
        pv = np.pad(valid.astype(np.float32), ((1, 1), (1, 1)), mode='edge')
        s = np.zeros_like(rgb)
        c = np.zeros(valid.shape, np.float32)
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                if dy == 1 and dx == 1:
                    continue
                s += pr[dy:dy + h, dx:dx + w]
                c += pv[dy:dy + h, dx:dx + w]
        grow = (~valid) & (c > 0)
        rgb[grow] = s[grow] / c[grow][:, None]
        valid |= grow
    return rgb


def resize_f(a, w, h, flt):
    """Float (H, W, C) array -> (h, w, C) with PIL's filter, channel by channel."""
    return np.stack([np.asarray(Image.fromarray(a[..., c].astype(np.float32), 'F').resize((w, h), flt)) for c in range(a.shape[2])], -1)


def _rgb565_expand(c):
    c = c.astype(np.int32)
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def decode_bc1(data, w, h):
    """BC1 blocks -> RGBA8 (h, w, 4), honouring the 3-colour mode's transparent index (black,
    alpha 0), as the GPU decodes it (quicktex's decoder returns that texel opaque)."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    blk = np.frombuffer(data, np.uint8)[:bw * bh * 8].reshape(bw * bh, 8)
    c0 = blk[:, 0].astype(np.int32) | (blk[:, 1].astype(np.int32) << 8)
    c1 = blk[:, 2].astype(np.int32) | (blk[:, 3].astype(np.int32) << 8)
    p0, p1 = _rgb565_expand(c0), _rgb565_expand(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    a3 = np.where(four[:, 0], 255, 0)
    pal = np.stack([np.concatenate([p, np.full((len(p), 1), 255)], -1) for p in (p0, p1, p2, p3)], 1)
    pal[:, 3, 3] = a3
    sel = np.stack([(blk[:, 4 + y, None] >> (2 * np.arange(4))) & 3 for y in range(4)], 1)  # (n, 4 rows, 4 cols)
    px = pal[np.arange(len(blk))[:, None, None], sel]
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].astype(np.uint8)


def _blocks(a, pw, ph):
    """(ph, pw, C) -> (nblocks, 16, C) in block order (rows of blocks, texels row-major)."""
    return a.reshape(ph // 4, 4, pw // 4, 4, -1).transpose(0, 2, 1, 3, 4).reshape(-1, 16, a.shape[-1])


def encode_bc1_punch(rgba8, pw, ph):
    """BC1 with 1-bit alpha (texels with alpha < 128 transparent). Opaque blocks: quicktex's
    four-colour encoding of the image (transparent texels carry their bled colour, which only
    matters for the blocks around them). Blocks with a transparent texel: endpoints from quicktex's
    three-colour encoding of the block with its transparent texels set to the mean of its opaque
    ones (so they do not pull the endpoints), ordered c0 <= c1 (three-colour mode), then every
    opaque texel takes the nearest of c0, c1 and their midpoint, every transparent one index 3."""
    import quicktex
    import quicktex.s3tc.bc1 as bc1
    raw = quicktex.RawTexture.frombytes(np.ascontiguousarray(rgba8).tobytes(), pw, ph)
    out = np.frombuffer(bc1.BC1Encoder(18).encode(raw).tobytes(), np.uint8).reshape(-1, 8).copy()
    tb = _blocks(rgba8, pw, ph)
    trans = tb[..., 3] < 128
    alpha_blocks = np.nonzero(trans.any(1))[0]
    if len(alpha_blocks) == 0:
        return out.tobytes()
    fill = tb.copy()
    for i in alpha_blocks:
        op = ~trans[i]
        mean = fill[i, op, :3].mean(0) if op.any() else np.zeros(3)
        fill[i, ~op, :3] = (mean + 0.5).astype(np.uint8)
        fill[i, :, 3] = 255
    fimg = fill.reshape(ph // 4, pw // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(ph, pw, 4)
    raw3 = quicktex.RawTexture.frombytes(np.ascontiguousarray(fimg).tobytes(), pw, ph)
    enc3 = np.frombuffer(bc1.BC1Encoder(18, bc1.BC1Encoder.ColorMode.ThreeColor).encode(raw3).tobytes(), np.uint8).reshape(-1, 8)
    for i in alpha_blocks:
        op = ~trans[i]
        c0 = int(enc3[i, 0]) | int(enc3[i, 1]) << 8
        c1 = int(enc3[i, 2]) | int(enc3[i, 3]) << 8
        if not op.any():
            c0 = c1 = 0
        if c0 > c1:
            c0, c1 = c1, c0
        p = _rgb565_expand(np.array([c0, c1]))
        pal = np.stack([p[0], p[1], (p[0] + p[1]) // 2]).astype(np.int32)
        d = ((tb[i, :, None, :3].astype(np.int32) - pal[None]) ** 2).sum(-1)
        sel = np.where(op, d.argmin(1), 3).reshape(4, 4)
        out[i, 0:4] = (c0 & 255, c0 >> 8, c1 & 255, c1 >> 8)
        out[i, 4:8] = (sel[:, 0] | (sel[:, 1] << 2) | (sel[:, 2] << 4) | (sel[:, 3] << 6)).astype(np.uint8)
    return out.tobytes()


def encode(rgba8, w, h, fmt, punch=False):
    """One level -> (blocks, decoded RGBA8). punch: DXT1 with 1-bit alpha (encode_bc1_punch)."""
    import quicktex
    import quicktex.s3tc.bc1 as bc1
    import quicktex.s3tc.bc3 as bc3
    pw, ph = max(4, (w + 3) // 4 * 4), max(4, (h + 3) // 4 * 4)
    if (pw, ph) != (w, h):
        rgba8 = np.pad(rgba8, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    if fmt == PACK_DXT1 and punch:
        blob = encode_bc1_punch(rgba8, pw, ph)
        return blob, decode_bc1(blob, pw, ph)[:h, :w]
    raw = quicktex.RawTexture.frombytes(np.ascontiguousarray(rgba8).tobytes(), pw, ph)
    if fmt == PACK_DXT1:
        tex = bc1.BC1Encoder(18).encode(raw)
        dec = bc1.BC1Decoder().decode(tex)
    else:
        tex = bc3.BC3Encoder(18).encode(raw)
        dec = bc3.BC3Decoder().decode(tex)
    out = np.frombuffer(dec.tobytes(), np.uint8).reshape(ph, pw, 4)[:h, :w]
    return tex.tobytes(), out


def pad_mode(src, axis, wrap):
    """How to pad the borders of one axis for the network (numpy pad mode).
    A repeating texture is padded with its other side only when it really tiles across that
    seam: many textures are flagged repeat but are atlases or gradients whose opposite borders
    differ. Padded with wrap-around, such a texture shows the network a hard step at the seam,
    which it sharpens like any edge: darker on one side, brighter on the other. Otherwise, and
    for mirrored textures, mirror padding (no step); clamped textures repeat their border texels,
    as the sampler does. (Strikers' wrap modes live in the material bindings, not in the texture:
    every texture is treated as repeat, so the seam test decides.)"""
    if wrap == 0:
        return 'edge'
    if wrap == 1:
        a = np.moveaxis(src, axis, 0)
        seam = np.abs(a[0] - a[-1]).mean()
        inner = np.abs(np.diff(a, axis=0)).mean() if a.shape[0] > 1 else 0.0
        if seam <= 2 * inner + 2 / 255:
            return 'wrap'
    return 'symmetric'


FIDELITY_T = 4 / 255  # deviations from the original (area-averaged) up to this are the network's to keep


def fidelity(hd, src, shown, opaque, iters=3):
    """Soft-thresholded back-projection: where the replacement, area-averaged to the original's
    size, differs from the original by more than FIDELITY_T, the excess is spread back over it
    (bilinearly), a few times. Corrects what the network gets wrong at the original's scale (a
    thin line along a border washed out, a uniform brightening of near-white areas, an alpha rim)
    while keeping what it adds below one texel and the CMPR block noise it removes (smaller than
    the threshold). `src`: the original as float RGBA (transparent texels' colour filled in)."""
    h, w = src.shape[:2]
    H, W = hd.shape[:2]
    for _ in range(iters):
        r = src - resize_f(hd, w, h, Image.BOX)
        r[..., :3] *= shown[..., None]
        if opaque:
            r[..., 3] = 0
        r = np.sign(r) * np.maximum(np.abs(r) - FIDELITY_T, 0)
        if not r.any():
            break
        hd = np.clip(hd + resize_f(r, W, H, Image.BILINEAR), 0, 1)
    return hd


def upscale_cached(entry, net, cache_dir):
    """The network's 4x output for one texture (RGB, and alpha when not opaque), run once and
    cached by key and padding: (img, opaque, src, up)."""
    import esrgan
    t = entry['t']
    img = gxdecode.decode(t.fmt, t.w, t.h, t.level0, t.palette, t.palFmt)
    alpha = img[..., 3]
    opaque = bool((alpha == 255).all())
    rgb = img[..., :3].astype(np.float32) / 255
    rgb = bleed(rgb, alpha > 0)
    src = np.concatenate([rgb, (alpha.astype(np.float32) / 255)[..., None]], -1)
    mode = (pad_mode(src, 0, t.wrapT), pad_mode(src, 1, t.wrapS))
    cpath = os.path.join(cache_dir, '%016x-%s%s.npz' % (entry['key'], mode[0][0], mode[1][0]))
    if os.path.exists(cpath):
        z = np.load(cpath)
        up_rgb, up_a = z['rgb'], z['a'] if 'a' in z else None
    else:
        if net is None:
            return None
        up_rgb = esrgan.upscale4(net, rgb, mode=mode)
        up_a = None
        if not opaque:
            a = np.repeat((alpha.astype(np.float32) / 255)[..., None], 3, -1)
            up_a = esrgan.upscale4(net, a, mode=mode).mean(-1, keepdims=True)
        np.savez_compressed(cpath + '.tmp.npz', rgb=up_rgb.astype(np.float16), **({'a': up_a.astype(np.float16)} if up_a is not None else {}))
        os.replace(cpath + '.tmp.npz', cpath)
    up = up_rgb.astype(np.float32) if up_a is None else np.concatenate([up_rgb.astype(np.float32), up_a.astype(np.float32)], -1)
    return img, opaque, src, up


def make_hd(entry, net, cache_dir):
    t = entry['t']
    img, opaque, src, up = upscale_cached(entry, net, cache_dir)
    alpha = img[..., 3]
    W, H = entry['hd']
    hd = np.clip(resize_f(up, W, H, Image.LANCZOS), 0, 1) if (W, H) != (up.shape[1], up.shape[0]) else up
    if opaque:
        hd = np.concatenate([hd, np.ones((H, W, 1), np.float32)], -1)
    hd = fidelity(hd, src, alpha > 0, opaque)
    onoff = not opaque and bool(np.isin(alpha, (0, 255)).all())
    if onoff:
        # The original's alpha is on/off (CMPR): so is the replacement's, with the network's
        # smoother outline (a soft rim would leak into transparent frames around the art).
        hd[..., 3] = (hd[..., 3] >= 0.5).astype(np.float32)
    # Opaque: DXT1. On/off alpha: DXT1 with 1-bit alpha (half of DXT5; every level's alpha
    # thresholded at 0.5 again after the area averaging, so the texels that stay visible take
    # their colour from the bled neighbours, not from black). Soft alpha: DXT5.
    fmt = PACK_DXT5 if not (opaque or onoff) else PACK_DXT1
    levels, decoded = [], []
    lw, lh = W, H
    while True:
        lv = hd if (lw, lh) == (W, H) else resize_f(hd, lw, lh, Image.BOX)
        lv8 = (np.clip(lv, 0, 1) * 255 + 0.5).astype(np.uint8)
        if opaque:
            lv8[..., 3] = 255
        elif onoff:
            lv8[..., 3] = np.where(lv[..., 3] >= 0.5, 255, 0)
        blob, dec = encode(lv8, lw, lh, fmt, punch=onoff)
        levels.append(blob)
        decoded.append((lv8, dec))
        if lw == 1 and lh == 1:
            break
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    src8, dec0 = decoded[0]
    # over what is shown: RGB where the texel is visible, and alpha (DXT1's transparent texels are black)
    d = src8.astype(np.float32) - dec0.astype(np.float32)
    d[..., :3] *= (src8[..., 3:] >= 128)
    mse = np.mean(d ** 2)
    psnr = 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    return dict(fmt=fmt, w=W, h=H, levels=levels, orig=img, hd=dec0, psnr=psnr, opaque=opaque)


def write_pack(path, items):
    """items: [(key, entry, hd)]; layout documented in README.md."""
    items = sorted(items, key=lambda x: x[0])
    names = bytearray()
    name_off = []
    for k, e, hd in items:
        name_off.append(len(names))
        names += ' '.join(e['names']).encode('ascii', 'replace')[:200] + b'\0'
    n = len(items)
    index_off = 32
    names_off = index_off + 32 * n
    data_off = (names_off + len(names) + 63) // 64 * 64
    blobs = bytearray()
    index = bytearray()
    for (k, e, hd), no in zip(items, name_off):
        while (data_off + len(blobs)) % 64:
            blobs.append(0)
        off = data_off + len(blobs)
        data = b''.join(hd['levels'])
        blobs += data
        t = e['t']
        flags = 0 if hd['opaque'] else 1
        index += struct.pack('<QIIHHHHBBBBI', k, off, len(data), hd['w'], hd['h'], t.w, t.h, hd['fmt'], len(hd['levels']), t.fmt, flags, no)
    size = data_off + len(blobs)
    hdr = struct.pack('<4sIIIIIII', b'HDTX', 1, n, index_off, names_off, data_off, size, 0)
    with open(path + '.tmp', 'wb') as f:
        f.write(hdr)
        f.write(index)
        f.write(names)
        f.write(b'\0' * (data_off - names_off - len(names)))
        f.write(blobs)
    os.replace(path + '.tmp', path)
    return size


def sheet(path, rows, S=256):
    """Before (original, nearest-neighbour to the same size) / after (the BC-compressed HD level 0)."""
    cols = 2
    pairs = 3
    nrows = (len(rows) + pairs - 1) // pairs
    im = Image.new('RGB', (pairs * cols * S, nrows * (S + 14)), (32, 32, 32))
    dr = ImageDraw.Draw(im)
    for i, (name, orig, hd) in enumerate(rows):
        x0, y0 = (i % pairs) * cols * S, (i // pairs) * (S + 14)
        dr.text((x0 + 2, y0 + 1), name[:60], fill=(255, 255, 0))
        sc = S / max(hd.shape[:2])
        size = (max(1, int(hd.shape[1] * sc)), max(1, int(hd.shape[0] * sc)))
        for c, a in enumerate((orig, hd)):
            p = Image.fromarray(a, 'RGBA').resize(size, Image.NEAREST if c == 0 else Image.LANCZOS)
            chk = (np.indices((p.size[1], p.size[0])).sum(0) // 8 % 2)[..., None]
            bg = np.where(chk, 150, 110).astype(np.uint8).repeat(3, -1)
            bg = np.concatenate([bg, np.full(bg.shape[:2] + (1,), 255, np.uint8)], -1)
            comp = Image.alpha_composite(Image.fromarray(bg, 'RGBA'), p).convert('RGB')
            im.paste(comp, (x0 + c * S, y0 + 14))
    im.save(path)


# ---- memory estimate -------------------------------------------------------------------------

def gpu_bytes(fmt_dxt5, w, h, clamp=True):
    """Bytes of a DXT1/DXT5 texture with its mip chain as the Vita back end holds it (mipClamp:
    down to the last level of at least 4x4)."""
    bs = 16 if fmt_dxt5 else 8
    total, lw, lh = 0, w, h
    while True:
        total += max(1, (lw + 3) // 4) * max(1, (lh + 3) // 4) * bs
        if (lw == 1 and lh == 1) or (clamp and (lw // 2 < 4 or lh // 2 < 4)):
            break
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    return total


def memory_report(items):
    """Text: HD bytes (as the Vita holds them) per bundle file, per character set, and for a
    typical match. Original bytes: the CMPR originals as the back end holds them (DXT1 with the
    same clamp)."""
    per_file = collections.defaultdict(lambda: [0, 0, 0])  # file -> [hd, orig, count]
    for k, e, hd in items:
        t = e['t']
        hb = gpu_bytes(hd['fmt'] == PACK_DXT5, hd['w'], hd['h'])
        ob = gpu_bytes(False, t.w, t.h) if t.fmt == 0xE else 2 * gpu_bytes(True, t.w, t.h)  # CMPR: DXT1; others: RGBA8
        for f in sorted(set(e['files'])):
            per_file[f][0] += hb
            per_file[f][1] += ob
            per_file[f][2] += 1

    def total(files):
        h = o = n = 0
        for f in files:
            a = per_file.get(f)
            if a:
                h, o, n = h + a[0], o + a[1], n + a[2]
        return h, o, n

    def fmt(x):
        return '%7.2f MiB' % (x / 1048576.0)

    lines = ['HD texture memory (DXT, mip chain down to 4x4 as the Vita back end keeps it).',
             'orig = the same textures as the game uploads them (CMPR -> DXT1, same chain).', '',
             '%-44s %5s %11s %11s' % ('bundle', 'tex', 'HD', 'orig')]
    for f in sorted(per_file):
        h, o, n = per_file[f]
        lines.append('%-44s %5d %s %s' % (f, n, fmt(h), fmt(o)))
    caps = ['mario', 'luigi', 'peach', 'daisy', 'yoshi', 'wario', 'waluigi', 'donkeykong', 'diddykong', 'bowser', 'bowserjr', 'petey']
    lines += ['', 'Captain sets (base: <c>.rlt + extratextures.rlt + mario_shock.rlt; + one variant: <c>_alt.rlt):',
              '%-12s %11s %11s %11s' % ('captain', 'base HD', '+variant HD', 'orig')]
    for c in caps:
        base = ['%s/%s.rlt' % (c, c), '%s/extratextures.rlt' % c, 'mario/mario_shock.rlt']
        h, o, n = total(base)
        ha, oa, na = total(base + ['%s/%s_alt.rlt' % (c, c)])
        lines.append('%-12s %s %s %s' % (c, fmt(h), fmt(ha), fmt(oa)))
    # typical match: Mario vs Bowser, sidekicks Toad (Mario's team) and Koopa (Bowser's team): each
    # sidekick type in its own team's colours; the goalies (shared Kritter base + the two captains'
    # goalie colours). "Both team colours" of a sidekick type: the variant for each captain.
    match = ['mario/mario.rlt', 'mario/extratextures.rlt', 'mario/mario_shock.rlt',
             'bowser/bowser.rlt', 'bowser/extratextures.rlt',
             'toad/toad.rlt', 'toad/toad_mario.rlt', 'toad/toad_bowser.rlt',
             'koopa/koopa.rlt', 'koopa/koopa_mario.rlt', 'koopa/koopa_bowser.rlt',
             'mariogoalie/mariogoalie.rlt', 'bowsergoalie/bowsergoalie.rlt',
             'objects/gameplay/ball.rlt', 'global.rlt']
    h, o, n = total(match)
    lines += ['', 'Typical match (Mario vs Bowser; sidekicks Toad and Koopa, each in both captains\' colours; two goalies;'
              ' the ball and the Mega Strike ball indicator; the transition ball comes on top when a transition plays):',
              '  ' + ' '.join(match), '  %d textures: HD %s, originals %s, extra %s' % (n, fmt(h), fmt(o), fmt(h - o))]
    # the worst match: the two captains with the largest sets, the same captain on both sides (alt)
    worst = max(((total(['%s/%s.rlt' % (c, c), '%s/extratextures.rlt' % c, '%s/%s_alt.rlt' % (c, c)])[0], c) for c in caps))
    lines.append('  largest captain set incl. its variant: %s %s' % (worst[1], fmt(worst[0])))
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DATA)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--model', default=os.path.join(OUT, 'models', 'realesr-general-x4v3.pth'))
    ap.add_argument('--scale', type=int, default=2, choices=(2, 4))
    ap.add_argument('--max-side', type=int, default=512,
                    help='long side cap of a replacement (power of two); a texture that cannot get at least 2x under it is left out')
    ap.add_argument('--only', help='regular expression on "<name> <file>:<index>:<hash>": build only these')
    ap.add_argument('--pack', default='hdtex.pack', help='pack file name inside --out')
    ap.add_argument('--threads', type=int, default=2)
    ap.add_argument('--list', action='store_true', help='only list the selected textures')
    ap.add_argument('--upscale-only', action='store_true', help='only fill the network cache (resumable)')
    a = ap.parse_args()
    stats = {}
    texs = collect(a.data, a.only, a.scale, a.max_side, stats)
    order = sorted(texs.items(), key=lambda kv: kv[1]['names'][0])
    print('%d textures selected' % len(texs))
    if a.list:
        print('formats of all textures read:', ', '.join('%s: %d' % kv for kv in sorted(stats['formats'].items(), key=lambda kv: -kv[1])))
        print('left out:', ', '.join('%s: %d' % kv for kv in stats['excluded'].most_common()))
        for k, e in order:
            t = e['t']
            print('%016x %-6s %4dx%-4d -> %4dx%-4d %s' % (k, rlt.FMT_NAME[t.fmt], t.w, t.h, e['hd'][0], e['hd'][1], ' '.join(e['names'])))
        return
    import torch
    import esrgan
    torch.set_num_threads(a.threads)
    cache = os.path.join(a.out, 'cache')
    os.makedirs(cache, exist_ok=True)
    os.makedirs(os.path.join(a.out, 'sheets'), exist_ok=True)
    net = esrgan.load(a.model)
    t0 = time.time()
    if a.upscale_only:
        for i, (k, e) in enumerate(order):
            upscale_cached(e, net, cache)
            print('[%3d/%d %5.0f s] %016x %s' % (i + 1, len(order), time.time() - t0, k, e['names'][0]), flush=True)
        return
    items, rows = [], []
    for i, (k, e) in enumerate(order):
        hd = make_hd(e, net, cache)
        items.append((k, e, hd))
        rows.append((e['names'][0], hd['orig'], hd['hd']))
        print('[%3d/%d %5.0f s] %016x %-50s %4dx%-4d -> %4dx%-4d %s psnr %.1f' % (i + 1, len(texs), time.time() - t0, k, e['names'][0][:50], e['t'].w, e['t'].h,
                                                                              hd['w'], hd['h'], 'DXT1' if hd['fmt'] == PACK_DXT1 else 'DXT5', hd['psnr']), flush=True)
    size = write_pack(os.path.join(a.out, a.pack), items)
    base = os.path.splitext(a.pack)[0]
    with open(os.path.join(a.out, base + '-manifest.tsv' if base != 'hdtex' else 'manifest.tsv'), 'w') as f:
        f.write('key\tnames\torig_fmt\torig_w\torig_h\thd_fmt\thd_w\thd_h\tlevels\tbytes\torig_bytes\tpsnr_db\n')
        for k, e, hd in sorted(items, key=lambda x: x[1]['names'][0]):
            t = e['t']
            f.write('%016x\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%.1f\n' % (k, ' '.join(e['names']), rlt.FMT_NAME[t.fmt], t.w, t.h,
                                                                             'DXT1' if hd['fmt'] == PACK_DXT1 else 'DXT5', hd['w'], hd['h'],
                                                                             len(hd['levels']), sum(len(b) for b in hd['levels']), len(t.level0), hd['psnr']))
    rep = memory_report(items)
    with open(os.path.join(a.out, base + '-memory.txt' if base != 'hdtex' else 'memory.txt'), 'w') as f:
        f.write(rep)
    print(rep)
    if base == 'hdtex':
        per = 18
        for s in range(0, len(rows), per):
            sheet(os.path.join(a.out, 'sheets', 'sheet%02d.png' % (s // per)), rows[s:s + per])
    print('pack: %s, %d textures, %.2f MiB (%.0f s)' % (os.path.join(a.out, a.pack), len(items), size / 1048576.0, time.time() - t0))


if __name__ == '__main__':
    main()
