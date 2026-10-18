/*
 * np_rlt.c - .rlt texture bundles (glEndLoadTextureBundle in src/NL/gl/glTexture.cpp) and the
 * identical payload of 0x24100 chunks inside .bun / gameworld.tmp.
 *
 *   BundleHeader   {u32 magic 'PTLG', u32 numTextures, u32 pad, u32 pad}
 *   glTexBundleDict[numTextures] {u32 hash, u32 offset, u32 fileSize, u32 pad}   offset is relative
 *                                to the first byte after the dictionary
 *   per texture: either GXTextureHeader (0x20) + GX texel data + u16 palette (CI8), or a texture
 *   animation (GLTextureAnim struct whose first word is the magic 0x5F6C6669 + GLAnimTex[]).
 *
 * Converted: header, dictionary, GXTextureHeader fields, texture-animation structs. NOT converted:
 * the texel data and palettes (GX native layout, big-endian, consumed by the GX layer).
 *
 * GXTextureHeader: 0x00 numLevels u32, 0x04 format u32, 0x08 numBits[4] u8, 0x0C missingTexture u8,
 * 0x0D pad, 0x0E width u16, 0x10 height u16, 0x12 pad[2], 0x14 numEntries u32, 0x18 pad[2] u32.
 * GLTextureAnim (0x24): 0x00 m_nFrame (= magic on disc), 0x04 hash, 0x08 numTextures, 0x0C mode,
 * 0x10 playDir, 0x14 bool paused + pad[3] (byte fields: untouched), 0x18 textureIndex, 0x1C time,
 * 0x20 anim table slot; then GLAnimTex[numTextures] {u32 handle, f32 time}.
 */
#include <string.h>

#include "np_internal.h"

#define RLT_MAGIC 0x50544C47u /* 'PTLG' */
#define TEXANIM_MAGIC 0x5F6C6669u
#define TEXHDR_SIZE 0x20
#define TEXANIM_SIZE 0x24

int npa_cvt_texture_entry(NpaCtx* c, uint8_t* e, uint32_t size, uint32_t hash)
{
    const uint8_t *lo = e, *hi = e + size;
    if (size >= TEXANIM_SIZE && npa_rd32(e) == TEXANIM_MAGIC)
    {
        uint32_t n, i;
        /* words 0..4, [byte flag word untouched], 0x18, 0x1C, 0x20 */
        if (npa_swap32_range(c, e + 0x00, 5, lo, hi)) return c->res->status;
        if (npa_swap32_range(c, e + 0x18, 3, lo, hi)) return c->res->status;
        n = npa_hrd32(e + 0x08);
        if ((size_t)TEXANIM_SIZE + (size_t)n * 8 > size)
        {
            npa_err(c, NPA_ERR_BOUNDS, "texture anim 0x%08x: %u frames do not fit %u bytes", hash, n, size);
            return NPA_ERR_BOUNDS;
        }
        for (i = 0; i < n; i++)
            if (npa_swap32_range(c, e + TEXANIM_SIZE + 8 * i, 2, lo, hi)) return c->res->status;
        return 0;
    }
    if (size < TEXHDR_SIZE)
    {
        npa_err(c, NPA_ERR_FORMAT, "texture 0x%08x: entry of %u bytes is smaller than its header", hash, size);
        return NPA_ERR_FORMAT;
    }
    if (npa_swap32_range(c, e + 0x00, 2, lo, hi)) return c->res->status;   /* numLevels, format */
    if (npa_swap16_range(c, e + 0x0E, 2, lo, hi)) return c->res->status;   /* width, height */
    if (npa_swap32_range(c, e + 0x14, 3, lo, hi)) return c->res->status;   /* numEntries, pad[2] */
    return 0;
}

int npa_cvt_rlt_data(NpaCtx* c, uint8_t* p, size_t size)
{
    uint32_t n, i;
    uint8_t *dict, *tex;
    size_t dictBytes;
    if (size < 16)
    {
        npa_err(c, NPA_ERR_FORMAT, "texture bundle of %zu bytes", size);
        return NPA_ERR_FORMAT;
    }
    if (npa_hrd32(p) == RLT_MAGIC && npa_rd32(p) != RLT_MAGIC) return NPA_ALREADY;
    if (npa_rd32(p) != RLT_MAGIC)
    {
        npa_err(c, NPA_ERR_FORMAT, "texture bundle magic 0x%08x is not 'PTLG'", npa_rd32(p));
        return NPA_ERR_FORMAT;
    }
    n = npa_rd32(p + 4);
    dictBytes = (size_t)n * 16;
    if (n > 65536 || 16 + dictBytes > size)
    {
        npa_err(c, NPA_ERR_BOUNDS, "texture bundle: %u textures do not fit %zu bytes", n, size);
        return NPA_ERR_BOUNDS;
    }
    if (npa_swap32_range(c, p, 4, p, p + size)) return c->res->status;
    dict = p + 16;
    tex = dict + dictBytes;
    if (npa_swap32_range(c, dict, n * 4, p, p + size)) return c->res->status;
    for (i = 0; i < n; i++)
    {
        uint32_t hash = npa_hrd32(dict + 16 * i), off = npa_hrd32(dict + 16 * i + 4), fsz = npa_hrd32(dict + 16 * i + 8);
        if ((size_t)off + fsz > (size_t)(p + size - tex))
        {
            npa_err(c, NPA_ERR_BOUNDS, "texture %u (0x%08x): offset 0x%x + size 0x%x exceeds the bundle", i, hash, off, fsz);
            return NPA_ERR_BOUNDS;
        }
        if (npa_cvt_texture_entry(c, tex + off, fsz, hash)) return c->res->status;
    }
    return 0;
}

int npa_convert_rlt(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    int r;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    r = npa_cvt_rlt_data(&c, c.base, size);
    if (r == NPA_ALREADY) { res->status = NPA_ALREADY; return NPA_ALREADY; }
    return npa_finish(&c, r < 0 ? res->status : 0);
}
