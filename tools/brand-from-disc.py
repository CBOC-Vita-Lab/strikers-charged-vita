#!/usr/bin/env python3
"""Brand your copy of the port with your own disc's art: home-screen bubble, LiveArea pages and loading picture.

    python3 tools/brand-from-disc.py GAME [--out DIR] [--vpk IN.vpk --vpk-out OUT.vpk] [--preview SHEET.png]

GAME is a prepared game folder (the one PREPARE-GAME.py made: it holds `files/`), or the disc's `files` folder itself.
Everything is made from the player's own disc files, on the player's own computer; nothing from the game is shipped in this
repository or in the public package:

    * the game's logo: files/opening.bnr (the Wii channel banner: IMET header, U8 archive, IMD5 + LZ77 members, TPL images)
    * the start-screen picture (the sunset, the ball and the boot): files/Art/fe/mainui.dmn (a bundle of GX textures)
    * the game's own heading font (for the START and SETTINGS labels): files/Art/fe/fonts/engfontheading36.res

and written as (the paths of the package, under --out):

    sce_sys/icon0.png                          128 x 128  the bubble on the home screen        (8-bit indexed)
    sce_sys/pic0.png                           960 x 544  the picture of the game's page       (8-bit indexed)
    sce_sys/livearea/contents/bg.png           840 x 500  the LiveArea background              (8-bit indexed)
    sce_sys/livearea/contents/startup.png      280 x 158  the START gate                       (8-bit indexed)
    sce_sys/livearea/contents/settings.png     280 x 124  the Settings item (psla:-settings)   (8-bit RGBA, like the shipped one)
    boot/loading.bin                           960 x 544  the loading screen's picture         (RGB565, see vita_bootscreen.cpp)

With --vpk the package is copied to --vpk-out with those members replaced or added (nothing else in it changes; the executable
stays byte for byte the same). Needs Python 3.8+ and Pillow (pip install pillow); nothing else.

SPDX-License-Identifier: MIT
Copyright (c) 2026 CBOC-Vita contributors. See licenses/CBOC-tools-MIT.txt.
"""
import argparse
import io
import math
import os
import struct
import sys
import zipfile

try:
    from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter
except ImportError:  # the caller (PREPARE-GAME.py) prints how to install it
    Image = None

# ---------------------------------------------------------------------------------------------------------------------------------------------
# Where things are on the disc
# ---------------------------------------------------------------------------------------------------------------------------------------------
BANNER = "opening.bnr"
FE_BUNDLE = "Art/fe/mainui.dmn"
SUPER_BUNDLE = "Art/fe/superabilityui.res"
FONT_RES = "Art/fe/fonts/engfontheading36.res"
KEY_ART_HASH = 0x57399903        # fe/screens/images: the start screen's 512 x 512 picture (the ball, the boot, the sunset)
BALL_ART_HASH = 0x944d752d       # mainui.dmn: 512 x 512 glowing charged metal soccer ball
SUNBURST_HASH = 0x250bbfa5       # mainui.dmn: 512 x 512 golden stadium starburst
ICEBURST_HASH = 0x23b38f66       # mainui.dmn: 256 x 256 electric blue radial energy burst
BOLT_A_HASH = 0x9cf58c6e         # mainui.dmn: 256 x 128 electric cyan lightning arc
BOLT_B_HASH = 0x292d15fb         # mainui.dmn: 256 x 128 electric cyan lightning arc
BOLT_C_HASH = 0x14083241         # mainui.dmn: 256 x 256 electric cyan lightning bolt
SPARKS_HASH = 0x2fd2a2e6         # mainui.dmn: 512 x 256 electric crackle sparks
MARIO_HASH = 0x886f2df4          # superabilityui.res: 256 x 256 Mario captain portrait (RGBA)
BOWSER_HASH = 0x6c217d2e         # superabilityui.res: 256 x 256 Bowser captain portrait (RGBA)
LOGO_MEMBER = "arc/timg/logo_UK_Banner.tpl"   # inside meta/banner.bin: the full logo "MARIO STRIKERS CHARGED" with alpha
SKY_MEMBER = "arc/timg/sky_banner.tpl"        # inside meta/banner.bin: fiery storm sky clouds

OUTPUTS = (
    "sce_sys/icon0.png",
    "sce_sys/pic0.png",
    "sce_sys/livearea/contents/bg.png",
    "sce_sys/livearea/contents/startup.png",
    "sce_sys/livearea/contents/settings.png",
    "boot/loading.bin",
)


class BrandError(Exception):
    pass


def find_file(root, relative):
    """root/relative with any letter case (the extractors differ), or None."""
    path = root
    for part in relative.split("/"):
        if not os.path.isdir(path):
            return None
        match = None
        for name in os.listdir(path):
            if name.lower() == part.lower():
                match = name
                break
        if match is None:
            return None
        path = os.path.join(path, match)
    return path if os.path.isfile(path) else None


def files_root(game):
    """The disc's files folder: GAME itself or GAME/files."""
    for candidate in (game, os.path.join(game, "files")):
        if os.path.isdir(candidate) and (find_file(candidate, BANNER) or find_file(candidate, FE_BUNDLE)):
            return candidate
    raise BrandError("No game files found in %s (expected a folder with files/opening.bnr and files/Art)." % game)


def read_disc(root, relative):
    path = find_file(root, relative)
    if not path:
        raise BrandError("The disc has no %s." % relative)
    with open(path, "rb") as handle:
        return handle.read()


# ---------------------------------------------------------------------------------------------------------------------------------------------
# GX texture decoding (big-endian, tiled): the formats the art uses
# ---------------------------------------------------------------------------------------------------------------------------------------------
def _e(v, bits):
    if bits == 3:
        return (v << 5) | (v << 2) | (v >> 1)
    if bits == 4:
        return v * 17
    if bits == 5:
        return (v << 3) | (v >> 2)
    return (v << 2) | (v >> 4)


def rgb565(v):
    return (_e((v >> 11) & 31, 5), _e((v >> 5) & 63, 6), _e(v & 31, 5), 255)


def rgb5a3(v):
    if v & 0x8000:
        return (_e((v >> 10) & 31, 5), _e((v >> 5) & 31, 5), _e(v & 31, 5), 255)
    return (_e((v >> 8) & 15, 4), _e((v >> 4) & 15, 4), _e(v & 15, 4), _e((v >> 12) & 7, 3))


