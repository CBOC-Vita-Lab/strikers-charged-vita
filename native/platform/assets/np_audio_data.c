/*
 * np_audio_data.c - the two blobs of audio/nlxgs.bun that are not plain struct tables.
 *
 * 0x1200 packed registry image (NL/nlRegistry.h, RegistryOwner::Load / Relocate):
 *   u32 reserved ('NMLB'), u32 reserved (1), u32 relocation base offset, then the root container.
 *   PackedRegistryContainer: u32 marker 0xDA07AB1E (the loader overwrites it with the vtable pointer),
 *   u16 namedCount, u16 unnamedCount, type words (2 bits per entry, 16 entries per u32; named list
 *   first, then the unnamed list), named entries {u32 hash, u32 data} sorted by hash, unnamed entries u32.
 *   Value types: 0/1 plain 32-bit values, 2 string offset (relative to the relocation base), 3 child
 *   container offset (relative to this container).
 *
 * 0x23704 audio script (AudioScriptRuntime::LoadScriptData): u32 signature 'MMGB', version, entry count,
 *   byte code offset; AudioScriptEntry[] {u32 hash, u32 data offset}; AudioScriptBindingList {u16 count,
 *   u16, u32 values offset}; binding values u32[]; per entry AudioScriptSelection {u16 count, u16
 *   conditionCount, u32 values[count]} followed by conditionCount x AudioScriptCondition {u32 key, u16
 *   function, u16 count, u32 args[count]}; the byte code (np_misc.c) at the byte code offset.
 */
#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

#define REG_MARKER 0xDA07AB1Eu

static int cvt_reg_container(NpaCtx* c, uint8_t* img, size_t imgsize, uint32_t off, int depth)
{
    uint8_t* p;
    uint32_t nn, nu, tw_n, tw_u, i, words;
    uint8_t *types_n, *types_u, *entries_n, *entries_u;
    const uint8_t* lo = img;
    const uint8_t* hi = img + imgsize;
    if (depth > 16)
    {
        npa_err(c, NPA_ERR_FORMAT, "registry nesting deeper than 16");
        return NPA_ERR_FORMAT;
    }
    if ((off & 3) || (size_t)off + 8 > imgsize)
    {
        npa_err(c, NPA_ERR_BOUNDS, "registry container at +0x%x outside the image", off);
        return NPA_ERR_BOUNDS;
    }
    p = img + off;
    if (npa_rd32(p) != REG_MARKER)
    {
        npa_err(c, NPA_ERR_FORMAT, "registry container at +0x%x has marker 0x%08x", off, npa_rd32(p));
        return NPA_ERR_FORMAT;
    }
    nn = npa_rd16(p + 4);
    nu = npa_rd16(p + 6);
    tw_n = (nn + 15) / 16;
    tw_u = (nu + 15) / 16;
    words = tw_n + tw_u + 2 * nn + nu;
    if ((size_t)off + 8 + 4ull * words > imgsize)
    {
        npa_err(c, NPA_ERR_BOUNDS, "registry container at +0x%x: %u named %u unnamed entries run past the image", off, nn, nu);
        return NPA_ERR_BOUNDS;
    }
    types_n = p + 8;
    types_u = types_n + 4 * tw_n;
    entries_n = types_u + 4 * tw_u;
    entries_u = entries_n + 8 * nn;
    /* read the types in big-endian before swapping */
    {
        uint8_t* child_slots = (uint8_t*)malloc(nn + nu + 1);
        uint32_t* child_off = (uint32_t*)malloc((nn + nu + 1) * sizeof(uint32_t));
        uint32_t nc = 0;
        int rc = 0;
        if (!child_slots || !child_off) { free(child_slots); free(child_off); npa_err(c, NPA_ERR_ARG, "out of memory"); return NPA_ERR_ARG; }
        for (i = 0; i < nn; i++)
            if (((npa_rd32(types_n + 4 * (i / 16)) >> ((i % 16) * 2)) & 3) == 3) child_off[nc++] = npa_rd32(entries_n + 8 * i + 4);
        for (i = 0; i < nu; i++)
            if (((npa_rd32(types_u + 4 * (i / 16)) >> ((i % 16) * 2)) & 3) == 3) child_off[nc++] = npa_rd32(entries_u + 4 * i);
        /* swap this container */
        npa_swap32_one(p); /* marker */
        npa_swap16_one(p + 4);
        npa_swap16_one(p + 6);
        c->res->bytes_swapped += 8;
        if (npa_swap32_range(c, types_n, words, lo, hi)) rc = c->res->status;
        for (i = 0; i < nc && !rc; i++)
        {
            rc = cvt_reg_container(c, img, imgsize, off + child_off[i], depth + 1);
        }
        free(child_slots);
        free(child_off);
        return rc;
    }
}

