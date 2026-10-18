/*
 * np_anim.c - converters for the nlChunk animation family:
 *   .sanim (cSAnim, 0x80017000), .shier (cSHierarchy, 0x80018000), .cam (cCameraData, 0x8002500B),
 *   .nis (cut scenes: flat list of 0x80017000 / 0x8002500B chunks), .cph (character physics
 *   0x8001D000), animretarget .bin (AnimRetargetList 0x80017104).
 *
 * Each converter mirrors the loader that consumes the chunk (Game/SAnim.cpp, SHierarchy.cpp,
 * Camera/animcam.cpp, Physics/CharacterPhysicsElement.cpp, SAnim/AnimRetargeter.cpp). The loaders
 * overlay C++ objects on chunk payloads and fill pointer slots themselves, so only byte order
 * changes here. Chunk ids are matched, not positions, so a reordered file still converts; the
 * loader's own positional walk is what constrains the real files and the tests check them.
 */
#include <string.h>

#include "np_internal.h"

/* ------------------------------------------------------------------------------------------- */
/* cSAnim                                                                                       */
/* ------------------------------------------------------------------------------------------- */

#define SANIM_OBJECT_SIZE 0x58 /* sizeof(cSAnim): cIdentifier + counts + pointer slots */

int npa_cvt_sanim_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k, obj = {0}, props = {0};
    int have_obj = 0, have_props = 0, r;
    uint32_t numKeys, numNodes, numMorph, numRootKeys;
    uint32_t nodeIndex = 0;

    /* pass 1: the object (counts) and the node property flags decide how the key arrays convert */
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x17001) { obj = k; have_obj = 1; }
        else if (k.id == 0x17003) { props = k; have_props = 1; }
    }
    if (r < 0) return c->res->status;
    if (!have_obj || !have_props)
    {
        npa_err(c, NPA_ERR_FORMAT, "sanim at +0x%zx lacks object (0x17001) or node properties (0x17003)",
                (size_t)(root->hdr - c->base));
        return NPA_ERR_FORMAT;
    }
    NPA_EXPECT(c, obj.dsize == SANIM_OBJECT_SIZE, "sanim object is %u bytes, expected 0x58", obj.dsize);
    if (obj.dsize < SANIM_OBJECT_SIZE)
    {
        npa_err(c, NPA_ERR_FORMAT, "sanim object chunk too small (%u)", obj.dsize);
        return NPA_ERR_FORMAT;
    }
    /* cSAnim fields are all 32-bit: name pointer slot, hash, counts, pointer slots, speed, signature */
    if (npa_swap32_range(c, obj.data, SANIM_OBJECT_SIZE / 4, obj.data, obj.data + obj.dsize)) return c->res->status;
    numKeys = npa_hrd32(obj.data + 0x08);
    numNodes = npa_hrd32(obj.data + 0x0C);
    numMorph = npa_hrd32(obj.data + 0x10);
    numRootKeys = npa_hrd32(obj.data + 0x34);
    if (numNodes > 4096 || numKeys > 65536 || numRootKeys > 65536 || numMorph > 4096)
    {
        npa_err(c, NPA_ERR_FORMAT, "sanim counts implausible: keys %u nodes %u root %u morph %u", numKeys, numNodes,
                numRootKeys, numMorph);
        return NPA_ERR_FORMAT;
    }
    NPA_EXPECT(c, props.dsize == 4 * numNodes, "sanim node properties chunk %u bytes for %u nodes", props.dsize, numNodes);
    npa_chunk_swap32(c, &props);
    if (c->res->status < 0) return c->res->status;

    /* pass 2: everything else, by id */
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x17001:
        case 0x17003:
            break; /* done */
        case 0x17002: /* name */
        case 0x1700B: /* morph keys: u8 */
            break;
        case 0x17110: /* per-node weight key counts */
        case 0x17113:
        case 0x17004: /* rot / trans / scale / weight / unknown key pointer slots */
        case 0x17005:
        case 0x17006:
        case 0x17111:
        case 0x17114:
            NPA_EXPECT(c, k.dsize == 4 * numNodes, "sanim chunk 0x%x is %u bytes for %u nodes", k.id, k.dsize, numNodes);
            npa_chunk_swap32(c, &k);
            break;
        case 0x17009: /* morph key counts */
        case 0x1700A: /* morph ids */
            NPA_EXPECT(c, k.dsize == 4 * numMorph, "sanim chunk 0x%x is %u bytes for %u morph channels", k.id, k.dsize, numMorph);
            npa_chunk_swap32(c, &k);
            break;
        case 0x17007: /* root rotation: u16 per root key */
            NPA_EXPECT(c, k.dsize >= 2 * numRootKeys, "sanim root rotation %u bytes for %u keys", k.dsize, numRootKeys);
            npa_chunk_swap16(c, &k);
            break;
        case 0x17008: /* root translation: 3 floats per root key */
            /* NIS animations carry numKeys translations while the object says one root key: only the first
             * numRootKeys are read, the rest is converted with the chunk */
            NPA_EXPECT(c, k.dsize >= 12 * numRootKeys, "sanim root translation %u bytes for %u keys", k.dsize, numRootKeys);
            npa_chunk_swap32(c, &k);
            break;
        case 0x80017100: /* one node */
        {
            NpaKids sub;
            NpaChunk s;
            uint32_t pr = nodeIndex < numNodes ? npa_hrd32(props.data + 4 * nodeIndex) : 0;
            int sr;
            NPA_EXPECT(c, nodeIndex < numNodes, "sanim has more node chunks than its %u nodes", numNodes);
            npa_kids_begin(c, &sub, &k);
            while ((sr = npa_kids_next(&sub, &s)) > 0)
            {
                switch (s.id)
                {
                case 0x17101: /* rotation keys */
                {
                    uint32_t nk = (pr & 2) ? 1 : numKeys;
                    uint32_t unit, want;
                    int wide;
                    if (pr & 1) { unit = 2; wide = 1; }      /* u16 angle about Z */
                    else if (pr & 0x10) { unit = 8; wide = 1; } /* 4 x s16 */
                    else if (pr & 0x20) { unit = 6; wide = 0; } /* 12-bit packed, bytes */
                    else { unit = 4; wide = 0; }             /* 4 x s8, bytes */
                    want = nk * unit;
                    NPA_EXPECT(c, s.dsize == want, "sanim node %u rot keys %u bytes, flags 0x%x x %u keys implies %u",
                               nodeIndex, s.dsize, pr, nk, want);
                    if (wide)
                    {
                        uint32_t n = (s.dsize < want ? s.dsize : want) / 2;
                        npa_swap16_range(c, s.data, n, s.data, s.data + s.dsize);
                    }
                    break;
                }
                case 0x17102: /* translation keys: 3 floats */
                {
                    uint32_t nk = (pr & 4) ? 1 : numKeys;
                    NPA_EXPECT(c, s.dsize == 12 * nk, "sanim node %u trans keys %u bytes, implies %u", nodeIndex, s.dsize, 12 * nk);
                    npa_chunk_swap32(c, &s);
                    break;
                }
                case 0x17103: /* scale keys: 3 x u16 */
                {
                    uint32_t nk = (pr & 8) ? 1 : numKeys;
                    NPA_EXPECT(c, s.dsize == 6 * nk, "sanim node %u scale keys %u bytes, implies %u", nodeIndex, s.dsize, 6 * nk);
                    npa_chunk_swap16(c, &s);
                    break;
                }
                case 0x17112: /* channel weight keys: u8 */
                    break;
                case 0x17115: /* "unknown30" key table: never present on disc; treat as words */
                    npa_warn(c, "sanim node %u carries unknown sub-chunk 0x17115", nodeIndex);
                    npa_chunk_swap32(c, &s);
                    break;
                default:
                    npa_err(c, NPA_ERR_FORMAT, "sanim node %u: unexpected sub-chunk 0x%x", nodeIndex, s.id);
                    return NPA_ERR_FORMAT;
                }
            }
            if (sr < 0) return c->res->status;
            nodeIndex++;
            break;
        }
        default:
            npa_err(c, NPA_ERR_FORMAT, "sanim: unexpected chunk 0x%x at +0x%zx", k.id, (size_t)(k.hdr - c->base));
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    if (r < 0) return c->res->status;
    NPA_EXPECT(c, nodeIndex == numNodes, "sanim has %u node chunks, header says %u nodes", nodeIndex, numNodes);
    return 0;
}