# GX format -> (block width, block height, bits per texel)
_BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32), 8: (8, 8, 4),
          9: (8, 4, 8), 10: (4, 4, 16), 14: (8, 8, 4)}


def level_size(w, h, fmt):
    bw, bh, bpp = _BLOCK[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8


def decode_gx(fmt, w, h, data, palette=None):
    """One level of GX texel data -> a Pillow RGBA image. fmt: 0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565, 5 RGB5A3, 6 RGBA8, 8 C4, 9 C8, 14 CMPR."""
    bw, bh, _ = _BLOCK[fmt]
    pw, ph = (w + bw - 1) // bw * bw, (h + bh - 1) // bh * bh
    out = bytearray(pw * ph * 4)
    o = 0

    def put(x, y, c):
        i = (y * pw + x) * 4
        out[i:i + 4] = bytes(c)

    for ty in range(0, ph, bh):
        for tx in range(0, pw, bw):
            if fmt == 14:
                for sy in range(0, 8, 4):
                    for sx in range(0, 8, 4):
                        c0 = (data[o] << 8) | data[o + 1]
                        c1 = (data[o + 2] << 8) | data[o + 3]
                        bits = struct.unpack_from(">I", data, o + 4)[0]
                        o += 8
                        a, b = rgb565(c0), rgb565(c1)
                        if c0 > c1:
                            cs = (a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                                  tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,))
                        else:
                            cs = (a, b, tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0))
                        for i in range(16):
                            put(tx + sx + i % 4, ty + sy + i // 4, cs[(bits >> (30 - 2 * i)) & 3])
            elif fmt == 6:
                for i in range(16):
                    put(tx + i % 4, ty + i // 4, (data[o + 2 * i + 1], data[o + 32 + 2 * i], data[o + 33 + 2 * i], data[o + 2 * i]))
                o += 64
            elif fmt in (4, 5):
                fn = rgb565 if fmt == 4 else rgb5a3
                for y in range(4):
                    for x in range(4):
                        put(tx + x, ty + y, fn((data[o] << 8) | data[o + 1]))
                        o += 2
            elif fmt == 3:
                for y in range(4):
                    for x in range(4):
                        a, i = data[o], data[o + 1]
                        o += 2
                        put(tx + x, ty + y, (i, i, i, a))
            elif fmt in (0, 8):
                for y in range(8):
                    for x in range(0, 8, 2):
                        v = data[o]
                        o += 1
                        for k, n in enumerate((v >> 4, v & 15)):
                            put(tx + x + k, ty + y, (n * 17,) * 4 if fmt == 0 else palette[n])
            elif fmt in (1, 2, 9):
                for y in range(4):
                    for x in range(8):
                        n = data[o]
                        o += 1
                        if fmt == 1:
                            put(tx + x, ty + y, (n, n, n, n))
                        elif fmt == 2:
                            put(tx + x, ty + y, ((n & 15) * 17, (n & 15) * 17, (n & 15) * 17, (n >> 4) * 17))
                        else:
                            put(tx + x, ty + y, palette[n] if n < len(palette) else (255, 0, 255, 255))
            else:
                raise BrandError("texture format %d is not handled" % fmt)
    image = Image.frombytes("RGBA", (pw, ph), bytes(out))
    return image.crop((0, 0, w, h)) if (pw, ph) != (w, h) else image


def tpl_image(blob, index=0):
    """An image of a TPL file (magic 0x0020AF30): Pillow RGBA."""
    magic, count, table = struct.unpack_from(">III", blob, 0)
    if magic != 0x0020AF30 or index >= count:
        raise BrandError("not a TPL image")
    image_at, palette_at = struct.unpack_from(">II", blob, table + 8 * index)
    h, w, fmt, data_at = struct.unpack_from(">HHII", blob, image_at)
    palette = None
    if palette_at:
        entries, _, pal_fmt, pal_data = struct.unpack_from(">HHII", blob, palette_at)
        decode = {1: rgb565, 2: rgb5a3}.get(pal_fmt, rgb5a3)
        palette = [decode((blob[pal_data + 2 * k] << 8) | blob[pal_data + 2 * k + 1]) for k in range(entries)]
    return decode_gx(fmt, w, h, blob[data_at:data_at + level_size(w, h, fmt)], palette)


# ---- the game's own containers ----------------------------------------------------------------------------------------------------------------
def u8_members(blob, base=0):
    """{path: bytes} of a U8 archive that starts at blob[base] (magic 55 AA 38 2D)."""
    if blob[base:base + 4] != b"U\xaa8-":
        raise BrandError("not a U8 archive")
    root = struct.unpack_from(">I", blob, base + 4)[0]
    count = struct.unpack_from(">I", blob, base + root + 8)[0]
    names = base + root + count * 12
    out = {}
    path, ends = [], [count]
    for i in range(1, count):
        word, off, size = struct.unpack_from(">III", blob, base + root + i * 12)
        at = names + (word & 0xFFFFFF)
        name = blob[at:blob.index(b"\0", at)].decode("latin-1")
        while ends and i >= ends[-1]:
            ends.pop()
            path.pop()
        if word >> 24 == 1:
            path.append(name)
            ends.append(size)
        else:
            out["/".join(path + [name])] = blob[base + off:base + off + size]
    return out


def lz77_10(data):
    """Nintendo LZ77 type 0x10 (the banner's members: 'LZ77' + 0x10 + 24-bit size)."""
    if data[:4] == b"LZ77":
        data = data[4:]
    if data[0] != 0x10:
        raise BrandError("unsupported compression")
    size = int.from_bytes(data[1:4], "little")
    out = bytearray()
    p = 4
    while len(out) < size:
        flags = data[p]
        p += 1
        for k in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> k):
                a, b = data[p], data[p + 1]
                p += 2
                length, disp = (a >> 4) + 3, ((a & 15) << 8 | b) + 1
                for _ in range(length):
                    out.append(out[-disp])
            else:
                out.append(data[p])
                p += 1
    return bytes(out[:size])


def banner_member(blob, member, name):
    """A file of meta/<member> in opening.bnr: IMET header, a U8 of IMD5-wrapped (LZ77-compressed) members, each a U8 of its own."""
    at = blob.find(b"U\xaa8-")
    if at < 0:
        raise BrandError("opening.bnr: no archive")
    inner = u8_members(blob, at)[member]
    if inner[:4] == b"IMD5":
        inner = inner[0x20:]
    if inner[:4] == b"LZ77":
        inner = lz77_10(inner)
    return u8_members(inner, inner.find(b"U\xaa8-"))[name]


def bundle_texture(blob, wanted):
    """The texture with the name hash `wanted` of a BundleFile (.res/.dmn): (game format, Pillow RGBA)."""
    sector, count, directory, _ = struct.unpack_from(">IIII", blob, 0)
    game_to_gx = {0: 4, 1: 5, 2: 14, 3: 6, 4: 1, 5: 0, 6: 1, 7: 3, 8: 9}
    for i in range(count):
        h, block, length = struct.unpack_from(">III", blob, directory * sector + i * 12)
        if h != wanted:
            continue
        off = block * sector
        levels, game_fmt = struct.unpack_from(">II", blob, off)
        w, ht = struct.unpack_from(">HH", blob, off + 0x0E)
        entries = struct.unpack_from(">I", blob, off + 0x14)[0]
        if game_fmt not in game_to_gx:
            raise BrandError("texture %08x: unknown format" % wanted)
        fmt = game_to_gx[game_fmt]
        palette = None
        if game_fmt == 8:   # the palette follows the texels of every level
            total, lw, lh = 0, w, ht
            for _ in range(levels):
                total += lw * lh
                lw, lh = max(lw // 2, 1), max(lh // 2, 1)
            po = off + 0x20 + total
            palette = [rgb5a3((blob[po + 2 * k] << 8) | blob[po + 2 * k + 1]) for k in range(entries)]
        return game_fmt, decode_gx(fmt, w, ht, blob[off + 0x20:off + 0x20 + level_size(w, ht, fmt)], palette)
    raise BrandError("texture %08x is not in the bundle" % wanted)


# ---- the game's heading font (a CI8 glyph page and a text description in one .res) --------------------------------------------------------------
class GameFont:
    """nlFont's file: 'Glyph <char> Width <advance> <render width> <offset>' lines, packed left to right on 512 px pages (version 1.1)."""

    def __init__(self, blob):
        sector, count, directory, _ = struct.unpack_from(">IIII", blob, 0)
        description, page = None, None
        for i in range(count):
            h, block, length = struct.unpack_from(">III", blob, directory * sector + i * 12)
            body = blob[block * sector:block * sector + length]
            if body[:3] == b"NLG":
                description = body.decode("latin-1")
            elif page is None:
                page = h
        if description is None or page is None:
            raise BrandError("font: unexpected file")
        self.page = bundle_texture(blob, page)[1]
        self.glyphs = {}
        size = 512
        render_h = 0
        x = y = 0
        for line in description.replace("\r", "").split("\n"):
            words = line.split()
            if not words:
                continue
            if words[0] == "PageSize":
                size = int(words[1])
            elif words[0] == "Height":
                render_h = int(words[words.index("RenderHeight") + 1])
            elif words[0] == "Glyph":
                ch = words[1] if len(words[1]) == 1 else chr(int(words[1]))
                advance, width, offset = int(words[3]), int(words[4]), int(words[5])
                if x + width > size:
                    x, y = 0, y + render_h
                self.glyphs[ch] = (x, y, width, advance, offset)
                x += width
        self.render_h = render_h

    def text(self, label):
        """The label in the game's heading font: RGBA strip, black fill with a white outline (as the page has it)."""
        pen, boxes = 0, []
        for ch in label:
            g = self.glyphs.get(ch) or self.glyphs.get(ch.upper())
            if not g:
                pen += 14
                continue
            x, y, w, advance, offset = g
            boxes.append((pen + offset, self.page.crop((x, y, x + w, y + self.render_h))))
            pen += advance
        strip = Image.new("RGBA", (pen + 24, self.render_h), (0, 0, 0, 0))
        for px, glyph in boxes:
            strip.alpha_composite(glyph, (max(px, 0) + 8, 0))
        return strip


# ---------------------------------------------------------------------------------------------------------------------------------------------
# Composition
# ---------------------------------------------------------------------------------------------------------------------------------------------
FLAME_TOP, FLAME_MID, FLAME_BOTTOM = (255, 244, 120), (255, 178, 38), (232, 84, 18)
INK = (28, 10, 12)
SHADE = (8, 5, 14)


def vgradient(size, stops):
    """A vertical RGB gradient: stops = [(position 0..1, (r, g, b)), ...]."""
    w, h = size
    column = Image.new("RGB", (1, h))
    px = column.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / max(t1 - t0, 1e-6)
                px[0, y] = tuple(int(c0[i] + (c1[i] - c0[i]) * k) for i in range(3))
                break
    return column.resize((w, h))


def ramp(length, stops, horizontal=True, size=None):
    """An 'L' mask that changes along one axis: stops = [(position 0..1, value 0..255)]; size = (w, h) of the result."""
    row = Image.new("L", (length, 1))
    px = row.load()
    for i in range(length):
        t = i / max(length - 1, 1)
        for (t0, v0), (t1, v1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                px[i, 0] = int(v0 + (v1 - v0) * (t - t0) / max(t1 - t0, 1e-6))
                break
    if horizontal:
        return row.resize(size) if size else row
    return row.rotate(-90, expand=True).transpose(Image.FLIP_LEFT_RIGHT).resize(size) if size else row


def column_ramp(h, stops, w):
    col = Image.new("L", (1, h))
    px = col.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        for (t0, v0), (t1, v1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                px[0, y] = int(v0 + (v1 - v0) * (t - t0) / max(t1 - t0, 1e-6))
                break
    return col.resize((w, h))


def row_ramp(w, stops, h):
    return ramp(w, stops).resize((w, h))


def with_shadow(image, offset=(3, 5), blur=4, opacity=0.65):
    pad = blur * 3 + max(offset)
    canvas = Image.new("RGBA", (image.width + 2 * pad, image.height + 2 * pad), (0, 0, 0, 0))
    shadow = Image.new("RGBA", image.size, (0, 0, 0, 255))
    shadow.putalpha(image.getchannel("A").point(lambda a: int(a * opacity)))
    canvas.alpha_composite(shadow, (pad + offset[0], pad + offset[1]))
    canvas = canvas.filter(ImageFilter.GaussianBlur(blur))
    canvas.alpha_composite(image, (pad, pad))
    return canvas


def flame_label(font, label, height):
    """The label in the logo's colours: the game's heading font recoloured (fill = flame gradient, outline = ink), with a soft drop shadow."""
    strip = font.text(label)
    box = strip.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    strip = strip.crop(box)
    luminance = strip.convert("RGB").convert("L")   # black fill = 0, white outline = 249
    fill = vgradient(strip.size, [(0.0, FLAME_TOP), (0.45, FLAME_MID), (1.0, FLAME_BOTTOM)])
    ink = Image.new("RGB", strip.size, INK)
    # the white outline becomes ink, the black fill the flame; the scratches inside the fill stay as dark flecks
    coloured = Image.composite(ink, fill, luminance.point(lambda v: min(255, int(v * 1.25))))
    coloured.putalpha(strip.getchannel("A"))
    scale = height / coloured.height
    coloured = coloured.resize((max(1, round(coloured.width * scale)), height), Image.LANCZOS)
    return with_shadow(coloured, offset=(max(1, height // 18), max(2, height // 12)), blur=max(1, height // 16))


def trim(image):
    box = image.getchannel("A").point(lambda a: 255 if a > 12 else 0).getbbox()
    return image.crop(box) if box else image


def fit_width(image, width):
    return image.resize((width, max(1, round(image.height * width / image.width))), Image.LANCZOS)


def fit_height(image, height):
    return image.resize((max(1, round(image.width * height / image.height)), height), Image.LANCZOS)


def cover(image, w, h, focus=(0.5, 0.5)):
    """Scale uniformly until the picture covers w x h, crop around the focus point (fractions of the scaled picture): never stretched."""
    scale = max(w / image.width, h / image.height)
    scaled = image.resize((max(w, round(image.width * scale)), max(h, round(image.height * scale))), Image.LANCZOS)
    x = min(max(round(scaled.width * focus[0] - w / 2), 0), scaled.width - w)
    y = min(max(round(scaled.height * focus[1] - h / 2), 0), scaled.height - h)
    return scaled.crop((x, y, x + w, y + h))


def darken(image, factor):
    return image.point(lambda v: int(v * factor))


def shade(image, mask, colour=SHADE):
    image.paste(Image.new("RGB", image.size, colour), (0, 0), mask)
    return image


def hero(art, w, h, base_dim=0.5, fade=0.45, shade_left=0.55):
    """The wide background: the picture at its own proportions (a square of the page's height) on the right, its blurred and darkened
    enlargement filling the rest, so that the picture is never stretched and the logo has quiet space beside it."""
    art = art.convert("RGB")
    # the quiet side is the picture's own sky (its left part: clouds and the glow of the horizon), enlarged and blurred
    sky = art.crop((0, 40, art.width * 2 // 5, 40 + art.height * 3 // 5))
    base = darken(cover(sky, w, h, (0.5, 0.5)).filter(ImageFilter.GaussianBlur(max(10, w // 40))), base_dim)
    front = art.resize((h, h), Image.LANCZOS)
    base.paste(front, (w - h, 0), row_ramp(h, [(0.0, 0), (fade, 255), (1.0, 255)], h))
    return shade(base, row_ramp(w, [(0.0, int(255 * shade_left)), (0.55, int(255 * shade_left * 0.5)), (1.0, 0)], h))


def vignette(image, strength=0.5):
    w, h = image.size
    mask = Image.radial_gradient("L").resize((w, h))
    return shade(image, mask.point(lambda v: int(max(0, v - 90) * strength * 255 / 165)), (6, 4, 10))


def bottom_shade(image, start, strength=0.8):
    mask = Image.new("L", image.size, 0)
    mask.paste(column_ramp(image.height - start, [(0.0, 0), (1.0, int(255 * strength))], image.width), (0, start))
    return shade(image, mask)


def paste_rgba(base, overlay, xy):
    out = base.convert("RGBA")
    out.alpha_composite(overlay, xy)
    return out.convert("RGB")


def rounded_mask(size, radius):
    mask = Image.new("L", (size[0] * 4, size[1] * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] * 4 - 1, size[1] * 4 - 1), radius * 4, fill=255)
    return mask.resize(size, Image.LANCZOS)


def boost(im, sat=1.18, contrast=1.12, bright=1.04, sharp=1.25):
    im = ImageEnhance.Color(im).enhance(sat)
    im = ImageEnhance.Contrast(im).enhance(contrast)
    im = ImageEnhance.Brightness(im).enhance(bright)
    if sharp != 1.0:
        im = ImageEnhance.Sharpness(im).enhance(sharp)
    return im


def boost_rgba(im, sat=1.15, contrast=1.10, bright=1.05, sharp=1.35):
    rgb = boost(im.convert("RGB"), sat=sat, contrast=contrast, bright=bright, sharp=sharp)
    out = rgb.convert("RGBA")
    out.putalpha(im.getchannel("A"))
    return out


def feather_edge(rgba, left=0, right=0, bottom=0):
    """Smoothly fade hard texture-border cuts on a character portrait."""
    a = rgba.getchannel("A").copy()
    w, h = rgba.size
    px = a.load()
    for y in range(h):
        ky = (h - 1 - y) / float(bottom) if (bottom > 0 and y >= h - bottom) else 1.0
        for x in range(w):
            k = ky
            if left > 0 and x < left:
                k *= x / float(left)
            if right > 0 and x >= w - right:
                k *= (w - 1 - x) / float(right)
            if k < 1.0:
                px[x, y] = int(px[x, y] * max(0.0, k))
    out = rgba.copy()
    out.putalpha(a)
    return out


def glow_layer(rgba, color, radius=12, opacity=0.75, pad=None):
    if pad is None:
        pad = radius * 3
    canvas = Image.new("RGBA", (rgba.width + 2 * pad, rgba.height + 2 * pad), (0, 0, 0, 0))
    glow = Image.new("RGBA", rgba.size, color + (255,))
    glow.putalpha(rgba.getchannel("A").point(lambda a: int(a * opacity)))
    canvas.alpha_composite(glow, (pad, pad))
    canvas = canvas.filter(ImageFilter.GaussianBlur(radius))
    canvas.alpha_composite(rgba, (pad, pad))
    return canvas, pad


def screen_rgb(base, over, opacity=1.0):
    if opacity < 1.0:
        over = over.point(lambda v: int(v * opacity))
    return ImageChops.screen(base.convert("RGB"), over.convert("RGB"))


def add_rgba_screen(base_rgb, overlay_rgba, xy, opacity=1.0):
    layer = Image.new("RGB", base_rgb.size, (0, 0, 0))
    ov = overlay_rgba.convert("RGBA")
    a = ov.getchannel("A").point(lambda v: int(v * opacity))
    pm = Image.composite(ov.convert("RGB"), Image.new("RGB", ov.size, (0, 0, 0)), a)
    layer.paste(pm, xy)
    return ImageChops.screen(base_rgb, layer)


def radial_glow(size, center, radius, color, strength=1.0):
    w, h = size
    cx, cy = center
    im = Image.new("RGB", (max(1, w // 2), max(1, h // 2)), (0, 0, 0))
    px = im.load()
    r2 = (radius / 2.0) ** 2
    ccx, ccy = cx / 2.0, cy / 2.0
    for y in range(im.height):
        dy2 = (y - ccy) ** 2
        for x in range(im.width):
            d2 = (x - ccx) ** 2 + dy2
            if d2 < r2:
                k = ((1.0 - math.sqrt(d2 / r2)) ** 2.0) * strength
                px[x, y] = (int(color[0] * k), int(color[1] * k), int(color[2] * k))
    return im.resize((w, h), Image.BILINEAR)


def _stadium_scene(art, logo, extras, w, h, logo_w, logo_y, hero_h, ball_cy, ball_scale, is_livearea):
    """Vibrant electrified stadium backdrop with Mario vs Bowser hero clash, lightning arcs, and centered logo."""
    mario = extras.get("mario") if extras else None
    bowser = extras.get("bowser") if extras else None
    if mario is None or bowser is None:
        # Minimal synthetic disc (e.g. unit tests without superabilityui.res): fall back to clean hero art
        im = hero(art, w, h, base_dim=0.45)
        mark = with_shadow(fit_width(logo, logo_w), (4, 7), 6)
        return vignette(paste_rgba(im, mark, ((w - mark.width) // 2 if is_livearea else 40, logo_y)), 0.45)

    bg = vgradient((w, h), [
        (0.0, (12, 22, 56)),
        (0.28, (30, 22, 66)),
        (0.56, (108, 38, 28)),
        (0.80, (176, 82, 22)),
        (1.0, (24, 16, 20)),
    ])
    sky_bnr = extras.get("sky")
    if sky_bnr is not None:
        clouds = boost(sky_bnr.resize((w, int(h * 0.64)), Image.LANCZOS), sat=1.35, contrast=1.18, bright=0.98, sharp=1.0)
        cmask = column_ramp(clouds.height, [(0.0, 220), (0.62, 195), (1.0, 0)], w)
        bg.paste(clouds, (0, 0), cmask)

    sunburst = extras.get("sunburst")
    if sunburst is not None and sunburst.width >= 128 and sunburst.height >= 128:
        inset = min(44, sunburst.width // 10, sunburst.height // 10)
        sun_crop = sunburst.crop((inset, inset, sunburst.width - inset, sunburst.height - inset))
        sun = boost(sun_crop.resize((w, h), Image.LANCZOS), sat=1.22, contrast=1.15, bright=0.72)
        bg = screen_rgb(bg, sun, opacity=0.22)

    iceburst = extras.get("iceburst")
    if iceburst is not None and iceburst.width >= 64 and iceburst.height >= 64:
        inset = min(16, iceburst.width // 12, iceburst.height // 12)
        ice_crop = iceburst.crop((inset, inset, iceburst.width - inset, iceburst.height - inset))
        ice = boost(ice_crop.resize((w, h), Image.LANCZOS), sat=1.35, contrast=1.22, bright=0.82)
        ice_l = Image.new("RGB", (w, h), (0, 0, 0))
        imask = Image.radial_gradient("L").resize((w, h)).point(lambda v: max(0, 255 - v))
        ice_l.paste(ice, (0, 0), imask)
        bg = screen_rgb(bg, ice_l, opacity=0.34)

    ball_src = extras.get("ball") or art
    bsz = int(h * ball_scale)
    binset = min(28, ball_src.width // 16, ball_src.height // 16)
    ball_sq = boost(
        ball_src.crop((binset, binset, ball_src.width - binset, ball_src.height - binset)).resize((bsz, bsz), Image.LANCZOS),
        sat=1.28, contrast=1.20, bright=1.06 if is_livearea else 0.92, sharp=1.20,
    )
    # Pillow's radial_gradient reaches ~180 at the 4 edge midpoints (255 only at corners), so fade to 0 by v=174 for a seamless circle.
    bmask = Image.radial_gradient("L").resize((bsz, bsz)).point(
        lambda v: 255 if v < 75 else int(255 * (max(0.0, 1.0 - (v - 75) / 99.0) ** 1.6))
    )
    bg.paste(ball_sq, ((w - bsz) // 2, ball_cy - bsz // 2), bmask)

    bg = screen_rgb(bg, radial_glow((w, h), (int(w * 0.16), int(h * 0.65)), int(h * 0.62), (255, 115, 25), 0.58))
    bg = screen_rgb(bg, radial_glow((w, h), (int(w * 0.84), int(h * 0.65)), int(h * 0.62), (30, 185, 255), 0.58))
    bg = screen_rgb(bg, radial_glow((w, h), (w // 2, int(h * 0.28)), int(h * 0.68), (255, 195, 55), 0.38))

    if extras.get("bolt_a") is not None:
        b1 = extras["bolt_a"].resize((int(w * 0.34), int(h * 0.28)), Image.LANCZOS).rotate(14, expand=True, resample=Image.BICUBIC)
        bg = add_rgba_screen(bg, b1, (int(w * 0.03), int(h * 0.04)), opacity=0.82)
    if extras.get("bolt_b") is not None:
        b2 = extras["bolt_b"].resize((int(w * 0.34), int(h * 0.28)), Image.LANCZOS).rotate(-16, expand=True, resample=Image.BICUBIC)
        bg = add_rgba_screen(bg, b2, (w - b2.width - int(w * 0.02), int(h * 0.05)), opacity=0.82)
    if extras.get("bolt_c") is not None:
        b3 = extras["bolt_c"].resize((int(h * 0.44), int(h * 0.44)), Image.LANCZOS).rotate(20, expand=True, resample=Image.BICUBIC)
        bg = add_rgba_screen(bg, b3, (w // 2 - b3.width // 2, int(h * 0.22)), opacity=0.42)

    out = bg.convert("RGBA")
    m_hero = boost_rgba(fit_height(mario, hero_h), sat=1.18, contrast=1.12, bright=1.06, sharp=1.35)
    b_hero = feather_edge(boost_rgba(fit_height(bowser, int(hero_h * 1.02)), sat=1.16, contrast=1.12, bright=1.05, sharp=1.35), left=26)

    m_g, m_pad = glow_layer(m_hero, (255, 165, 35), radius=14, opacity=0.72, pad=36)
    b_g, b_pad = glow_layer(b_hero, (50, 205, 255), radius=14, opacity=0.75, pad=36)
    mx = -26 if is_livearea else -18
    bx = (w - b_hero.width + 32) if is_livearea else (w - b_hero.width + 24)
    out.alpha_composite(m_g, (mx - m_pad, h - m_hero.height + 12 - m_pad))
    out.alpha_composite(b_g, (bx - b_pad, h - b_hero.height + 14 - b_pad))

    res = bottom_shade(out.convert("RGB"), int(h * 0.78), 0.50)
    res = vignette(res, 0.28)

    if extras.get("sparks") is not None:
        sp = extras["sparks"].resize((int(w * 0.54), int(h * 0.22)), Image.LANCZOS)
        res = add_rgba_screen(res, sp, ((w - sp.width) // 2, logo_y + int(logo_w * 0.34)), opacity=0.68)

    hero_logo = boost_rgba(fit_width(logo, logo_w), sat=1.12, contrast=1.08, bright=1.04, sharp=1.22)
    logo_glow, lpad = glow_layer(hero_logo, (45, 195, 255), radius=10, opacity=0.48, pad=26)
    sh_blur, sh_off = 7, (4, 7)
    sh_pad = sh_blur * 3 + max(sh_off)
    logo_sh = with_shadow(logo_glow, offset=sh_off, blur=sh_blur, opacity=0.80)
    return paste_rgba(res, logo_sh, ((w - logo_sh.width) // 2, logo_y - lpad - sh_pad))


# ---- the images --------------------------------------------------------------------------------------------------------------------------------
def make_background(art, logo, extras=None):
    """840 x 500: Mario vs Bowser stadium clash framing the centered hero logo cleanly above the START gate."""
    return _stadium_scene(art, logo, extras, 840, 500, logo_w=364, logo_y=24, hero_h=326, ball_cy=365, ball_scale=0.72, is_livearea=True)


def make_icon(art, logo, extras=None):
    """128 x 128: circular-safe PS Vita Home Screen bubble with bright Mario portrait, electric-gold rim, and unclipped centered logo."""
    mario = extras.get("mario") if extras else None
    if mario is None:
        im = darken(cover(art.convert("RGB"), 128, 128, (0.45, 0.62)), 0.78)
        im = bottom_shade(im, 52, 0.85)
        mark = with_shadow(fit_width(logo, 96), (1, 2), 2, 0.85)
        return paste_rgba(im, mark, ((128 - mark.width) // 2, 117 - mark.height))

    s = 128
    bg = vgradient((s, s), [(0.0, (14, 92, 198)), (0.48, (30, 48, 124)), (0.78, (205, 82, 18)), (1.0, (82, 22, 14))])
    iceburst = extras.get("iceburst")
    if iceburst is not None and iceburst.width >= 64 and iceburst.height >= 64:
        inset = min(16, iceburst.width // 12, iceburst.height // 12)
        ice = boost(iceburst.crop((inset, inset, iceburst.width - inset, iceburst.height - inset)).resize((s, s), Image.LANCZOS),
                    sat=1.38, contrast=1.22, bright=0.88)
        bg = screen_rgb(bg, ice, opacity=0.40)
    bg = screen_rgb(bg, radial_glow((s, s), (64, 48), 72, (255, 210, 65), 0.58))
    if extras.get("bolt_b") is not None:
        b_sm = extras["bolt_b"].resize((120, 58), Image.LANCZOS).rotate(-16, expand=True, resample=Image.BICUBIC)
        bg = add_rgba_screen(bg, b_sm, (4, 14), opacity=0.72)

    m = boost_rgba(fit_height(mario, 80), sat=1.22, contrast=1.15, bright=1.08, sharp=1.45)
    mg, mpad = glow_layer(m, (255, 215, 75), radius=5, opacity=0.78, pad=14)
    out = bg.convert("RGBA")
    out.alpha_composite(mg, ((s - m.width) // 2 - mpad + 2, 6 - mpad))

    res = out.convert("RGB")
    cradle = Image.new("L", (s, s), 0)
    ImageDraw.Draw(cradle).ellipse((14, 62, 114, 122), fill=155)
    res = shade(res, cradle.filter(ImageFilter.GaussianBlur(10)), (10, 6, 18))

    ring_g = Image.new("L", (s * 4, s * 4), 0)
    ImageDraw.Draw(ring_g).ellipse((8 * 4, 8 * 4, 120 * 4, 120 * 4), outline=210, width=4 * 4)
    ring_g = ring_g.resize((s, s), Image.LANCZOS)
    rgrad = vgradient((s, s), [(0.0, (115, 238, 255)), (0.48, (255, 230, 100)), (1.0, (255, 112, 22))])
    res.paste(rgrad, (0, 0), ring_g.filter(ImageFilter.GaussianBlur(2)))
    res.paste(rgrad, (0, 0), ring_g.point(lambda v: int(v * 0.92)))

    mk = boost_rgba(fit_width(logo, 92), sat=1.16, contrast=1.12, bright=1.06, sharp=1.32)
    mk_s = with_shadow(mk, offset=(1, 2), blur=2, opacity=0.92)
    return paste_rgba(res, mk_s, ((s - mk_s.width) // 2, 117 - mk_s.height))


def make_startup(art, logo, font, extras=None):
    """280 x 158: sleek steel-and-energy LiveArea START gate with glowing charged-ball core and crisp heading typography."""
    w, h = 280, 158
    base = vgradient((w, h), [
        (0.0, (20, 40, 82)),
        (0.45, (14, 22, 48)),
        (0.60, (12, 16, 32)),
        (1.0, (32, 16, 18)),
    ])
    ball_src = (extras.get("ball") if extras else None) or art
    binset = min(24, ball_src.width // 16, ball_src.height // 16)
    b = boost(ball_src.crop((binset, binset, ball_src.width - binset, ball_src.height - binset)).resize((w, w), Image.LANCZOS),
              sat=1.30, contrast=1.18, bright=0.68, sharp=1.15)
    bm = Image.radial_gradient("L").resize((w, w)).point(lambda v: max(0, int((max(0.0, 1.0 - v / 176.0) ** 1.35) * 215)))
    base.paste(b, (0, (h - w) // 2 + 4), bm)

    base = screen_rgb(base, radial_glow((w, h), (w // 2, 14), 175, (55, 205, 255), 0.48))
    base = screen_rgb(base, radial_glow((w, h), (w // 2, h - 8), 185, (255, 135, 20), 0.54))
    if extras and extras.get("bolt_a") is not None:
        base = add_rgba_screen(base, extras["bolt_a"].resize((256, 94), Image.LANCZOS), (12, 30), opacity=0.58)

    gloss = Image.new("RGBA", (w * 4, h * 4), (0, 0, 0, 0))
    ImageDraw.Draw(gloss).ellipse((-40 * 4, -95 * 4, (w + 40) * 4, (h // 2 + 12) * 4), fill=(255, 255, 255, 28))
    out = base.convert("RGBA")
    out.alpha_composite(gloss.resize((w, h), Image.LANCZOS))

    scale = 4
    rim_outer = Image.new("L", (w * scale, h * scale), 0)
    ImageDraw.Draw(rim_outer).rounded_rectangle((3 * scale, 3 * scale, (w - 4) * scale, (h - 4) * scale), 14 * scale, outline=255, width=4 * scale)
    rim_grad = vgradient((w, h), [(0.0, (125, 235, 255)), (0.45, (255, 228, 100)), (1.0, (245, 95, 20))])
    out.paste(rim_grad.convert("RGBA"), (0, 0), rim_outer.resize((w, h), Image.LANCZOS))

    rim_in = Image.new("L", (w * scale, h * scale), 0)
    ImageDraw.Draw(rim_in).rounded_rectangle((8 * scale, 8 * scale, (w - 9) * scale, (h - 9) * scale), 10 * scale, outline=135, width=1 * scale)
    out.paste(Image.new("RGBA", (w, h), (225, 248, 255, 255)), (0, 0), rim_in.resize((w, h), Image.LANCZOS))

    label = flame_label(font, "START", 70)
    label_g, _ = glow_layer(label, (255, 165, 25), radius=7, opacity=0.52, pad=18)
    out.alpha_composite(label_g, ((w - label_g.width) // 2, (h - label_g.height) // 2 + 2))
    return out.convert("RGB")


def make_settings(art, font):
    """280 x 124 RGBA: a rounded banner (the footprint of the shipped item: 278 x 64 at the top) with the sunset and SETTINGS in the game's font."""
    w, h = 278, 64
    strip = darken(cover(art.convert("RGB"), w, h, (0.5, 0.32)), 0.8)
    strip = shade(strip, row_ramp(w, [(0.0, 190), (0.6, 70), (1.0, 0)], h))
    panel = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    panel.paste(strip, (0, 0), rounded_mask((w, h), 16))
    edge = Image.new("L", (w * 4, h * 4), 0)
    ImageDraw.Draw(edge).rounded_rectangle((0, 0, w * 4 - 1, h * 4 - 1), 64, outline=255, width=8)
    edge = edge.resize((w, h), Image.LANCZOS)
    panel.paste(vgradient((w, h), [(0.0, (255, 214, 90)), (1.0, (230, 90, 20))]), (0, 0), edge)
    label = flame_label(font, "SETTINGS", 36)
    panel.alpha_composite(label, (14, (h - label.height) // 2))
    out = Image.new("RGBA", (280, 124), (0, 0, 0, 0))
    out.alpha_composite(panel, (1, 1))
    return out


def make_pic0(art, logo, extras=None):
    """960 x 544: widescreen launch splash matching the LiveArea stadium clash."""
    return _stadium_scene(art, logo, extras, 960, 544, logo_w=460, logo_y=64, hero_h=356, ball_cy=430, ball_scale=0.60, is_livearea=False)


def make_loading(art, logo):
    """960 x 544 for boot/loading.bin: the logo large at the top, quiet at the bottom where the progress bar and the ball go (y 460 and below)."""
    im = vignette(hero(art, 960, 544, base_dim=0.45), 0.5)
    mark = with_shadow(fit_width(logo, 430), (5, 9), 8)
    return bottom_shade(paste_rgba(im, mark, (40, 70)), 420, 0.75)


def to_indexed(image):
    return image.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)


def png_bytes(image):
    buffer = io.BytesIO()
    image.save(buffer, "PNG", optimize=True)
    return buffer.getvalue()


def loading_bin(image):
    """boot/loading.bin: 'NPBS', u16 width, u16 height, u32 format (1 = RGB565 little-endian), the pixels, rows top first."""
    rgb = image.convert("RGB").tobytes()
    count = image.width * image.height
    pixels = bytearray(count * 2)
    for i in range(count):
        v = ((rgb[3 * i] >> 3) << 11) | ((rgb[3 * i + 1] >> 2) << 5) | (rgb[3 * i + 2] >> 3)
        pixels[2 * i] = v & 255
        pixels[2 * i + 1] = v >> 8
    return b"NPBS" + struct.pack("<HHI", image.width, image.height, 1) + bytes(pixels)


# ---------------------------------------------------------------------------------------------------------------------------------------------
# The whole thing
# ---------------------------------------------------------------------------------------------------------------------------------------------
def _try_bundle_tex(blob, h, mode="RGBA", do_trim=False):
    try:
        im = bundle_texture(blob, h)[1].convert(mode)
        return trim(im) if do_trim else im
    except (BrandError, OSError, KeyError, ValueError, struct.error, IndexError):
        return None


def load_extras(root, banner, mainui):
    """Optional disc artwork used by the upgraded LiveArea/bubble compositions (gracefully absent on minimal test discs)."""
    extras = {}
    try:
        extras["sky"] = tpl_image(banner_member(banner, "meta/banner.bin", SKY_MEMBER)).convert("RGB")
    except (BrandError, OSError, KeyError, ValueError, struct.error, IndexError):
        pass
    extras["ball"] = _try_bundle_tex(mainui, BALL_ART_HASH, "RGB")
    extras["sunburst"] = _try_bundle_tex(mainui, SUNBURST_HASH, "RGB")
    extras["iceburst"] = _try_bundle_tex(mainui, ICEBURST_HASH, "RGB")
    extras["bolt_a"] = _try_bundle_tex(mainui, BOLT_A_HASH, "RGBA")
    extras["bolt_b"] = _try_bundle_tex(mainui, BOLT_B_HASH, "RGBA")
    extras["bolt_c"] = _try_bundle_tex(mainui, BOLT_C_HASH, "RGBA")
    extras["sparks"] = _try_bundle_tex(mainui, SPARKS_HASH, "RGBA")
    super_path = find_file(root, SUPER_BUNDLE)
    if super_path:
        try:
            with open(super_path, "rb") as handle:
                sa = handle.read()
            extras["mario"] = _try_bundle_tex(sa, MARIO_HASH, "RGBA", do_trim=True)
            extras["bowser"] = _try_bundle_tex(sa, BOWSER_HASH, "RGBA", do_trim=True)
        except OSError:
            pass
    return extras


def load_sources(game):
    root = files_root(game)
    banner = read_disc(root, BANNER)
    mainui = read_disc(root, FE_BUNDLE)
    logo = trim(tpl_image(banner_member(banner, "meta/banner.bin", LOGO_MEMBER)))
    art = bundle_texture(mainui, KEY_ART_HASH)[1].convert("RGB")
    return logo, art, GameFont(read_disc(root, FONT_RES)), load_extras(root, banner, mainui)


def build(game):
    """({package path: bytes} of every branded file, {name: Pillow image} of what they show)."""
    if Image is None:
        raise BrandError("Pillow is not installed (python3 -m pip install pillow).")
    logo, art, font, extras = load_sources(game)
    shown = {
        "icon0": make_icon(art, logo, extras),
        "bg": make_background(art, logo, extras),
        "startup": make_startup(art, logo, font, extras),
        "settings": make_settings(art, font),
        "pic0": make_pic0(art, logo, extras),
        "loading": make_loading(art, logo),
    }
    files = {
        "sce_sys/icon0.png": png_bytes(to_indexed(shown["icon0"])),
        "sce_sys/pic0.png": png_bytes(to_indexed(shown["pic0"])),
        "sce_sys/livearea/contents/bg.png": png_bytes(to_indexed(shown["bg"])),
        "sce_sys/livearea/contents/startup.png": png_bytes(to_indexed(shown["startup"])),
        "sce_sys/livearea/contents/settings.png": png_bytes(shown["settings"]),
        "boot/loading.bin": loading_bin(shown["loading"]),
    }
    return files, shown


def write_tree(files, out):
    for name, data in files.items():
        path = os.path.join(out, *name.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data)


def apply_to_vpk(files, source, destination):
    """Copy the package with the branded members replaced or added; every other member is copied unchanged, in its order."""
    if os.path.abspath(source) == os.path.abspath(destination):
        raise BrandError("Write the branded package to a new file; the original is left as it is.")
    with zipfile.ZipFile(source) as old:
        if "eboot.bin" not in old.namelist():
            raise BrandError("%s is not a Vita package (no eboot.bin)." % source)
        with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as new:
            for info in old.infolist():
                if info.filename not in files:
                    new.writestr(info, old.read(info.filename), compress_type=info.compress_type)
            for name in OUTPUTS:   # a fixed order and timestamp: the same disc gives the same bytes
                entry = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_STORED if name.endswith(".png") else zipfile.ZIP_DEFLATED
                entry.external_attr = 0o644 << 16
                new.writestr(entry, files[name])


def contact_sheet(shown, path):
    """One picture of everything made, on a checkerboard so that the transparent parts show."""
    pad = 16
    width = 960 + 2 * pad
    rows, row, x = [], [], pad
    for name in ("icon0", "startup", "settings", "bg", "pic0", "loading"):
        im = shown[name]
        if x + im.width > width - pad and row:
            rows.append(row)
            row, x = [], pad
        row.append((name, im, x))
        x += im.width + pad
    rows.append(row)
    sheet = Image.new("RGB", (width, pad + sum(max(i.height for _, i, _ in r) + 22 + pad for r in rows)), (34, 34, 38))
    draw = ImageDraw.Draw(sheet)
    y = pad
    for row in rows:
        for name, im, x in row:
            draw.text((x, y), "%s  %dx%d" % (name, im.width, im.height), fill=(220, 220, 220))
            if im.mode == "RGBA":
                checker = Image.new("RGBA", im.size)
                cd = ImageDraw.Draw(checker)
                for cy in range(0, im.height, 10):
                    for cx in range(0, im.width, 10):
                        cd.rectangle((cx, cy, cx + 9, cy + 9), fill=(90, 90, 96, 255) if (cx // 10 + cy // 10) % 2 else (60, 60, 66, 255))
                checker.alpha_composite(im)
                im = checker
            sheet.paste(im.convert("RGB"), (x, y + 18))
        y += max(i.height for _, i, _ in row) + 22 + pad
    sheet.save(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("game", help="the prepared game folder (it holds files/) or the disc's files folder")
    parser.add_argument("--out", help="write the branded files (sce_sys/..., boot/loading.bin) into this folder")
    parser.add_argument("--vpk", help="the package to brand (the original is not changed)")
    parser.add_argument("--vpk-out", help="where to write the branded package")
    parser.add_argument("--preview", help="write one picture of everything made (a contact sheet)")
    args = parser.parse_args(argv)
    if args.vpk and not args.vpk_out:
        parser.error("--vpk needs --vpk-out")
    if not (args.out or args.vpk or args.preview):
        parser.error("nothing to do: give --out, --vpk and --vpk-out, or --preview")
    try:
        files, shown = build(args.game)
        if args.out:
            write_tree(files, args.out)
            print("Wrote %d branded files to %s" % (len(files), args.out))
        if args.vpk:
            apply_to_vpk(files, args.vpk, args.vpk_out)
            print("Wrote %s" % args.vpk_out)
        if args.preview:
            contact_sheet(shown, args.preview)
            print("Wrote %s" % args.preview)
    except (BrandError, OSError, KeyError, ValueError, struct.error, IndexError) as error:
        print("Branding skipped: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
