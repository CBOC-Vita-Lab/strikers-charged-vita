/*
 * np_rlg.c - .rlg model files (RLGReader in src/NL/gl/glLoadModel.cpp, glxLoadModel.cpp).
 *
 * A model file is a chunk tree: root 0x8001B000 (one model set) or 0x8001B100 (several sets, used
 * by the stadium worlds), children:
 *
 *   0x1B002 matrices       nlMatrix4[] (16 floats)                       -> swap32
 *   0x1B003 models         glModel[]   {id, numPackets, packets}  0xC    -> swap32
 *   0x1B004 packets        glModelPacket[] 0x30 (see below)               -> mixed
 *   0x1B005 streams        glModelStream[] 8 {address, index, stride, id, pad} -> swap32 @0
 *   0x1B006 vertex data    GX vertex arrays                                 NOT converted (BE, GX reads it)
 *   0x1B007 index data     u16 indices                                    -> swap16 (host order: the CPU
 *                                                                           reads them; the display-list
 *                                                                           writer must emit BE, see docs)
 *   0x1B016 parameters     per-packet material parameter blocks           -> per program (np_matprog.inc)
 *   0x8001B008 skin        0x1B00A bone matrices {u32 id, 16 floats} 0x44, 0x1B00B bone maps u32[],
 *                          0x1B00C morph targets (all u32/float words)    -> swap32
 *   0x8001B200 vertex anim 0x1B201 {6 x u32 header, s32 stream ids, frame vertex data}: header + ids
 *                          swapped, frame data (GX arrays) NOT converted
 *
 * glModelPacket (0x30): 0x00 indexBuffer(offset) u32, 0x04 numVertices u32, 0x08 numUniqueVertices u16,
 * 0x0A primType s8, 0x0B numStreams u8, 0x0C streams(offset), 0x10 material program hash, 0x14 u32,
 * 0x18 matrix index, 0x1C raster state, 0x20 parameters(offset), 0x24.. runtime slots (zero on disc).
 * The loader (RLGReader::FixupModelData) turns the offsets into pointers; that stays untouched.
 */
#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

typedef struct
{
    uint32_t hash;
    uint32_t size;
    int nparams;
    int tex[8]; /* texture binding offsets, -1 terminated */
    const char* name;
} MatProg;

static const MatProg kProgs[] = {
#include "np_matprog.inc"
};

static const MatProg* find_prog(uint32_t hash)
{
    size_t i;
    for (i = 0; i < sizeof kProgs / sizeof kProgs[0]; i++)
        if (kProgs[i].hash == hash) return &kProgs[i];
    return NULL;
}

#define PACKET_SIZE 0x30
#define STREAM_SIZE 8
#define MODEL_SIZE 0xC

/* Converts the material parameter blocks of all packets. Blocks may be shared between packets, so
 * a word is swapped once (bitmap). */
