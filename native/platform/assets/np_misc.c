/*
 * np_misc.c - small non-chunk formats:
 *   .byte_code  InterpreterCore programs (Game/InterpreterCore.cpp, ByteCodeHeader)
 *   .trg        animation trigger tables with an embedded byte code tail (Triggers/BinaryTriggerFile.h)
 *   .loc        localisation tables (NL/nlLocalization.h): strings are UTF-16
 *   .res .dmn   BundleFile archives of the front end (NL/nlBundleFile.h) holding GX textures and
 *               text font descriptions
 *
 * Each converter mirrors the struct definitions of the loader named above.
 */
#include <string.h>

#include "np_internal.h"

/* ------------------------------------------------------------------------------------------- */
/* byte code                                                                                    */
/* ------------------------------------------------------------------------------------------- */

#define BC_SIGNATURE 0xE11C2112u
#define BC_HEADER_SIZE 0x48 /* 12 u32 + 6 pointer slots */

int npa_cvt_bytecode(NpaCtx* c, uint8_t* p, size_t size)
{
    uint32_t nf, tweakSize, globalSize, dataSize, codeSize, stringSize, numGlobals, numVars, i;
    uint64_t ft, tw, gi, ds, cs, ss, end;
    uint8_t* q;
    uint32_t count;
    if (size < BC_HEADER_SIZE)
    {
        npa_err(c, NPA_ERR_FORMAT, "byte code of %zu bytes is smaller than its header", size);
        return NPA_ERR_FORMAT;
    }
    if (npa_hrd32(p) == BC_SIGNATURE && npa_rd32(p) != BC_SIGNATURE) return NPA_ALREADY;
    if (npa_rd32(p) != BC_SIGNATURE)
    {
        npa_err(c, NPA_ERR_FORMAT, "byte code signature 0x%08x", npa_rd32(p));
        return NPA_ERR_FORMAT;
    }
    nf = npa_rd32(p + 0x04);
    tweakSize = npa_rd32(p + 0x08);
    globalSize = npa_rd32(p + 0x0C);
    dataSize = npa_rd32(p + 0x10);
    codeSize = npa_rd32(p + 0x14);
    stringSize = npa_rd32(p + 0x18);
    numGlobals = npa_rd32(p + 0x1C);
    numVars = npa_rd32(p + 0x28);
    ft = BC_HEADER_SIZE;
    tw = ft + 12ull * nf;
    gi = tw + tweakSize;
    ds = gi + globalSize;
    cs = ds + dataSize;
    ss = cs + codeSize;
    end = ss + stringSize;
    if (end > size || (globalSize & 3) || (dataSize & 3) || (codeSize & 1) || numVars < numGlobals)
    {
        npa_err(c, NPA_ERR_BOUNDS, "byte code sections (%llu bytes, functions %u, tweaks %u, globals %u, data %u, code %u, strings %u) vs a %zu byte file",
                (unsigned long long)end, nf, tweakSize, globalSize, dataSize, codeSize, stringSize, size);
        return NPA_ERR_BOUNDS;
    }
    /* header incl. the pointer slots the loader fills in */
    if (npa_swap32_range(c, p, BC_HEADER_SIZE / 4, p, p + size)) return c->res->status;
    /* function table: u32 hash, u32 offset, u16 frameSize, u8 numArgs, u8 flags */
    for (i = 0; i < nf; i++)
    {
        uint8_t* f = p + ft + 12ull * i;
        if (npa_swap32_range(c, f, 2, p, p + size)) return c->res->status;
        if (npa_swap16_range(c, f + 8, 1, p, p + size)) return c->res->status;
    }
    /* tweak data: per variable u8 flags, NUL terminated name, one u32 per flag bit 0..3 (unaligned) */
    q = p + tw;
    count = numVars - numGlobals;
    for (i = 0; i < count; i++)
    {
        uint8_t flags;
        int b;
        if (q >= p + gi)
        {
            npa_err(c, NPA_ERR_BOUNDS, "byte code tweak %u of %u runs past the tweak data", i, count);
            return NPA_ERR_BOUNDS;
        }
        flags = *q++;
        while (q < p + gi && *q) q++;
        if (q >= p + gi)
        {
            npa_err(c, NPA_ERR_BOUNDS, "byte code tweak %u: unterminated name", i);
            return NPA_ERR_BOUNDS;
        }
        q++;
        for (b = 0; b < 4; b++)
            if (flags & (1u << b))
            {
                if (q + 4 > p + gi)
                {
                    npa_err(c, NPA_ERR_BOUNDS, "byte code tweak %u: value runs past the tweak data", i);
                    return NPA_ERR_BOUNDS;
                }
                npa_swap32_one(q);
                c->res->bytes_swapped += 4;
                q += 4;
            }
    }
    NPA_EXPECT(c, q == p + gi, "byte code tweak data: parse ends %lld bytes before the section end", (long long)(p + gi - q));
    if (npa_swap32_range(c, p + gi, globalSize / 4, p, p + size)) return c->res->status;
    if (npa_swap32_range(c, p + ds, dataSize / 4, p, p + size)) return c->res->status;
    if (npa_swap16_range(c, p + cs, codeSize / 2, p, p + size)) return c->res->status;
    /* string segment: bytes */
    return 0;
}