int npa_cvt_registry_image(NpaCtx* c, const NpaChunk* k)
{
    uint8_t* img = k->data;
    if (k->dsize < 0x14)
    {
        npa_err(c, NPA_ERR_FORMAT, "registry image of %u bytes", k->dsize);
        return NPA_ERR_FORMAT;
    }
    if (npa_swap32_range(c, img, 3, img, img + k->dsize)) return c->res->status;
    /* the loader reads +8 as the relocation base offset: it must lie inside the image */
    {
        uint32_t rb = npa_hrd32(img + 8);
        NPA_EXPECT(c, rb <= k->dsize, "registry relocation base 0x%x beyond the %u byte image", rb, k->dsize);
    }
    /* the root container follows the 12 byte header; offsets of children are relative to their parent */
    {
        uint8_t* root = img + 0x0C;
        (void)root;
    }
    return cvt_reg_container(c, img, k->dsize, 0x0C, 0);
}

/* ---- audio script */

static int swap_once(NpaCtx* c, uint8_t* done, uint8_t* base, size_t basesize, uint32_t off, int size, uint32_t n)
{
    /* size 2 or 4; off in bytes; n values; marks 32-bit words as done to detect shared data */
    uint32_t i;
    if ((size_t)off + (size_t)size * n > basesize || (off & (size - 1)))
    {
        npa_err(c, NPA_ERR_BOUNDS, "audio script data +0x%x x%u of %d bytes outside the blob", off, n, size);
        return NPA_ERR_BOUNDS;
    }
    for (i = 0; i < n; i++)
    {
        uint32_t wo = off + i * (uint32_t)size;
        uint32_t wi = wo / 4;
        if (size == 4)
        {
            if (done[wi >> 3] & (1u << (wi & 7))) continue; /* shared: already converted */
            done[wi >> 3] |= (uint8_t)(1u << (wi & 7));
            npa_swap32_one(base + wo);
        }
        else
        {
            uint32_t half = wo / 2;
            (void)half;
            /* u16 pairs share a word: track per 16-bit unit in a second region (done + basesize/32 bytes) */
            uint8_t* done16 = done + (basesize / 4 + 7) / 8;
            uint32_t hi = wo / 2;
            if (done16[hi >> 3] & (1u << (hi & 7))) continue;
            done16[hi >> 3] |= (uint8_t)(1u << (hi & 7));
            npa_swap16_one(base + wo);
        }
        c->res->bytes_swapped += (size_t)size;
    }
    return 0;
}