/* ------------------------------------------------------------------------------------------- */
/* cSHierarchy                                                                                  */
/* ------------------------------------------------------------------------------------------- */

int npa_cvt_shier_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    uint32_t numNodes = 0;
    int have = 0;

    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x18001)
        {
            if (k.dsize < 0x34)
            {
                npa_err(c, NPA_ERR_FORMAT, "shier object chunk too small (%u)", k.dsize);
                return NPA_ERR_FORMAT;
            }
            /* cSHierarchy: name slot, hash, node count, pointer slots, pelvis, spine: all 32-bit */
            if (npa_swap32_range(c, k.data, 0x34 / 4, k.data, k.data + k.dsize)) return c->res->status;
            numNodes = npa_hrd32(k.data + 0x08);
            have = 1;
            break;
        }
    }
    if (r < 0) return c->res->status;
    if (!have)
    {
        npa_err(c, NPA_ERR_FORMAT, "shier lacks 0x18001");
        return NPA_ERR_FORMAT;
    }
    if (numNodes > 4096)
    {
        npa_err(c, NPA_ERR_FORMAT, "shier node count %u implausible", numNodes);
        return NPA_ERR_FORMAT;
    }
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x18001:
            break;
        case 0x18002: /* name */
        case 0x18011: /* preserve-bone-length flags: u8 per node */
            break;
        case 0x18003: /* node ids */
        case 0x18009: /* parents */
        case 0x18004: /* child counts */
        case 0x18005: /* child pointer slots */
        case 0x18006: /* push/pop (computed by the loader, zero on disc) */
        case 0x18008: /* mirror table */
            NPA_EXPECT(c, k.dsize == 4 * numNodes, "shier chunk 0x%x is %u bytes for %u nodes", k.id, k.dsize, numNodes);
            npa_chunk_swap32(c, &k);
            break;
        case 0x18007: /* flattened child index list */
            npa_chunk_swap32(c, &k);
            break;
        case 0x18010: /* translation offsets: 3 floats per node */
            NPA_EXPECT(c, k.dsize == 12 * numNodes, "shier offsets %u bytes for %u nodes", k.dsize, numNodes);
            npa_chunk_swap32(c, &k);
            break;
        default:
            npa_err(c, NPA_ERR_FORMAT, "shier: unexpected chunk 0x%x", k.id);
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

