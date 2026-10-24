"""Reader for Mario Strikers Charged .rlt texture bundles, as they are on the disc (big-endian).

    BundleHeader     {u32 magic 'PTLG'; u32 numTextures; u32 pad1, pad2}
    glTexBundleDict  numTextures x {u32 hash; u32 offset; u32 fileSize; u32 pad}
                     offset: from the first byte after the dictionary
    per entry:       GXTextureHeader (0x20) + the levels (GX-tiled, big-endian, level 0 first,
                     consecutive) + for CI8 the palette (numEntries u16 RGB5A3, big-endian) right
                     after the last level; or a texture animation (first word 0x5F6C6669)

    GXTextureHeader  0x00 u32 numLevels; 0x04 u32 format (the game's enum, eGXTextureFormat);
                     0x08 u8 numBits[4]; 0x0C u8 missingTexture; 0x0D pad; 0x0E u16 width;
                     0x10 u16 height; 0x12 pad[2]; 0x14 u32 numEntries; 0x18 u32 pad[2]

How the game hands these to GX (native/src/NL/glx/glxTexture.cpp): glx_MakeTexture copies the
levels as they are into the texture's memory (npa_texture_entry_swap / npa_cvt_texture_entry
convert only the header fields, never texels or palette) and PlatTexture::Prepare maps the
format through glx_GetGXFormatTable() = {4, 5, 14, 6, 1, 0, 1, 3, 9}; a CI8 palette is copied
from textureData + GCTextureSize(all levels) and loaded as a GX_TL_RGB5A3 TLUT of numEntries.
gbReduceTextures (skip 1 or 2 levels) is a tweak that defaults to false and nothing in the port
sets it, so the GPU's level 0 is the file's level 0.
"""
import struct

RLT_MAGIC = 0x50544C47  # 'PTLG'
TEXANIM_MAGIC = 0x5F6C6669

# the game's eGXTextureFormat -> GX hardware texture format (glx_GetGXFormatTable)
GAME_TO_GX = {0: 0x4, 1: 0x5, 2: 0xE, 3: 0x6, 4: 0x1, 5: 0x0, 6: 0x1, 7: 0x3, 8: 0x9}
GAME_FMT_NAME = {0: 'RGB565', 1: 'RGB5A3', 2: 'CMPR', 3: 'RGBA8', 4: 'I8', 5: 'I4', 6: 'A8', 7: 'IA8', 8: 'CI8'}

# GX texture formats: (block w, block h, bits per texel)
FMT = {0x0: (8, 8, 4), 0x1: (8, 4, 8), 0x2: (8, 4, 8), 0x3: (4, 4, 16), 0x4: (4, 4, 16), 0x5: (4, 4, 16), 0x6: (4, 4, 32),
       0x8: (8, 8, 4), 0x9: (8, 4, 8), 0xA: (4, 4, 16), 0xE: (8, 8, 4)}
FMT_NAME = {0: 'I4', 1: 'I8', 2: 'IA4', 3: 'IA8', 4: 'RGB565', 5: 'RGB5A3', 6: 'RGBA8', 8: 'C4', 9: 'C8', 0xA: 'C14X2', 0xE: 'CMPR'}


def level_size(w, h, fmt):
    """Bytes of one GX-tiled level (whole blocks), as the GPU (and vgx's TexLevelSize) reads it."""
    bw, bh, bpp = FMT[fmt]
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    return bx * by * bw * bh * bpp // 8


def gc_texture_size(game_fmt, w, h, levels):
    """GCTextureSize() of native/src/NL/gc/gcSwizzler.cpp (the game's own level walk, no block
    rounding): where the game finds the palette and how many bytes it copies."""
    size = 0
    while True:
        if game_fmt == 2:
            row = w >> 1
        elif game_fmt == 3:
            row = w << 2
        elif game_fmt in (6, 8, 4):
            row = w
        else:
            row = w << 1
        size += h * row
        levels -= 1
        if levels <= 0:
            break
        w >>= 1
        h >>= 1
    return size


class Tex:
    """One bundle entry's GXTextureHeader and level-0 data (and palette)."""
    def __init__(self, buf, off, size, label, bhash):
        self.label = label
        self.bhash = bhash
        self.anim = size >= 0x24 and struct.unpack_from('>I', buf, off)[0] == TEXANIM_MAGIC
        self.ok = False
        self.level0 = b''
        self.palette = b''
        self.palFmt = 2  # GX_TL_RGB5A3 (PlatTexture::Prepare)
        self.wrapS = self.wrapT = 1  # wrap lives in the material binding, not the texture: treat as repeat (pad_mode checks the seam)
        if self.anim or size < 0x20:
            return
        self.levels, self.gameFmt = struct.unpack_from('>II', buf, off)
        self.bits = tuple(buf[off + 8:off + 12])
        self.missing = buf[off + 0xC]
        self.w, self.h = struct.unpack_from('>HH', buf, off + 0xE)
        self.numEntries = struct.unpack_from('>I', buf, off + 0x14)[0]
        if self.gameFmt not in GAME_TO_GX or not (1 <= self.levels <= 13) or not (0 < self.w <= 1024 and 0 < self.h <= 1024):
            return
        self.fmt = GAME_TO_GX[self.gameFmt]
        data = off + 0x20
        n0 = level_size(self.w, self.h, self.fmt)
        self.level0 = bytes(buf[data:data + n0])
        if self.gameFmt == 8 and self.numEntries:
            po = data + gc_texture_size(self.gameFmt, self.w, self.h, self.levels)
            self.palette = bytes(buf[po:po + 2 * self.numEntries])
            if len(self.palette) != 2 * self.numEntries:
                return
        self.ok = len(self.level0) == n0 and data + n0 <= off + size


def read_rlt(b, name):
    """[Tex] of one .rlt file's bytes; labels '<name>:<index>:<hash>'."""
    magic, n = struct.unpack_from('>II', b, 0)
    if magic != RLT_MAGIC:
        raise ValueError('%s: not a PTLG bundle' % name)
    base = 16 + 16 * n
    out = []
    for i in range(n):
        h, off, size, _ = struct.unpack_from('>IIII', b, 16 + 16 * i)
        out.append(Tex(b, base + off, size, '%s:%d:%08x' % (name, i, h), h))
    return out


def string_lower_hash(s):
    """nlStringLowerHash (native/src/NL/nlString.cpp): the bundle hash of a texture name."""
    h = 0xFFFFFFFF
    for c in s.lower().encode():
        h = (h + (h << 5) + c) & 0xFFFFFFFF
    return h