int npa_cvt_audio_script(NpaCtx* c, const NpaChunk* k)
{
    uint8_t* d = k->data;
    size_t size = k->dsize;
    uint32_t nent, bco, bind_off, bcount, bvals, i;
    uint8_t* done;
    int rc = 0;
    if (size < 0x10 + 8)
    {
        npa_err(c, NPA_ERR_FORMAT, "audio script blob of %zu bytes", size);
        return NPA_ERR_FORMAT;
    }
    if (npa_rd32(d) != 0x4D4D4742u)
    {
        npa_err(c, NPA_ERR_FORMAT, "audio script signature 0x%08x", npa_rd32(d));
        return NPA_ERR_FORMAT;
    }
    nent = npa_rd32(d + 8);
    bco = npa_rd32(d + 12);
    bind_off = 0x10 + 8 * nent;
    if (nent > 100000 || (size_t)bind_off + 8 > size || bco > size || bco < bind_off + 8)
    {
        npa_err(c, NPA_ERR_BOUNDS, "audio script: %u entries, byte code at 0x%x in %zu bytes", nent, bco, size);
        return NPA_ERR_BOUNDS;
    }
    done = (uint8_t*)calloc((size / 4 + 7) / 8 + (size / 2 + 7) / 8 + 8, 1);
    if (!done) { npa_err(c, NPA_ERR_ARG, "out of memory"); return NPA_ERR_ARG; }
    /* read the offsets in big-endian first, then swap */
    bcount = npa_rd16(d + bind_off);
    bvals = npa_rd32(d + bind_off + 4);
    {
        uint32_t* eoff = (uint32_t*)malloc((nent + 1) * sizeof(uint32_t));
        if (!eoff) { free(done); npa_err(c, NPA_ERR_ARG, "out of memory"); return NPA_ERR_ARG; }
        for (i = 0; i < nent; i++) eoff[i] = npa_rd32(d + 0x10 + 8 * i + 4);
        if (swap_once(c, done, d, size, 0, 4, 4 + 2 * nent)) { rc = c->res->status; goto out; }
        if (swap_once(c, done, d, size, bind_off, 2, 2)) { rc = c->res->status; goto out; }
        if (swap_once(c, done, d, size, bind_off + 4, 4, 1)) { rc = c->res->status; goto out; }
        if (bcount && swap_once(c, done, d, size, bvals, 4, bcount)) { rc = c->res->status; goto out; }
        for (i = 0; i < nent; i++)
        {
            uint32_t o = eoff[i], cnt, ccnt, j;
            if (o < bind_off + 8 || (o & 3) || (size_t)o + 4 > bco)
            {
                npa_err(c, NPA_ERR_BOUNDS, "audio script entry %u data offset 0x%x", i, o);
                rc = NPA_ERR_BOUNDS;
                goto out;
            }
            /* the selection header is read BE before the swap */
            {
                uint32_t hword = npa_rd32(d + o);
                uint8_t* done16 = done + (size / 4 + 7) / 8;
                uint32_t u = o / 2;
                if ((done16[u >> 3] >> (u & 7)) & 1) continue; /* shared with an earlier entry */
                cnt = hword >> 16;
                ccnt = hword & 0xFFFF;
            }
            if (swap_once(c, done, d, size, o, 2, 2)) { rc = c->res->status; goto out; }
            if (swap_once(c, done, d, size, o + 4, 4, cnt)) { rc = c->res->status; goto out; }
            {
                uint32_t co = o + 4 + 4 * cnt;
                for (j = 0; j < ccnt; j++)
                {
                    uint32_t fcnt;
                    if ((size_t)co + 8 > bco)
                    {
                        npa_err(c, NPA_ERR_BOUNDS, "audio script entry %u condition %u runs past the data", i, j);
                        rc = NPA_ERR_BOUNDS;
                        goto out;
                    }
                    fcnt = npa_rd16(d + co + 6);
                    if (swap_once(c, done, d, size, co, 4, 1) || swap_once(c, done, d, size, co + 4, 2, 2) ||
                        swap_once(c, done, d, size, co + 8, 4, fcnt)) { rc = c->res->status; goto out; }
                    co += 8 + 4 * fcnt;
                }
            }
        }
        free(eoff);
        free(done);
        return npa_cvt_bytecode(c, d + bco, size - bco) < 0 ? c->res->status : 0;
    out:
        free(eoff);
        free(done);
        return rc;
    }
}