/* ------------------------------------------------------------------------------------------- */
/* camera animation                                                                             */
/* ------------------------------------------------------------------------------------------- */

int npa_cvt_cam_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x25000: /* name: char[] */
            break;
        case 0x25001: /* hash/id */
        case 0x25002: /* matrices */
        case 0x2500D:
        case 0x2500E:
        case 0x2500F:
        case 0x2500C: /* key count */
        case 0x25003: /* camera positions */
        case 0x25004: /* camera rotation quaternions */
        case 0x25005:
        case 0x25006: /* target positions */
        case 0x25007:
        case 0x25008:
        case 0x25009: /* FOV */
        case 0x2500A: /* focal length */
            npa_chunk_swap32(c, &k);
            break;
        default:
            npa_err(c, NPA_ERR_FORMAT, "cam: unexpected chunk 0x%x", k.id);
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

/* ------------------------------------------------------------------------------------------- */
/* character physics (.cph)                                                                     */
/* ------------------------------------------------------------------------------------------- */

#define CPH_ELEMENT_SIZE 0xA0 /* CharacterPhysicsElement */

static int cvt_cph_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    uint32_t count = 0;
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x1D001)
        {
            if (k.dsize >= 4)
            {
                npa_swap32_range(c, k.data, 1, k.data, k.data + k.dsize);
                count = npa_hrd32(k.data);
            }
        }
        else if (k.id == 0x1D002)
        {
            uint32_t i;
            NPA_EXPECT(c, k.dsize == count * CPH_ELEMENT_SIZE, "cph elements %u bytes for %u records", k.dsize, count);
            for (i = 0; i < count && (size_t)(i + 1) * CPH_ELEMENT_SIZE <= k.dsize; i++)
            {
                uint8_t* e = k.data + (size_t)i * CPH_ELEMENT_SIZE;
                const uint8_t* lo = k.data;
                const uint8_t* hi = k.data + k.dsize;
                /* matLocalToParent (16 floats), szName[32] bytes, hash, szParentName[32] bytes,
                 * parent hash, primitive type, width, length, height, radius, reserved */
                if (npa_swap32_range(c, e + 0x00, 16, lo, hi)) return c->res->status;
                if (npa_swap32_range(c, e + 0x60, 1, lo, hi)) return c->res->status;
                if (npa_swap32_range(c, e + 0x84, 7, lo, hi)) return c->res->status;
            }
        }
        else
        {
            npa_err(c, NPA_ERR_FORMAT, "cph: unexpected chunk 0x%x", k.id);
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

/* ------------------------------------------------------------------------------------------- */
/* animation retarget list (.bin)                                                               */
/* ------------------------------------------------------------------------------------------- */

static int cvt_retarget_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x17105) /* AnimRetargetList: cIdentifier, count, array slot */
        {
            NPA_EXPECT(c, k.dsize == 0x10, "retarget list chunk is %u bytes", k.dsize);
            npa_chunk_swap32(c, &k);
        }
        else if (k.id == 0x80017106)
        {
            NpaKids sub;
            NpaChunk s;
            int sr;
            npa_kids_begin(c, &sub, &k);
            while ((sr = npa_kids_next(&sub, &s)) > 0)
            {
                if (s.id == 0x17107) /* AnimRetarget[]: signature, bone count, unknown, map slot */
                {
                    NPA_EXPECT(c, (s.dsize & 15) == 0, "retarget array %u bytes", s.dsize);
                    npa_chunk_swap32(c, &s);
                }
                else if (s.id == 0x17108) /* bone map: s16 per bone */
                {
                    npa_chunk_swap16(c, &s);
                }
                else
                {
                    npa_err(c, NPA_ERR_FORMAT, "retarget: unexpected sub-chunk 0x%x", s.id);
                    return NPA_ERR_FORMAT;
                }
            }
            if (sr < 0) return c->res->status;
        }
        else
        {
            npa_err(c, NPA_ERR_FORMAT, "retarget: unexpected chunk 0x%x", k.id);
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

/* ------------------------------------------------------------------------------------------- */
/* file-level entry points                                                                      */
/* ------------------------------------------------------------------------------------------- */

/* Is the first chunk header already host-order? Returns 1 host, 0 big-endian, -1 neither. */
static int root_is_host(const uint8_t* p, const uint32_t* roots, int n)
{
    uint32_t h = npa_hrd32(p) & 0x80FFFFFFu, b = npa_rd32(p) & 0x80FFFFFFu;
    int i;
    for (i = 0; i < n; i++)
        if (h == roots[i]) return 1;
    for (i = 0; i < n; i++)
        if (b == roots[i]) return 0;
    return -1;
}

typedef int (*NpaChunkFn)(NpaCtx*, const NpaChunk*);

/* One chunk list at the top of the file (a single root chunk, or several siblings for .nis). */
static int convert_chunk_file(void* data, size_t size, NpaResult* res, const uint32_t* roots, int nroots,
                              NpaChunkFn fn, uint32_t only_id1, NpaChunkFn fn1, uint32_t only_id2, NpaChunkFn fn2,
                              int many_roots)
{
    NpaCtx c;
    NpaKids it;
    NpaChunk k;
    int r, host;
    (void)many_roots;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    host = root_is_host(c.base, roots, nroots);
    if (host < 0)
    {
        npa_err(&c, NPA_ERR_FORMAT, "unexpected first chunk id 0x%08x (BE) / 0x%08x (host)", npa_rd32(c.base),
                npa_hrd32(c.base));
        return res->status;
    }
    if (host)
    {
        res->status = NPA_ALREADY;
        return res->status;
    }
    if (npa_chunks_swap_headers(&c, c.base, c.end, 0)) return npa_finish(&c, res->status);
    npa_kids_range(&c, &it, c.base, c.end);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == only_id1 && fn1) r = fn1(&c, &k);
        else if (k.id == only_id2 && fn2) r = fn2(&c, &k);
        else if (fn) r = fn(&c, &k);
        else r = 0;
        if (r < 0) break;
    }
    return npa_finish(&c, r < 0 ? c.res->status : 0);
}