int npa_convert_bytecode(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    int r;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    r = npa_cvt_bytecode(&c, c.base, size);
    if (r == NPA_ALREADY) { res->status = NPA_ALREADY; return NPA_ALREADY; }
    return npa_finish(&c, r < 0 ? res->status : 0);
}

/* ------------------------------------------------------------------------------------------- */
/* .trg                                                                                         */
/* ------------------------------------------------------------------------------------------- */

#define TRG_HEADER_SIZE 12
#define TRG_ANIM_SIZE 8
#define TRG_TRIGGER_SIZE 12

int npa_convert_trg(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    uint8_t* p;
    uint32_t nanim, bco, i, ntrig;
    int r;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    p = c.base;
    if (size < TRG_HEADER_SIZE || memcmp(p, "NLBT", 4) != 0)
    {
        npa_err(&c, NPA_ERR_FORMAT, "trg thumbprint is not 'NLBT'");
        return res->status;
    }
    if (npa_hrd16(p + 4) == 1 && npa_rd16(p + 4) != 1) { res->status = NPA_ALREADY; return NPA_ALREADY; }
    if (npa_rd16(p + 4) != 1)
    {
        npa_err(&c, NPA_ERR_FORMAT, "trg version %u", npa_rd16(p + 4));
        return res->status;
    }
    nanim = npa_rd16(p + 6);
    bco = npa_rd16(p + 8);
    if (bco < TRG_HEADER_SIZE + TRG_ANIM_SIZE * nanim || bco > size || (bco - TRG_HEADER_SIZE - TRG_ANIM_SIZE * nanim) % TRG_TRIGGER_SIZE)
    {
        npa_err(&c, NPA_ERR_BOUNDS, "trg: %u animations and byte code at 0x%x do not fit a %zu byte file", nanim, bco, size);
        return res->status;
    }
    ntrig = (bco - TRG_HEADER_SIZE - TRG_ANIM_SIZE * nanim) / TRG_TRIGGER_SIZE;
    if (npa_swap16_range(&c, p + 4, 4, p, p + size)) return res->status;
    for (i = 0; i < nanim; i++)
    {
        uint8_t* a = p + TRG_HEADER_SIZE + TRG_ANIM_SIZE * i;
        uint32_t tc = npa_rd16(a + 4), to = npa_rd16(a + 6);
        if (npa_swap32_range(&c, a, 1, p, p + size) || npa_swap16_range(&c, a + 4, 2, p, p + size)) return res->status;
        if (tc + to > ntrig) NPA_EXPECT(&c, 0, "trg animation %u: triggers %u+%u exceed the %u records", i, to, tc, ntrig);
    }
    for (i = 0; i < ntrig; i++)
        if (npa_swap32_range(&c, p + TRG_HEADER_SIZE + TRG_ANIM_SIZE * nanim + TRG_TRIGGER_SIZE * i, 3, p, p + size)) return res->status;
    if (bco < size)
    {
        r = npa_cvt_bytecode(&c, p + bco, size - bco);
        if (r < 0) return res->status;
    }
    return npa_finish(&c, 0);
}

/* ------------------------------------------------------------------------------------------- */
/* .loc                                                                                         */
/* ------------------------------------------------------------------------------------------- */

#define LOC_HEADER_SIZE 20 /* char[4], version, language, count, flags */

int npa_convert_loc(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    uint8_t* p;
    uint32_t count;
    size_t lookup_end, tail;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    p = c.base;
    if (size < LOC_HEADER_SIZE || memcmp(p, "NLOC", 4) != 0)
    {
        npa_err(&c, NPA_ERR_FORMAT, "loc thumbprint is not 'NLOC'");
        return res->status;
    }
    if (npa_hrd32(p + 4) == 1 && npa_rd32(p + 4) != 1) { res->status = NPA_ALREADY; return NPA_ALREADY; }
    if (npa_rd32(p + 4) != 1)
    {
        npa_err(&c, NPA_ERR_FORMAT, "loc version %u", npa_rd32(p + 4));
        return res->status;
    }
    count = npa_rd32(p + 12);
    lookup_end = LOC_HEADER_SIZE + (size_t)count * 8;
    if (count > 100000 || lookup_end > size)
    {
        npa_err(&c, NPA_ERR_BOUNDS, "loc: %u strings do not fit %zu bytes", count, size);
        return res->status;
    }
    if (npa_swap32_range(&c, p + 4, 4, p, p + size)) return res->status;           /* version .. flags */
    if (npa_swap32_range(&c, p + LOC_HEADER_SIZE, count * 2, p, p + size)) return res->status; /* hash, offset */
    tail = size - lookup_end;
    NPA_EXPECT(&c, (tail & 1) == 0, "loc string area has an odd %zu bytes", tail);
    if (npa_swap16_range(&c, p + lookup_end, tail / 2, p, p + size)) return res->status; /* UTF-16 */
    return npa_finish(&c, 0);
}