static int cvt_param_blocks(NpaCtx* c, const NpaChunk* packets, const NpaChunk* params)
{
    size_t nwords = params->dsize / 4;
    uint8_t* done;
    uint32_t i, n = packets->dsize / PACKET_SIZE;
    int rc = 0;
    if (nwords == 0)
    {
        return 0;
    }
    done = (uint8_t*)calloc((nwords + 7) / 8, 1);
    if (!done)
    {
        npa_err(c, NPA_ERR_ARG, "out of memory");
        return NPA_ERR_ARG;
    }
#define IS_DONE(w) (done[(w) >> 3] & (1u << ((w) & 7)))
#define SET_DONE(w) (done[(w) >> 3] |= (uint8_t)(1u << ((w) & 7)))
    for (i = 0; i < n && !rc; i++)
    {
        const uint8_t* pk = packets->data + (size_t)i * PACKET_SIZE;
        uint32_t hash = npa_hrd32(pk + 0x10), off = npa_hrd32(pk + 0x20);
        const MatProg* mp = find_prog(hash);
        uint32_t w, nw;
        if (!mp)
        {
            npa_err(c, NPA_ERR_FORMAT, "packet %u: unknown material program 0x%08X", i, hash);
            rc = NPA_ERR_FORMAT;
            break;
        }
        if (mp->size == 0) continue;
        if ((off & 3) || (size_t)off + mp->size > params->dsize)
        {
            npa_err(c, NPA_ERR_BOUNDS, "packet %u: parameter block +0x%x size 0x%x outside the 0x%x byte parameter chunk", i, off,
                    mp->size, params->dsize);
            rc = NPA_ERR_BOUNDS;
            break;
        }
        nw = mp->size / 4;
        for (w = 0; w < nw; w++)
        {
            size_t gw = off / 4 + w;
            uint32_t boff = w * 4;
            int is_tex = 0, t;
            int tex_part = 0; /* 0: whole word, 1: first word of a binding, 2: second word of a binding */
            for (t = 0; mp->tex[t] >= 0; t++)
            {
                if ((uint32_t)mp->tex[t] == boff) { is_tex = 1; tex_part = 1; }
                else if ((uint32_t)mp->tex[t] + 4 == boff) { is_tex = 1; tex_part = 2; }
            }
            if (IS_DONE(gw)) continue;
            SET_DONE(gw);
            if (!is_tex || tex_part == 1)
            {
                npa_swap32_one(params->data + gw * 4);
                c->res->bytes_swapped += 4;
            }
            else
            {
                /* glTextureBinding second word: u16 textureIndex, u8 flags, u8 pad. Only the u16 swaps. */
                npa_swap16_one(params->data + gw * 4);
                c->res->bytes_swapped += 2;
            }
        }
    }
#undef IS_DONE
#undef SET_DONE
    free(done);
    return rc;
}

static int cvt_packets(NpaCtx* c, const NpaChunk* k)
{
    uint32_t i, n = k->dsize / PACKET_SIZE;
    NPA_EXPECT(c, k->dsize % PACKET_SIZE == 0, "packet chunk %u bytes is not a multiple of 0x30", k->dsize);
    for (i = 0; i < n; i++)
    {
        uint8_t* p = k->data + (size_t)i * PACKET_SIZE;
        const uint8_t* lo = k->data;
        const uint8_t* hi = k->data + k->dsize;
        if (npa_swap32_range(c, p + 0x00, 2, lo, hi)) return c->res->status;  /* indexBuffer, numVertices */
        if (npa_swap16_range(c, p + 0x08, 1, lo, hi)) return c->res->status;  /* numUniqueVertices; 0x0A/0x0B bytes stay */
        if (npa_swap32_range(c, p + 0x0C, 9, lo, hi)) return c->res->status;  /* streams .. runtime slots */
    }
    return 0;
}

static int cvt_streams(NpaCtx* c, const NpaChunk* k)
{
    uint32_t i, n = k->dsize / STREAM_SIZE;
    NPA_EXPECT(c, k->dsize % STREAM_SIZE == 0, "stream chunk %u bytes is not a multiple of 8", k->dsize);
    for (i = 0; i < n; i++)
        if (npa_swap32_range(c, k->data + (size_t)i * STREAM_SIZE, 1, k->data, k->data + k->dsize)) return c->res->status;
    return 0;
}

static int cvt_vertex_anim(NpaCtx* c, const NpaChunk* container)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, container);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        uint32_t nas, hdr;
        if (k.id != 0x1B201)
        {
            npa_err(c, NPA_ERR_FORMAT, "rlg vertex anim: unexpected chunk 0x%x", k.id);
            return NPA_ERR_FORMAT;
        }
        if (k.dsize < 0x18)
        {
            npa_err(c, NPA_ERR_FORMAT, "rlg vertex anim chunk too small (%u)", k.dsize);
            return NPA_ERR_FORMAT;
        }
        if (npa_swap32_range(c, k.data, 6, k.data, k.data + k.dsize)) return c->res->status;
        nas = npa_hrd32(k.data + 0x14);
        hdr = 0x18 + 4 * nas;
        if (hdr > k.dsize)
        {
            npa_err(c, NPA_ERR_BOUNDS, "rlg vertex anim: %u stream ids do not fit %u bytes", nas, k.dsize);
            return NPA_ERR_BOUNDS;
        }
        if (npa_swap32_range(c, k.data + 0x18, nas, k.data, k.data + k.dsize)) return c->res->status;
        /* vertex frames after the ids stay big-endian (GX arrays) */
    }
    return r < 0 ? c->res->status : 0;
}