int npa_convert_sanim(void* data, size_t size, NpaResult* res)
{
    static const uint32_t roots[] = { 0x80017000u };
    return convert_chunk_file(data, size, res, roots, 1, npa_cvt_sanim_chunk, 0, NULL, 0, NULL, 0);
}

int npa_convert_shier(void* data, size_t size, NpaResult* res)
{
    static const uint32_t roots[] = { 0x80018000u };
    return convert_chunk_file(data, size, res, roots, 1, npa_cvt_shier_chunk, 0, NULL, 0, NULL, 0);
}

int npa_convert_cam(void* data, size_t size, NpaResult* res)
{
    static const uint32_t roots[] = { 0x8002500Bu };
    return convert_chunk_file(data, size, res, roots, 1, npa_cvt_cam_chunk, 0, NULL, 0, NULL, 0);
}

int npa_convert_nis(void* data, size_t size, NpaResult* res)
{
    static const uint32_t roots[] = { 0x80017000u, 0x8002500Bu };
    NpaCtx c;
    NpaKids it;
    NpaChunk k;
    int r, host;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    host = root_is_host(c.base, roots, 2);
    if (host < 0)
    {
        npa_err(&c, NPA_ERR_FORMAT, "nis: unexpected first chunk id 0x%08x", npa_rd32(c.base));
        return res->status;
    }
    if (host)
    {
        res->status = NPA_ALREADY;
        return res->status;
    }
    if (npa_chunks_swap_headers(&c, c.base, c.end, 0)) return npa_finish(&c, res->status);
    npa_kids_range(&c, &it, c.base, c.end);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x80017000u) r = npa_cvt_sanim_chunk(&c, &k);
        else if (k.id == 0x8002500Bu) r = npa_cvt_cam_chunk(&c, &k);
        else
        {
            npa_err(&c, NPA_ERR_FORMAT, "nis: unexpected top-level chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
        }
        if (r < 0) break;
    }
    return npa_finish(&c, r < 0 ? c.res->status : 0);
}

int npa_convert_cph(void* data, size_t size, NpaResult* res)
{
    static const uint32_t roots[] = { 0x8001D000u };
    return convert_chunk_file(data, size, res, roots, 1, cvt_cph_chunk, 0, NULL, 0, NULL, 0);
}

int npa_convert_retarget(void* data, size_t size, NpaResult* res)
{
    static const uint32_t roots[] = { 0x80017104u };
    return convert_chunk_file(data, size, res, roots, 1, cvt_retarget_chunk, 0, NULL, 0, NULL, 0);
}
