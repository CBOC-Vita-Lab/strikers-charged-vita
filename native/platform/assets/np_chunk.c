/*
 * np_chunk.c - nlChunk file walking and the header-swap pass shared by every chunk format.
 *
 * Chunk header (include/NL/nlChunk.h): u32 id, u32 size, both big-endian on disc.
 *   id bits 0..23 type, 24..27 payload alignment (log2), 31 container (payload is a chunk list).
 *   size = bytes after the 8-byte header, INCLUDING the alignment padding in front of the payload.
 *   next chunk = (header + 8 + size) rounded up to 4.
 * Ground truth for the geometry is nlChunk's inline methods; this file mirrors them exactly.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "np_internal.h"

void npa_err(NpaCtx* c, int status, const char* fmt, ...)
{
    va_list ap;
    if (c->res->status >= 0)
    {
        va_start(ap, fmt);
        vsnprintf(c->res->msg, sizeof c->res->msg, fmt, ap);
        va_end(ap);
        c->res->status = status;
    }
}

void npa_warn(NpaCtx* c, const char* fmt, ...)
{
    va_list ap;
    c->res->warnings++;
    if (c->res->msg[0] == 0)
    {
        va_start(ap, fmt);
        vsnprintf(c->res->msg, sizeof c->res->msg, fmt, ap);
        va_end(ap);
    }
}

int npa_begin(NpaCtx* c, void* data, size_t size, NpaResult* res)
{
    static NpaResult dummy;
    if (res == NULL) res = &dummy;
    memset(res, 0, sizeof *res);
    res->status = NPA_OK;
    c->res = res;
    c->base = (uint8_t*)data;
    c->end = c->base + size;
    if (data == NULL || size < 8)
    {
        res->status = NPA_ERR_ARG;
        snprintf(res->msg, sizeof res->msg, "empty or tiny buffer (%zu bytes)", size);
        return res->status;
    }
    if (!NPA_HOST_LE)
    {
        res->status = NPA_ALREADY;
        return res->status;
    }
    return NPA_OK;
}

int npa_finish(NpaCtx* c, int status)
{
    if (status < 0 && c->res->status >= 0)
    {
        c->res->status = status;
    }
    else if (status >= 0 && c->res->status == NPA_OK)
    {
        c->res->status = status;
    }
    return c->res->status;
}

static inline uint8_t* align_up_addr(uint8_t* p, uintptr_t a)
{
    uintptr_t v = (uintptr_t)p;
    return p + (((v + a - 1) & ~(a - 1)) - v);
}

int npa_chunk_geom(NpaCtx* c, uint8_t* p, uint8_t* limit, int be, NpaChunk* out)
{
    uint32_t raw, size, abits, pad;
    uint8_t* unaligned;
    uint8_t* aligned;
    uint8_t* dataend;
    uintptr_t nx;

    if (p < c->base || limit > c->end || limit < p || (size_t)(limit - p) < 8)
    {
        npa_err(c, NPA_ERR_BOUNDS, "chunk header at +0x%zx outside its range", (size_t)(p - c->base));
        return NPA_ERR_BOUNDS;
    }
    raw = be ? npa_rd32(p) : npa_hrd32(p);
    size = be ? npa_rd32(p + 4) : npa_hrd32(p + 4);
    abits = raw & 0x0F000000u;
    unaligned = p + 8;
    aligned = abits ? align_up_addr(unaligned, (uintptr_t)1 << (abits >> 24)) : unaligned;
    pad = (uint32_t)(aligned - unaligned);
    if (pad > size)
    {
        npa_err(c, NPA_ERR_FORMAT, "chunk 0x%08x at +0x%zx: alignment padding %u exceeds size %u",
                raw & 0x80FFFFFFu, (size_t)(p - c->base), pad, size);
        return NPA_ERR_FORMAT;
    }
    dataend = unaligned + size; /* may be beyond limit only for truncated tail padding */
    if (dataend > limit)
    {
        /* Tiny files (.cph with zero elements) stop before the padding the header promises. The
         * game only ever reads inside the file, so accept a payload that is entirely padding. */
        if (!(size - pad == 0 && (size_t)(dataend - limit) <= 32))
        {
            /* a container may also hang over by the same slack; its children stop at limit */
            if (!(NPA_IS_CONTAINER(raw) && (size_t)(dataend - limit) <= 8))
            {
                npa_err(c, NPA_ERR_BOUNDS, "chunk 0x%08x at +0x%zx: size 0x%x runs %zu bytes past its range",
                        raw & 0x80FFFFFFu, (size_t)(p - c->base), size, (size_t)(dataend - limit));
                return NPA_ERR_BOUNDS;
            }
        }
    }
    out->hdr = p;
    out->rawid = raw;
    out->id = raw & 0x80FFFFFFu;
    out->size = size;
    out->data = aligned;
    out->dsize = size - pad;
    out->container = NPA_IS_CONTAINER(raw);
    nx = (uintptr_t)(unaligned + size);
    out->next = unaligned + size + ((nx & 3) ? (4 - (nx & 3)) : 0);
    return 0;
}