/* ------------------------------------------------------------------------------------------- */
/* BundleFile (.res / .dmn) and texture entries                                                  */
/* ------------------------------------------------------------------------------------------- */

/* Convert one GX texture entry (GXTextureHeader + texels) read from a bundle. Idempotent: a header
 * whose numLevels already reads as 1..13 in host order is left alone. */
int npa_texture_entry_swap(void* entry, size_t size)
{
    uint8_t* e = (uint8_t*)entry;
    NpaResult res;
    NpaCtx c;
    uint32_t le, be;
    if (size < 0x20) return NPA_ERR_FORMAT;
    le = npa_hrd32(e);
    be = npa_rd32(e);
    if (le >= 1 && le <= 13 && !(be >= 1 && be <= 13)) return NPA_ALREADY;
    if (!(be >= 1 && be <= 13) && !(size >= 0x24 && npa_rd32(e) == 0x5F6C6669u)) return NPA_ERR_FORMAT;
    memset(&res, 0, sizeof res);
    c.res = &res;
    c.base = e;
    c.end = e + size;
    if (npa_cvt_texture_entry(&c, e, (uint32_t)size, 0)) return res.status;
    return NPA_OK;
}

/* Fix the 16-byte BundleFile header read raw from disc. Returns 1 when it was big-endian and has been
 * swapped (the directory read next needs npa_bundle_swap_directory), 0 when it was already host order. */
int npa_bundle_fix_header(void* header16)
{
    uint32_t v = npa_hrd32(header16);
    if (v == 0x20u) return 0;               /* host-order sector size 32 */
    if (npa_bswap32(v) == 0x20u)
    {
        npa_bundle_swap_header(header16);
        return 1;
    }
    return 0;                               /* not a bundle header we know: leave it */
}

void npa_bundle_swap_header(void* header16)
{
    npa_swap32_in(header16, 4);
}

void npa_bundle_swap_directory(void* dir, uint32_t numFiles)
{
    npa_swap32_in(dir, (size_t)numFiles * 3);
}

#define BUNDLE_MAXFILES 65536

int npa_convert_bundlefile(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    uint8_t* p;
    uint32_t sector, n, diroff, dataoff, i;
    uint8_t* dir;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    p = c.base;
    if (size < 16)
    {
        npa_err(&c, NPA_ERR_FORMAT, "bundle of %zu bytes", size);
        return res->status;
    }
    /* nSectorSize is 0x20 on every shipped file: a host-order 0x20 means converted */
    if (npa_hrd32(p) == 0x20 && npa_rd32(p) != 0x20) { res->status = NPA_ALREADY; return NPA_ALREADY; }
    sector = npa_rd32(p);
    n = npa_rd32(p + 4);
    diroff = npa_rd32(p + 8);
    dataoff = npa_rd32(p + 12);
    if (sector == 0 || sector > 65536 || n > BUNDLE_MAXFILES || (n && ((uint64_t)diroff * sector + 12ull * n > size || (uint64_t)dataoff * sector > size)))
    {
        npa_err(&c, NPA_ERR_FORMAT, "bundle header: sector %u files %u directory@%u data@%u in %zu bytes", sector, n, diroff, dataoff, size);
        return res->status;
    }
    dir = p + (size_t)diroff * sector;
    /* entries first (they are located through the big-endian directory), then the directory */
    for (i = 0; i < n; i++)
    {
        uint32_t block = npa_rd32(dir + 12 * i + 4), len = npa_rd32(dir + 12 * i + 8);
        uint64_t off = (uint64_t)block * sector;
        uint8_t* e = p + off;
        if (off + len > size)
        {
            npa_err(&c, NPA_ERR_BOUNDS, "bundle entry %u (hash 0x%08x): block %u length %u outside the file", i, npa_rd32(dir + 12 * i), block, len);
            return res->status;
        }
        /* textures (GXTextureHeader / texture animation) vs text (font descriptions) */
        if (len >= 0x20 && npa_rd32(e) >= 1 && npa_rd32(e) <= 13 && npa_rd32(e + 4) <= 8)
        {
            if (npa_cvt_texture_entry(&c, e, len, npa_rd32(dir + 12 * i))) return res->status;
        }
        else if (len >= 0x24 && npa_rd32(e) == 0x5F6C6669u)
        {
            if (npa_cvt_texture_entry(&c, e, len, npa_rd32(dir + 12 * i))) return res->status;
        }
        /* anything else is text and stays */
        c.res->chunks++;
    }
    npa_bundle_swap_directory(dir, n);
    c.res->bytes_swapped += 12ull * n;
    npa_bundle_swap_header(p);
    c.res->bytes_swapped += 16;
    return npa_finish(&c, 0);
}