static int cvt_skin(NpaCtx* c, const NpaChunk* container)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, container);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x1B009:
        case 0x1B00A: /* bone matrices {u32 bone id, nlMatrix4} */
            NPA_EXPECT(c, k.id == 0x1B009 || k.dsize % 0x44 == 0, "skin bone matrices %u bytes not a multiple of 0x44", k.dsize);
            npa_chunk_swap32(c, &k);
            break;
        case 0x1B00B: /* bone map: u32 ids */
        case 0x1B00C: /* morph targets: counts, ids, nlVector3 deltas + index */
            npa_chunk_swap32(c, &k);
            break;
        default:
            npa_err(c, NPA_ERR_FORMAT, "rlg skin: unexpected chunk 0x%x", k.id);
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

int npa_cvt_rlg_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k, packets = {0}, params = {0};
    int r, have_packets = 0, have_params = 0;

    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x1B002: npa_chunk_swap32(c, &k); break;
        case 0x1B003:
            NPA_EXPECT(c, k.dsize % MODEL_SIZE == 0, "model chunk %u bytes is not a multiple of 0xC", k.dsize);
            npa_chunk_swap32(c, &k);
            break;
        case 0x1B004:
            packets = k; have_packets = 1;
            if (cvt_packets(c, &k)) return c->res->status;
            break;
        case 0x1B005:
            if (cvt_streams(c, &k)) return c->res->status;
            break;
        case 0x1B006: break; /* vertex arrays stay big-endian */
        case 0x1B007: npa_chunk_swap16(c, &k); break;
        case 0x1B016: params = k; have_params = 1; break;
        case 0x8001B008:
            if (cvt_skin(c, &k)) return c->res->status;
            break;
        case 0x8001B200:
            if (cvt_vertex_anim(c, &k)) return c->res->status;
            break;
        default:
            npa_err(c, NPA_ERR_FORMAT, "rlg: unexpected chunk 0x%x at +0x%zx", k.id, (size_t)(k.hdr - c->base));
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    if (r < 0) return c->res->status;
    if (have_packets && have_params)
        return cvt_param_blocks(c, &packets, &params);
    if (have_packets && packets.dsize && !have_params)
        npa_warn(c, "rlg has packets but no parameter chunk");
    return 0;
}

int npa_cvt_rlg_multi_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x8001B000u) r = npa_cvt_rlg_chunk(c, &k);
        else
        {
            npa_err(c, NPA_ERR_FORMAT, "rlg set container: unexpected chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
        }
        if (r < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

int npa_convert_rlg(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    NpaKids it;
    NpaChunk k;
    int r;
    uint32_t h, b;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    h = npa_hrd32(c.base) & 0x80FFFFFFu;
    b = npa_rd32(c.base) & 0x80FFFFFFu;
    if (h == 0x8001B000u || h == 0x8001B100u) { res->status = NPA_ALREADY; return res->status; }
    if (b != 0x8001B000u && b != 0x8001B100u)
    {
        npa_err(&c, NPA_ERR_FORMAT, "rlg: unexpected first chunk id 0x%08x", b);
        return res->status;
    }
    if (npa_chunks_swap_headers(&c, c.base, c.end, 0)) return npa_finish(&c, res->status);
    npa_kids_range(&c, &it, c.base, c.end);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x8001B000u) r = npa_cvt_rlg_chunk(&c, &k);
        else if (k.id == 0x8001B100u) r = npa_cvt_rlg_multi_chunk(&c, &k);
        else
        {
            npa_err(&c, NPA_ERR_FORMAT, "rlg: unexpected top-level chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
        }
        if (r < 0) break;
    }
    return npa_finish(&c, r < 0 ? c.res->status : 0);
}