int npa_chunks_swap_headers(NpaCtx* c, uint8_t* p, uint8_t* limit, int depth)
{
    NpaChunk k;
    int r;
    if (depth > 12)
    {
        npa_err(c, NPA_ERR_FORMAT, "chunk tree deeper than 12 levels at +0x%zx", (size_t)(p - c->base));
        return NPA_ERR_FORMAT;
    }
    while (p + 8 <= limit)
    {
        r = npa_chunk_geom(c, p, limit, 1, &k);
        if (r) return r;
        /* now host order for everything that follows */
        npa_swap32_one(p);
        npa_swap32_one(p + 4);
        c->res->chunks++;
        if (k.container && k.dsize)
        {
            uint8_t* cend = k.data + k.dsize;
            if (cend > limit) cend = limit;
            r = npa_chunks_swap_headers(c, k.data, cend, depth + 1);
            if (r) return r;
        }
        p = k.next;
    }
    return 0;
}

void npa_kids_range(NpaCtx* c, NpaKids* it, uint8_t* p, uint8_t* limit)
{
    it->c = c;
    it->p = p;
    it->limit = limit;
}

void npa_kids_begin(NpaCtx* c, NpaKids* it, const NpaChunk* parent)
{
    uint8_t* e = parent->data + parent->dsize;
    if (e > c->end) e = c->end;
    npa_kids_range(c, it, parent->data, e);
}

int npa_kids_next(NpaKids* it, NpaChunk* out)
{
    int r;
    if (it->p + 8 > it->limit) return 0;
    r = npa_chunk_geom(it->c, it->p, it->limit, 0, out);
    if (r) return -1;
    it->p = out->next;
    return 1;
}

int npa_swap32_range(NpaCtx* c, uint8_t* p, size_t n, const uint8_t* lo, const uint8_t* hi)
{
    if (p < lo || p + n * 4 > hi)
    {
        npa_err(c, NPA_ERR_BOUNDS, "32-bit run of %zu words at +0x%zx outside +0x%zx..+0x%zx", n,
                (size_t)(p - c->base), (size_t)(lo - c->base), (size_t)(hi - c->base));
        return NPA_ERR_BOUNDS;
    }
    npa_swap32_in(p, n);
    c->res->bytes_swapped += n * 4;
    return 0;
}

int npa_swap16_range(NpaCtx* c, uint8_t* p, size_t n, const uint8_t* lo, const uint8_t* hi)
{
    if (p < lo || p + n * 2 > hi)
    {
        npa_err(c, NPA_ERR_BOUNDS, "16-bit run of %zu values at +0x%zx outside +0x%zx..+0x%zx", n,
                (size_t)(p - c->base), (size_t)(lo - c->base), (size_t)(hi - c->base));
        return NPA_ERR_BOUNDS;
    }
    npa_swap16_in(p, n);
    c->res->bytes_swapped += n * 2;
    return 0;
}

void npa_chunk_swap32(NpaCtx* c, const NpaChunk* k)
{
    NPA_EXPECT(c, (k->dsize & 3) == 0, "chunk 0x%x at +0x%zx: payload %u bytes is not a multiple of 4",
               k->id, (size_t)(k->hdr - c->base), k->dsize);
    npa_swap32_range(c, k->data, k->dsize / 4, k->data, k->data + k->dsize);
}

void npa_chunk_swap16(NpaCtx* c, const NpaChunk* k)
{
    NPA_EXPECT(c, (k->dsize & 1) == 0, "chunk 0x%x at +0x%zx: payload %u bytes is not a multiple of 2",
               k->id, (size_t)(k->hdr - c->base), k->dsize);
    npa_swap16_range(c, k->data, k->dsize / 2, k->data, k->data + k->dsize);
}

int npa_swap_struct_ex(NpaCtx* c, uint8_t* obj, size_t avail, const NpaStruct* st, int partial)
{
    int i;
    if (avail < st->size && !partial)
    {
        npa_err(c, NPA_ERR_BOUNDS, "%s at +0x%zx needs %u bytes, %zu available", st->name, (size_t)(obj - c->base), st->size, avail);
        return NPA_ERR_BOUNDS;
    }
    for (i = 0; i < st->nflds; i++)
    {
        const NpaFld* f = &st->flds[i];
        uint8_t* p = obj + f->off;
        uint32_t n;
        if (f->size == 0) continue;
        if (partial && (size_t)f->off + (size_t)f->size * f->count > avail)
        {
            npa_warn(c, "%s: field at +0x%x lies beyond the %zu bytes present (file stores a shorter record than sizeof)", st->name, f->off, avail);
            continue;
        }
        if ((size_t)f->off + (size_t)f->size * f->count > st->size)
        {
            npa_err(c, NPA_ERR_FORMAT, "%s: field table overruns the struct", st->name);
            return NPA_ERR_FORMAT;
        }
        for (n = 0; n < f->count; n++, p += f->size)
        {
            if (f->size == 4) npa_swap32_one(p);
            else if (f->size == 2) npa_swap16_one(p);
            else if (f->size == 8)
            {
                uint8_t t[8];
                int b;
                memcpy(t, p, 8);
                for (b = 0; b < 8; b++) p[b] = t[7 - b];
            }
        }
        c->res->bytes_swapped += (size_t)f->size * f->count;
    }
    return 0;
}

int npa_swap_struct(NpaCtx* c, uint8_t* obj, size_t avail, const NpaStruct* st)
{
    return npa_swap_struct_ex(c, obj, avail, st, 0);
}

const NpaStruct* npa_find_struct(const NpaStruct* table, int n, const char* name)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(table[i].name, name) == 0) return &table[i];
    return NULL;
}
