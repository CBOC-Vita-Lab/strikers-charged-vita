/*
 * np_world.c - stadium world files (gameworld.res / gameworld.tmp, both NLG zlib containers).
 *
 *   gameworld.res  root 0x80000001: 0x80018000 hierarchies, 0x80017000 animations, 0x26000 world
 *                  object stream, (0x80026100 visibility tree: absent in every shipped stadium)
 *   gameworld.tmp  root 0x80000001: 0x24100 texture bundle, 0x8001B100 model sets (RLG)
 *
 * The 0x26000 object stream (World::LoadObjects / World::CreateObject / BasicStadium::
 * HandleObjectCreation): u32 count + 3 words, then `count` records. Each record is the in-place
 * image of a C++ object (vtable slot at +0 is overwritten by placement new). Common prefix:
 *   +0x04 hash, +0x08 type, +0x0C creation flags (type 0x10: size of the marker), +0x10..0x1F
 *   pointer slots, +0x20 nlMatrix4 world matrix (all but type 0x102).
 * Record sizes by type are fixed by the loader (table below). The exporter wrote every record as
 * an array of u32 (the unidentified padding holds PC memory words), so all words are swapped. The
 * one byte-typed identified member is WorldEffect::m_bAlwaysVisible at +0x9C, whose byte stays
 * where the Wii read it (tools/assets/layout lists every byte-typed member of the 19 classes).
 * Type 0x10 is a parent marker: +0x20.. holds WorldAnimBinding[] and animation hash u32[].
 */
#include <string.h>

#include "np_internal.h"

typedef struct { uint32_t type, size; } ObjType;
static const ObjType kObjTypes[] = {
    { 0x101, 0x70 }, { 0x102, 0x30 }, { 0x103, 0x90 }, { 0x104, 0x60 }, { 0x106, 0x90 }, { 0x107, 0x80 },
    { 0x108, 0x70 }, { 0x109, 0xA0 }, { 0x10000, 0x90 }, { 0x10001, 0x80 }, { 0x10002, 0x90 }, { 0x10003, 0x90 },
    { 0x10004, 0x80 }, { 0x10005, 0x80 }, { 0x10006, 0x70 }, { 0x10007, 0x70 }, { 0x10008, 0x80 }, { 0x10009, 0x80 },
    { 0x1000A, 0x90 }
};

static int obj_size(uint32_t type)
{
    size_t i;
    for (i = 0; i < sizeof kObjTypes / sizeof kObjTypes[0]; i++)
        if (kObjTypes[i].type == type) return (int)kObjTypes[i].size;
    return -1;
}

static int cvt_world_objects(NpaCtx* c, const NpaChunk* k)
{
    uint8_t *p = k->data, *end = k->data + k->dsize;
    uint32_t count, i;
    if (k->dsize < 0x10)
    {
        npa_err(c, NPA_ERR_FORMAT, "world object chunk too small (%u)", k->dsize);
        return NPA_ERR_FORMAT;
    }
    count = npa_rd32(p);
    if (npa_swap32_range(c, p, 4, k->data, end)) return c->res->status;
    p += 0x10;
    for (i = 0; i < count; i++)
    {
        uint32_t type, size;
        int sz;
        if (p + 0x10 > end)
        {
            npa_err(c, NPA_ERR_BOUNDS, "world object %u/%u starts at the end of the stream", i, count);
            return NPA_ERR_BOUNDS;
        }
        type = npa_rd32(p + 8);
        if (type == 0x10)
        {
            size = npa_rd32(p + 0xC);
            if (size < 0x20 || (size & 3) || p + size > end)
            {
                npa_err(c, NPA_ERR_BOUNDS, "world parent marker %u has size 0x%x", i, size);
                return NPA_ERR_BOUNDS;
            }
        }
        else
        {
            sz = obj_size(type);
            if (sz < 0)
            {
                npa_err(c, NPA_ERR_FORMAT, "world object %u has unknown type 0x%x", i, type);
                return NPA_ERR_FORMAT;
            }
            size = (uint32_t)sz;
            if (p + size > end)
            {
                npa_err(c, NPA_ERR_BOUNDS, "world object %u (type 0x%x) runs past the stream", i, type);
                return NPA_ERR_BOUNDS;
            }
        }
        if (type == 0x109)
        {
            /* WorldEffect: m_bAlwaysVisible is a byte at +0x9C, keep it where the Wii read it */
            if (npa_swap32_range(c, p, 0x9C / 4, k->data, end)) return c->res->status;
        }
        else if (npa_swap32_range(c, p, size / 4, k->data, end)) return c->res->status;
        p += size;
    }
    NPA_EXPECT(c, p == end, "world object stream: %u records end %zd bytes before the chunk end", count, (ptrdiff_t)(end - p));
    return 0;
}

static int cvt_visibility(NpaCtx* c, const NpaChunk* k)
{
    /* 0x80026100: a tree of chunks, each payload one WorldVisibilityNode (all 32-bit) */
    NpaKids it;
    NpaChunk s;
    int r;
    npa_kids_begin(c, &it, k);
    while ((r = npa_kids_next(&it, &s)) > 0)
        npa_chunk_swap32(c, &s);
    return r < 0 ? c->res->status : 0;
}

static int cvt_world_res_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x80018000u: r = npa_cvt_shier_chunk(c, &k); break;
        case 0x80017000u: r = npa_cvt_sanim_chunk(c, &k); break;
        case 0x00026000u: r = cvt_world_objects(c, &k); break;
        case 0x80026100u: r = cvt_visibility(c, &k); break;
        default:
            npa_err(c, NPA_ERR_FORMAT, "world.res: unexpected chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
        }
        if (r < 0 || c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

static int cvt_world_tmp_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x00024100u:
            r = npa_cvt_rlt_data(c, k.data, k.dsize);
            if (r == NPA_ALREADY) r = 0;
            break;
        case 0x8001B100u: r = npa_cvt_rlg_multi_chunk(c, &k); break;
        case 0x8001B000u: r = npa_cvt_rlg_chunk(c, &k); break;
        default:
            npa_err(c, NPA_ERR_FORMAT, "world.tmp: unexpected chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
        }
        if (r < 0 || c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

static int convert_world(void* data, size_t size, NpaResult* res, int (*fn)(NpaCtx*, const NpaChunk*))
{
    NpaCtx c;
    NpaKids it;
    NpaChunk k;
    int r;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    if ((npa_hrd32(c.base) & 0x80FFFFFFu) == 0x80000001u) { res->status = NPA_ALREADY; return res->status; }
    if ((npa_rd32(c.base) & 0x80FFFFFFu) != 0x80000001u)
    {
        npa_err(&c, NPA_ERR_FORMAT, "world: unexpected root chunk id 0x%08x", npa_rd32(c.base));
        return res->status;
    }
    if (npa_chunks_swap_headers(&c, c.base, c.end, 0)) return npa_finish(&c, res->status);
    npa_kids_range(&c, &it, c.base, c.end);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id != 0x80000001u)
        {
            npa_err(&c, NPA_ERR_FORMAT, "world: unexpected top-level chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
            break;
        }
        r = fn(&c, &k);
        if (r < 0) break;
    }
    return npa_finish(&c, r < 0 ? c.res->status : 0);
}

int npa_convert_world_res(void* data, size_t size, NpaResult* res)
{
    return convert_world(data, size, res, cvt_world_res_chunk);
}

int npa_convert_world_tmp(void* data, size_t size, NpaResult* res)
{
    return convert_world(data, size, res, cvt_world_tmp_chunk);
}
