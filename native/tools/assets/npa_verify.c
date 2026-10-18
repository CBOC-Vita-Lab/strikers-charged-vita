/*
 * npa_verify.c - structural verification of converted assets. Walks the data the way the decomp
 * loaders do (positional chunk walks, pointer slots, counts) and checks that every field line
 * up: counts match payload sizes, indexes are in bounds, floats are finite and plausible,
 * quaternions decode to unit length, names are readable text (byte arrays were NOT swapped).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "np_internal.h"
#include "npa_verify.h"

typedef struct V
{
    NpaCtx c;
    NpaResult res;
    char* msg;
    size_t cap;
    int bad;
    unsigned long qkeys, qbad;
    char first_q[160];
    const uint8_t* base;
    const uint8_t* end;
} V;

static void vfail(V* v, const char* fmt, ...)
{
    va_list ap;
    if (v->bad) return;
    v->bad = 1;
    va_start(ap, fmt);
    vsnprintf(v->msg, v->cap, fmt, ap);
    va_end(ap);
}

static int finite_f(float f) { return isfinite(f); }
static float hf(const uint8_t* p) { float f; memcpy(&f, p, 4); return f; }
static int32_t hs32(const uint8_t* p) { int32_t x; memcpy(&x, p, 4); return x; }
static int16_t hs16(const uint8_t* p) { int16_t x; memcpy(&x, p, 2); return x; }

static int printable_name(const uint8_t* p, size_t cap, size_t* len)
{
    size_t i;
    for (i = 0; i < cap; i++)
    {
        if (p[i] == 0) { if (len) *len = i; return 1; }
        if (p[i] < 32 || p[i] > 126) return 0;
    }
    return 0;
}

/* Collect the direct children of a container into a small array. */
static int kids_array(V* v, const NpaChunk* parent, NpaChunk* out, int cap, int* n)
{
    NpaKids it;
    int r = 0;
    *n = 0;
    npa_kids_begin(&v->c, &it, parent);
    while (*n < cap && (r = npa_kids_next(&it, &out[*n])) > 0) (*n)++;
    if (r < 0) { vfail(v, "malformed child chunk (%s)", v->res.msg); return -1; }
    if (it.p + 8 <= it.limit) { vfail(v, "more than %d children", cap); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- sanim */

static void quat_check(V* v, const char* what, uint32_t node, uint32_t key, float x, float y, float z, float w, float tol)
{
    float n = sqrtf(x * x + y * y + z * z + w * w);
    v->qkeys++;
    if (!(fabsf(n - 1.0f) <= tol))
    {
        v->qbad++;
        if (v->qbad == 1)
            snprintf(v->first_q, sizeof v->first_q, "%s node %u key %u: length %.4f (%g %g %g %g)", what, node, key, n, x, y, z, w);
    }
}

static void v_sanim(V* v, const NpaChunk* root)
{
    NpaChunk k[64 + 4096];
    int n, i;
    uint32_t numKeys, numNodes, numMorph, numRoot, node = 0;
    const NpaChunk *obj = NULL, *props = NULL;
    static const uint32_t order[] = { 0x17001, 0x17002, 0x17110, 0x17113, 0x17004, 0x17005, 0x17006, 0x17111, 0x17114, 0x17007, 0x17008 };
    if (kids_array(v, root, k, (int)(sizeof k / sizeof k[0]), &n)) return;
    for (i = 0; i < 11; i++)
        if (i >= n || k[i].id != order[i]) { vfail(v, "sanim chunk %d is not 0x%x (loader's positional order)", i, order[i]); return; }
    obj = &k[0];
    numKeys = npa_hrd32(obj->data + 8);
    numNodes = npa_hrd32(obj->data + 0xC);
    numMorph = npa_hrd32(obj->data + 0x10);
    numRoot = npa_hrd32(obj->data + 0x34);
    if (n != 11 + (int)numNodes + 4) { vfail(v, "sanim: %d chunks for %u nodes", n, numNodes); return; }
    if (k[n - 4].id != 0x17009 || k[n - 3].id != 0x1700A || k[n - 2].id != 0x1700B || k[n - 1].id != 0x17003)
    { vfail(v, "sanim tail chunks are not 17009,1700a,1700b,17003"); return; }
    props = &k[n - 1];
    if (props->dsize != 4 * numNodes) { vfail(v, "sanim props size"); return; }
    if (k[n - 4].dsize != 4 * numMorph || k[n - 3].dsize != 4 * numMorph) { vfail(v, "sanim morph arrays vs %u channels", numMorph); return; }
    if (numKeys == 0 || numKeys > 20000) { vfail(v, "sanim numKeys %u", numKeys); return; }
    {
        float spd = hf(obj->data + 0x50);
        if (!finite_f(spd) || spd < 0 || spd > 1000) vfail(v, "sanim linear speed %g", spd);
    }
    if (k[1].dsize == 0 || !printable_name(k[1].data, k[1].dsize, NULL)) vfail(v, "sanim name not text");
    /* root keys */
    /* NIS animations store numKeys root translations while the object says one root key: only the
     * first numRoot entries are ever read, so the arrays may be larger, never smaller */
    if (k[9].dsize < 2 * numRoot || k[10].dsize < 12 * numRoot) { vfail(v, "root key arrays (%u, %u bytes) vs %u keys", k[9].dsize, k[10].dsize, numRoot); return; }
    for (i = 0; i < (int)(3 * numRoot); i++)
        if (!finite_f(hf(k[10].data + 4 * i)) || fabsf(hf(k[10].data + 4 * i)) > 1e5f) { vfail(v, "root trans %d = %g", i, hf(k[10].data + 4 * i)); return; }
    /* nodes */
    for (node = 0; node < numNodes; node++)
    {
        const NpaChunk* nc = &k[11 + node];
        NpaKids sub;
        NpaChunk s;
        uint32_t pr = npa_hrd32(props->data + 4 * node);
        int r;
        if (nc->id != 0x80017100) { vfail(v, "sanim node %u chunk id 0x%x", node, nc->id); return; }
        npa_kids_begin(&v->c, &sub, nc);
        while ((r = npa_kids_next(&sub, &s)) > 0)
        {
            uint32_t j, nk;
            if (s.id == 0x17101)
            {
                nk = (pr & 2) ? 1 : numKeys;
                if (pr & 1)
                {
                    if (s.dsize != 2 * nk) { vfail(v, "node %u zrot size %u keys %u", node, s.dsize, nk); return; }
                }
                else if (pr & 0x10)
                {
                    if (s.dsize != 8 * nk) { vfail(v, "node %u rot16 size %u keys %u", node, s.dsize, nk); return; }
                    for (j = 0; j < nk; j++)
                    {
                        const uint8_t* q = s.data + 8 * j;
                        quat_check(v, "rot16", node, j, hs16(q) / 32768.0f, hs16(q + 2) / 32768.0f, hs16(q + 4) / 32768.0f, hs16(q + 6) / 32768.0f, 0.03f);
                    }
                }
                else if (pr & 0x20)
                {
                    if (s.dsize != 6 * nk) { vfail(v, "node %u rot12 size %u keys %u", node, s.dsize, nk); return; }
                    for (j = 0; j < nk; j++)
                    {
                        const uint8_t* b = s.data + 6 * j;
                        int a0 = (b[0] << 4) | (b[1] >> 4), a1 = (b[2] << 4) | (b[1] & 15);
                        int a2 = (b[3] << 4) | (b[4] >> 4), a3 = (b[5] << 4) | (b[4] & 15);
                        if (a0 & 0x800) a0 -= 0x1000; if (a1 & 0x800) a1 -= 0x1000;
                        if (a2 & 0x800) a2 -= 0x1000; if (a3 & 0x800) a3 -= 0x1000;
                        quat_check(v, "rot12", node, j, a0 / 2048.0f, a1 / 2048.0f, a2 / 2048.0f, a3 / 2048.0f, 0.06f);
                    }
                }
                else
                {
                    if (s.dsize != 4 * nk) { vfail(v, "node %u rot8 size %u keys %u", node, s.dsize, nk); return; }
                    for (j = 0; j < nk; j++)
                    {
                        const int8_t* b = (const int8_t*)s.data + 4 * j;
                        quat_check(v, "rot8", node, j, b[0] / 128.0f, b[1] / 128.0f, b[2] / 128.0f, b[3] / 128.0f, 0.12f);
                    }
                }
            }
            else if (s.id == 0x17102)
            {
                nk = (pr & 4) ? 1 : numKeys;
                if (s.dsize != 12 * nk) { vfail(v, "node %u trans size %u keys %u", node, s.dsize, nk); return; }
                for (j = 0; j < 3 * nk; j++)
                {
                    /* some constant translations are the canonical NaN 0x7FC00000 in the original data */
                    if (npa_hrd32(s.data + 4 * j) == 0x7FC00000u) continue;
                    if (!finite_f(hf(s.data + 4 * j)) || fabsf(hf(s.data + 4 * j)) > 1e5f) { vfail(v, "node %u trans value %g", node, hf(s.data + 4 * j)); return; }
                }
            }
            else if (s.id == 0x17103)
            {
                nk = (pr & 8) ? 1 : numKeys;
                if (s.dsize != 6 * nk) { vfail(v, "node %u scale size %u keys %u", node, s.dsize, nk); return; }
                for (j = 0; j < 3 * nk; j++)
                {
                    float f = npa_hrd16(s.data + 2 * j) * 0.00048828125f;
                    if (f > 40.0f) { vfail(v, "node %u scale %g", node, f); return; }
                }
            }
            else if (s.id == 0x17112) { /* u8 weights */ }
            else { vfail(v, "node %u sub-chunk 0x%x", node, s.id); return; }
        }
        if (r < 0) { vfail(v, "node %u: %s", node, v->res.msg); return; }
    }
}

/* ---------------------------------------------------------------- shier */

static void v_shier(V* v, const NpaChunk* root)
{
    NpaChunk k[16];
    int n, i;
    uint32_t nn, total = 0;
    static const uint32_t order[] = { 0x18001, 0x18002, 0x18003, 0x18009, 0x18004, 0x18005, 0x18006, 0x18007, 0x18008, 0x18010, 0x18011 };
    if (kids_array(v, root, k, 16, &n)) return;
    if (n != 11) { vfail(v, "shier has %d chunks", n); return; }
    for (i = 0; i < 11; i++)
        if (k[i].id != order[i]) { vfail(v, "shier chunk %d is 0x%x not 0x%x", i, k[i].id, order[i]); return; }
    nn = npa_hrd32(k[0].data + 8);
    if (k[2].dsize != 4 * nn || k[3].dsize != 4 * nn || k[4].dsize != 4 * nn || k[5].dsize != 4 * nn ||
        k[6].dsize != 4 * nn || k[8].dsize != 4 * nn || k[9].dsize != 12 * nn || k[10].dsize != nn)
    { vfail(v, "shier array sizes vs %u nodes", nn); return; }
    for (i = 0; i < (int)nn; i++)
    {
        int32_t par = hs32(k[3].data + 4 * i), nc = hs32(k[4].data + 4 * i), mir = hs32(k[8].data + 4 * i);
        if (par < -1 || par >= (int)nn) { vfail(v, "node %d parent %d", i, par); return; }
        if (nc < 0 || nc > (int)nn) { vfail(v, "node %d child count %d", i, nc); return; }
        if (mir < -1 || mir >= (int)nn) { vfail(v, "node %d mirror %d", i, mir); return; }
        total += (uint32_t)nc;
    }
    if (k[7].dsize != 4 * total) { vfail(v, "shier child list %u bytes for %u children", k[7].dsize, total); return; }
    for (i = 0; i < (int)total; i++)
    {
        int32_t ch = hs32(k[7].data + 4 * i);
        if (ch < 0 || ch >= (int)nn) { vfail(v, "child index %d", ch); return; }
    }
    for (i = 0; i < (int)(3 * nn); i++)
        if (!finite_f(hf(k[9].data + 4 * i)) || fabsf(hf(k[9].data + 4 * i)) > 1e4f) { vfail(v, "translation offset %g", hf(k[9].data + 4 * i)); return; }
    {
        int32_t pel = hs32(k[0].data + 0x24), sp = hs32(k[0].data + 0x28);
        if (pel < -1 || pel >= (int)nn || sp < -1 || sp >= (int)nn) vfail(v, "pelvis %d spine %d of %u", pel, sp, nn);
    }
    if (!printable_name(k[1].data, k[1].dsize, NULL)) vfail(v, "shier name not text");
}

/* ---------------------------------------------------------------- cam */

static void v_cam(V* v, const NpaChunk* root)
{
    NpaChunk k[32];
    int n, i;
    uint32_t keys = 0, j;
    if (kids_array(v, root, k, 32, &n)) return;
    if (n != 15) { vfail(v, "cam has %d chunks", n); return; }
    for (i = 0; i < n; i++)
        if (k[i].id == 0x2500C) keys = npa_hrd32(k[i].data);
    if (keys == 0 || keys > 100000) { vfail(v, "cam key count %u", keys); return; }
    for (i = 0; i < n; i++)
    {
        uint32_t want = 0;
        switch (k[i].id)
        {
        case 0x25003: case 0x25005: case 0x25006: case 0x25008: want = 12 * keys; break;
        case 0x25004: case 0x25007: want = 16 * keys; break;
        case 0x25009: case 0x2500A: want = 4 * keys; break;
        default: break;
        }
        if (want && k[i].dsize != want) { vfail(v, "cam chunk 0x%x %u bytes for %u keys", k[i].id, k[i].dsize, keys); return; }
        /* 0x25005..0x25008 hold uninitialised memory in the shoot-to-score cameras (look-at data) */
        if (want && (k[i].id == 0x25003 || k[i].id == 0x25004 || k[i].id == 0x25009 || k[i].id == 0x2500A))
            for (j = 0; j < want / 4; j++)
                if (!finite_f(hf(k[i].data + 4 * j)) || fabsf(hf(k[i].data + 4 * j)) > 1e6f) { vfail(v, "cam chunk 0x%x value %g", k[i].id, hf(k[i].data + 4 * j)); return; }
        if (k[i].id == 0x25004)
            for (j = 0; j < keys; j++)
            {
                const uint8_t* q = k[i].data + 16 * j;
                quat_check(v, "cam rot", 0, j, hf(q), hf(q + 4), hf(q + 8), hf(q + 12), 0.02f);
            }
        if (k[i].id == 0x25000 && !printable_name(k[i].data, k[i].dsize, NULL)) { vfail(v, "cam name not text"); return; }
    }
}

/* ---------------------------------------------------------------- cph */

static void v_cph(V* v, const NpaChunk* root)
{
    NpaChunk k[8];
    int n, i;
    uint32_t count = 0, j;
    if (kids_array(v, root, k, 8, &n)) return;
    if (n != 2 || k[0].id != 0x1D001 || k[1].id != 0x1D002) { vfail(v, "cph chunk layout"); return; }
    if (k[0].dsize >= 4) count = npa_hrd32(k[0].data);
    if (k[1].dsize != count * 0xA0u) { vfail(v, "cph %u bytes for %u elements", k[1].dsize, count); return; }
    for (j = 0; j < count; j++)
    {
        const uint8_t* e = k[1].data + 0xA0 * j;
        size_t l1, l2;
        if (!printable_name(e + 0x40, 32, &l1) || !printable_name(e + 0x64, 32, &l2)) { vfail(v, "cph element %u names not text", j); return; }
        if (l1 == 0) { vfail(v, "cph element %u empty name", j); return; }
        for (i = 0; i < 16; i++)
            if (!finite_f(hf(e + 4 * i)) || fabsf(hf(e + 4 * i)) > 1e4f) { vfail(v, "cph element %u matrix %g", j, hf(e + 4 * i)); return; }
        {
            uint32_t pt = npa_hrd32(e + 0x88);
            if (pt > 8) { vfail(v, "cph element %u primitive type %u", j, pt); return; }
            for (i = 0; i < 4; i++)
                if (!finite_f(hf(e + 0x8C + 4 * i)) || fabsf(hf(e + 0x8C + 4 * i)) > 1e4f) { vfail(v, "cph element %u dims %g", j, hf(e + 0x8C + 4 * i)); return; }
        }
    }
}

/* ---------------------------------------------------------------- retarget */

static void v_retarget(V* v, const NpaChunk* root)
{
    NpaChunk k[8], sub[256];
    int n, ns, i;
    uint32_t count;
    if (kids_array(v, root, k, 8, &n)) return;
    if (n != 2 || k[0].id != 0x17105 || k[1].id != 0x80017106) { vfail(v, "retarget layout"); return; }
    count = npa_hrd32(k[0].data + 8);
    if (kids_array(v, &k[1], sub, 256, &ns)) return;
    if (ns != (int)count + 1 || sub[0].id != 0x17107 || sub[0].dsize != 16 * count) { vfail(v, "retarget array chunk vs %u entries", count); return; }
    for (i = 0; i < (int)count; i++)
    {
        uint32_t nb = npa_hrd32(sub[0].data + 16 * i + 8); /* word 2 is the map length (decomp calls word 1 m_NumBones) */
        if (sub[1 + i].id != 0x17108 || sub[1 + i].dsize != ((2 * nb + 3) & ~3u)) { vfail(v, "retarget map %d is %u bytes for %u bones", i, sub[1 + i].dsize, nb); return; }
        {
            uint32_t j;
            for (j = 0; j < nb; j++)
            {
                int16_t m = hs16(sub[1 + i].data + 2 * j);
                if (m < -1 || m > 300) { vfail(v, "retarget map value %d", m); return; }
            }
        }
    }
}


/* ---------------------------------------------------------------- rlg */

typedef struct { uint32_t hash, size; int nparams; int tex[8]; const char* name; } VMatProg;
static const VMatProg kVProgs[] = {
#include "np_matprog.inc"
};
static const VMatProg* vfind_prog(uint32_t h)
{
    size_t i;
    for (i = 0; i < sizeof kVProgs / sizeof kVProgs[0]; i++)
        if (kVProgs[i].hash == h) return &kVProgs[i];
    return NULL;
}

static float bef(const uint8_t* p) { uint32_t u = npa_rd32(p); float f; memcpy(&f, &u, 4); return f; }

static void v_rlg_set(V* v, const NpaChunk* root)
{
    NpaChunk k[256];
    int n, i;
    const NpaChunk *mat = NULL, *mod = NULL, *pkt = NULL, *str = NULL, *vtx = NULL, *idx = NULL, *par = NULL;
    uint32_t nm, np, ni, totalPk = 0;
    if (kids_array(v, root, k, 256, &n)) return;
    for (i = 0; i < n; i++)
        switch (k[i].id)
        {
        case 0x1B002: mat = &k[i]; break;
        case 0x1B003: mod = &k[i]; break;
        case 0x1B004: pkt = &k[i]; break;
        case 0x1B005: str = &k[i]; break;
        case 0x1B006: vtx = &k[i]; break;
        case 0x1B007: idx = &k[i]; break;
        case 0x1B016: par = &k[i]; break;
        default: break;
        }
    if (!mat || !mod || !pkt || !str || !vtx || !idx || !par) { vfail(v, "rlg lacks one of the seven model chunks"); return; }
    if (mat->dsize % 64 || mod->dsize % 12 || pkt->dsize % 0x30 || str->dsize % 8) { vfail(v, "rlg record chunk sizes not record multiples"); return; }
    nm = mod->dsize / 12; np = pkt->dsize / 0x30; ni = idx->dsize / 2;
    for (i = 0; i < (int)(mat->dsize / 4); i++)
        if (!finite_f(hf(mat->data + 4 * i)) || fabsf(hf(mat->data + 4 * i)) > 1e6f) { vfail(v, "rlg matrix value %g", hf(mat->data + 4 * i)); return; }
    for (i = 0; i < (int)nm; i++)
    {
        uint32_t id = npa_hrd32(mod->data + 12 * i), npk = npa_hrd32(mod->data + 12 * i + 4);
        if (id == 0 || npk > 1024) { vfail(v, "rlg model %d id 0x%x numPackets %u", i, id, npk); return; }
        totalPk += npk;
    }
    if (totalPk != np) { vfail(v, "rlg models claim %u packets, packet chunk holds %u", totalPk, np); return; }
    for (i = 0; i < (int)np; i++)
    {
        const uint8_t* q = pkt->data + 0x30 * i;
        uint32_t ioff = npa_hrd32(q), nv = npa_hrd32(q + 4), nu = npa_hrd16(q + 8), soff = npa_hrd32(q + 0xC);
        uint32_t prog = npa_hrd32(q + 0x10), midx = npa_hrd32(q + 0x18), poff = npa_hrd32(q + 0x20);
        uint8_t prim = q[0xA], ns = q[0xB];
        const VMatProg* mp = vfind_prog(prog);
        uint32_t j;
        if (!mp) { vfail(v, "packet %d: program 0x%08x unknown", i, prog); return; }
        if (prim > 5 || ns == 0 || ns > 16 || nv == 0 || nu == 0 || nu > nv + 0) { vfail(v, "packet %d: prim %u streams %u verts %u unique %u", i, prim, ns, nv, nu); return; }
        if ((ioff & 1) || ioff / 2 + nv > ni) { vfail(v, "packet %d: indices +0x%x x%u outside %u indices", i, ioff, nv, ni); return; }
        for (j = 0; j < nv; j++)
        {
            uint32_t ix = npa_hrd16(idx->data + ioff + 2 * j);
            if (ix >= nu) { vfail(v, "packet %d: index %u >= %u unique vertices", i, ix, nu); return; }
        }
        if ((soff & 7) || soff + 8u * ns > str->dsize) { vfail(v, "packet %d: stream table +0x%x", i, soff); return; }
        if (poff + mp->size > par->dsize) { vfail(v, "packet %d: params +0x%x+%u > %u", i, poff, mp->size, par->dsize); return; }
        if (midx * 64u + 64u > mat->dsize) { vfail(v, "packet %d: matrix index %u", i, midx); return; }
        if (npa_hrd32(q + 0x24) != 0 || npa_hrd32(q + 0x28) != 0 || npa_hrd32(q + 0x2C) != 0) { vfail(v, "packet %d: runtime slots not zero", i); return; }
        for (j = 0; j < ns; j++)
        {
            const uint8_t* sp = str->data + soff + 8 * j;
            uint32_t addr = npa_hrd32(sp);
            uint8_t stride = sp[5], sid = sp[6];
            uint32_t u;
            if (stride == 0 || stride > 16 || sid == 0 || sid > 16) { vfail(v, "packet %d stream %u: stride %u id %u", i, j, stride, sid); return; }
            if ((size_t)addr + (size_t)stride * nu > vtx->dsize) { vfail(v, "packet %d stream %u: +0x%x stride %u x %u > %u", i, j, addr, stride, nu, vtx->dsize); return; }
            /* vertex arrays are big-endian and were NOT converted: they must still decode */
            if (sid == 1 && stride == 12)
                for (u = 0; u < nu * 3; u++)
                {
                    float f = bef(vtx->data + addr + 4 * u);
                    if (!finite_f(f) || fabsf(f) > 1e5f) { vfail(v, "packet %d: BE position %g", i, f); return; }
                }
            if (sid == 5 && stride == 16)
                for (u = 0; u < nu; u++)
                {
                    float w = bef(vtx->data + addr + 16 * u) + bef(vtx->data + addr + 16 * u + 4) + bef(vtx->data + addr + 16 * u + 8) + bef(vtx->data + addr + 16 * u + 12);
                    if (!finite_f(w) || fabsf(w - 1.0f) > 0.02f) { vfail(v, "packet %d: bone weights of vertex %u sum to %g", i, u, w); return; }
                }
        }
        /* parameter block: texture bindings carry textureIndex 0xFFFF / flags <= 3 */
        {
            int t;
            for (t = 0; mp->tex[t] >= 0; t++)
            {
                const uint8_t* tb = par->data + poff + mp->tex[t];
                if (tb[6] > 3) { vfail(v, "packet %d: texture binding flags 0x%x", i, tb[6]); return; }
            }
        }
    }
    /* skin / vertex animation containers */
    for (i = 0; i < n; i++)
    {
        if (k[i].id == 0x8001B008)
        {
            NpaChunk s[128];
            int ns2, a;
            if (kids_array(v, &k[i], s, 128, &ns2)) return;
            for (a = 0; a < ns2; a++)
            {
                uint32_t b;
                if (s[a].id == 0x1B00A)
                {
                    if (s[a].dsize % 0x44) { vfail(v, "skin bone matrices %u bytes", s[a].dsize); return; }
                    for (b = 0; b < s[a].dsize / 0x44; b++)
                    {
                        uint32_t j;
                        for (j = 1; j < 17; j++)
                        { float f = hf(s[a].data + 0x44 * b + 4 * j); if (!finite_f(f) || fabsf(f) > 1e5f) { vfail(v, "skin matrix value %g", f); return; } }
                    }
                }
                else if (s[a].id == 0x1B00C)
                {
                    uint32_t nmo = npa_hrd32(s[a].data), pos, pidx, mi;
                    uint32_t elem, npk;
                    if (nmo > 64 || 4 + 4 * nmo + 8 > s[a].dsize) { vfail(v, "skin morph header (%u morphs)", nmo); return; }
                    elem = npa_hrd32(s[a].data + 4 + 4 * nmo);
                    npk = npa_hrd32(s[a].data + 8 + 4 * nmo);
                    if (elem != 16) { vfail(v, "skin morph element size %u", elem); return; }
                    pos = 12 + 4 * nmo;
                    for (pidx = 0; pidx < npk; pidx++)
                        for (mi = 0; mi < nmo; mi++)
                        {
                            uint32_t cnt;
                            if (pos + 4 > s[a].dsize) { vfail(v, "skin morph runs out of data"); return; }
                            cnt = npa_hrd32(s[a].data + pos);
                            pos += 4;
                            if ((size_t)pos + (size_t)cnt * 16 > s[a].dsize) { vfail(v, "skin morph deltas overflow"); return; }
                            {
                                uint32_t d;
                                for (d = 0; d < cnt; d++)
                                {
                                    int32_t ix = hs32(s[a].data + pos + 16 * d + 12);
                                    if (ix < 0 || ix > 65535) { vfail(v, "skin morph delta index %d", ix); return; }
                                    if (!finite_f(hf(s[a].data + pos + 16 * d)) || fabsf(hf(s[a].data + pos + 16 * d)) > 1e4f) { vfail(v, "skin morph delta value"); return; }
                                }
                            }
                            pos += cnt * 16;
                        }
                    if (pos != s[a].dsize) { vfail(v, "skin morph chunk has %u bytes, parse consumed %u", s[a].dsize, pos); return; }
                }
            }
        }
        else if (k[i].id == 0x8001B200)
        {
            NpaChunk s[64];
            int ns2, a;
            if (kids_array(v, &k[i], s, 64, &ns2)) return;
            for (a = 0; a < ns2; a++)
            {
                uint32_t nf = npa_hrd32(s[a].data + 4), nv = npa_hrd32(s[a].data + 8), st = npa_hrd32(s[a].data + 0xC), nas = npa_hrd32(s[a].data + 0x14);
                if (s[a].dsize != 0x18 + 4 * nas + nf * nv * st) { vfail(v, "vertex anim: %u bytes for %u frames x %u verts x %u stride + %u ids", s[a].dsize, nf, nv, st, nas); return; }
                {
                    uint32_t j;
                    for (j = 0; j < nas; j++)
                    { int32_t sid = hs32(s[a].data + 0x18 + 4 * j); if (sid < 1 || sid > 8) { vfail(v, "vertex anim stream id %d", sid); return; } }
                }
            }
        }
    }
}

static void v_rlg_top(V* v, const NpaChunk* k)
{
    if (k->id == 0x8001B000u) v_rlg_set(v, k);
    else if (k->id == 0x8001B100u)
    {
        NpaKids it;
        NpaChunk s;
        int r;
        npa_kids_begin(&v->c, &it, k);
        while ((r = npa_kids_next(&it, &s)) > 0)
        {
            if (s.id != 0x8001B000u) { vfail(v, "rlg set container child 0x%x", s.id); return; }
            v_rlg_set(v, &s);
            if (v->bad) return;
        }
    }
    else vfail(v, "rlg top-level chunk 0x%x", k->id);
}

/* ---------------------------------------------------------------- rlt */

static uint32_t gc_texture_size(uint32_t format, uint32_t width, uint32_t height, uint32_t levels)
{
    uint32_t size = 0;
    for (;;)
    {
        uint32_t row;
        if (format == 2) row = width / 2;
        else if (format == 3) row = width * 4;
        else if (format == 6 || format == 8 || format == 4) row = width;
        else row = width * 2;
        size += height * row;
        if (--levels == 0) break;
        width >>= 1; height >>= 1;
    }
    return size;
}

static void v_rlt_data(V* v, const uint8_t* p, size_t size)
{
    uint32_t n, i;
    const uint8_t *dict, *tex;
    unsigned long seen = 0;
    if (size < 16 || npa_hrd32(p) != 0x50544C47u) { vfail(v, "rlt magic"); return; }
    n = npa_hrd32(p + 4);
    if (n > 65536 || 16 + 16 * (size_t)n > size) { vfail(v, "rlt dictionary of %u entries", n); return; }
    dict = p + 16; tex = dict + 16 * n;
    for (i = 0; i < n; i++)
    {
        uint32_t hash = npa_hrd32(dict + 16 * i), off = npa_hrd32(dict + 16 * i + 4), fsz = npa_hrd32(dict + 16 * i + 8);
        const uint8_t* e = tex + off;
        uint32_t j;
        (void)seen;
        for (j = 0; j < i; j++)
            if (npa_hrd32(dict + 16 * j) == hash) { vfail(v, "rlt duplicate texture hash 0x%08x", hash); return; }
        if ((size_t)off + fsz > (size_t)(p + size - tex)) { vfail(v, "rlt entry %u out of bounds", i); return; }
        if (fsz >= 0x24 && npa_hrd32(e) == 0x5F6C6669u)
        {
            uint32_t nt = npa_hrd32(e + 8);
            if (0x24 + 8 * (size_t)nt > fsz || nt > 4096) { vfail(v, "texture anim 0x%08x frames %u in %u bytes", hash, nt, fsz); return; }
            for (j = 0; j < nt; j++)
            {
                float t = hf(e + 0x24 + 8 * j + 4);
                if (!finite_f(t) || t < 0 || t > 1000) { vfail(v, "texture anim frame time %g", t); return; }
            }
            continue;
        }
        {
            uint32_t lv = npa_hrd32(e), fmt = npa_hrd32(e + 4), w = npa_hrd16(e + 0xE), h = npa_hrd16(e + 0x10), pal = npa_hrd32(e + 0x14);
            uint32_t want;
            if (fsz < 0x20 || lv == 0 || lv > 13 || fmt > 8 || w == 0 || h == 0 || w > 2048 || h > 2048) { vfail(v, "texture 0x%08x: levels %u format %u %ux%u", hash, lv, fmt, w, h); return; }
            if ((fmt == 8) != (pal != 0) || pal > 256) { vfail(v, "texture 0x%08x: format %u with %u palette entries", hash, fmt, pal); return; }
            want = 0x20 + gc_texture_size(fmt, w, h, lv) + 2 * pal;
            if (fsz < want || fsz - want > 64) { vfail(v, "texture 0x%08x: entry %u bytes, header implies %u", hash, fsz, want); return; }
        }
    }
}

/* ---------------------------------------------------------------- world */

static int v_obj_size(uint32_t type)
{
    static const uint32_t t[][2] = { {0x101,0x70},{0x102,0x30},{0x103,0x90},{0x104,0x60},{0x106,0x90},{0x107,0x80},{0x108,0x70},{0x109,0xA0},
        {0x10000,0x90},{0x10001,0x80},{0x10002,0x90},{0x10003,0x90},{0x10004,0x80},{0x10005,0x80},{0x10006,0x70},{0x10007,0x70},{0x10008,0x80},
        {0x10009,0x80},{0x1000A,0x90} };
    size_t i;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i][0] == type) return (int)t[i][1];
    return -1;
}

static void v_world_objects(V* v, const NpaChunk* k)
{
    const uint8_t *p = k->data + 0x10, *end = k->data + k->dsize;
    uint32_t count = npa_hrd32(k->data), i;
    unsigned long per_type[0x20] = { 0 };
    for (i = 0; i < count; i++)
    {
        uint32_t type, size;
        int sz;
        if (p + 0x10 > end) { vfail(v, "world object %u beyond the stream", i); return; }
        type = npa_hrd32(p + 8);
        if (type == 0x10)
        {
            size = npa_hrd32(p + 0xC);
            if (size < 0x20 || size & 3 || p + size > end) { vfail(v, "world marker %u size 0x%x", i, size); return; }
            p += size;
            continue;
        }
        sz = v_obj_size(type);
        if (sz < 0 || p + sz > end) { vfail(v, "world object %u type 0x%x", i, type); return; }
        if (type != 0x102)
        {
            /* +0x20 nlMatrix4: translation row 4th column zero, w = 1 */
            const uint8_t* m = p + 0x20;
            int j;
            for (j = 0; j < 16; j++)
                if (!finite_f(hf(m + 4 * j)) || fabsf(hf(m + 4 * j)) > 1e6f) { vfail(v, "world object %u (0x%x): matrix value %g", i, type, hf(m + 4 * j)); return; }
            if (hf(m + 0xC) != 0 || hf(m + 0x1C) != 0 || hf(m + 0x2C) != 0 || hf(m + 0x3C) != 1.0f)
            { vfail(v, "world object %u (0x%x): matrix column 4 is %g %g %g %g", i, type, hf(m + 0xC), hf(m + 0x1C), hf(m + 0x2C), hf(m + 0x3C)); return; }
        }
        if (npa_hrd32(p + 0x14) != 0xFFFFFFFFu && type != 0x102 && type != 0x103 && type != 0x10000) { /* m_nAnimNode is -1 on disc */ vfail(v, "world object %u (0x%x): anim node 0x%x", i, type, npa_hrd32(p + 0x14)); return; }
        p += sz;
        per_type[type < 0x20 ? type : 0]++;
    }
    if (p != end) vfail(v, "world objects end %zd bytes before the chunk end", (ptrdiff_t)(end - p));
}

static void v_world_root(V* v, const NpaChunk* root, int is_res)
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(&v->c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (is_res)
        {
            if (k.id == 0x80018000u) v_shier(v, &k);
            else if (k.id == 0x80017000u) v_sanim(v, &k);
            else if (k.id == 0x26000u) v_world_objects(v, &k);
            else vfail(v, "world.res chunk 0x%x", k.id);
        }
        else
        {
            if (k.id == 0x24100u) v_rlt_data(v, k.data, k.dsize);
            else if (k.id == 0x8001B100u || k.id == 0x8001B000u) v_rlg_top(v, &k);
            else vfail(v, "world.tmp chunk 0x%x", k.id);
        }
        if (v->bad) return;
    }
}

static void v_world_res_top(V* v, const NpaChunk* k) { if (k->id != 0x80000001u) vfail(v, "world root 0x%x", k->id); else v_world_root(v, k, 1); }
static void v_world_tmp_top(V* v, const NpaChunk* k) { if (k->id != 0x80000001u) vfail(v, "world root 0x%x", k->id); else v_world_root(v, k, 0); }


/* ---------------------------------------------------------------- fen */

static void v_fen(V* v, const uint8_t* p, size_t size)
{
    uint32_t dataLen, tabLen, i, ntab;
    const uint8_t *img, *tab;
    uint32_t pkgRes, nres, count = 0, lib, guard;
    if (size < 16 || memcmp(p, "FENL", 4) != 0 || npa_hrd32(p + 4) != 1) { vfail(v, "fen header"); return; }
    dataLen = npa_hrd32(p + 8); tabLen = npa_hrd32(p + 12);
    if (16 + (uint64_t)dataLen + tabLen > size) { vfail(v, "fen sizes"); return; }
    img = p + 16; tab = img + dataLen; ntab = tabLen / 4;
    for (i = 0; i < ntab; i++)
    {
        uint32_t slot = npa_hrd32(tab + 4 * i), val;
        if ((slot & 3) || slot + 4 > dataLen) { vfail(v, "fen table entry 0x%x outside the image", slot); return; }
        val = npa_hrd32(img + slot);
        if (val != 0xFFFFFFFFu && val >= dataLen) { vfail(v, "fen slot 0x%x holds offset 0x%x beyond the image", slot, val); return; }
    }
    pkgRes = npa_hrd32(img + 8); nres = npa_hrd32(img + 0x14); lib = npa_hrd32(img + 0xC);
    /* the resource ring: next pointers close the ring after m_uResourceCount nodes */
    if (pkgRes != 0xFFFFFFFFu)
    {
        uint32_t cur = pkgRes;
        do
        {
            if (cur + 0x18 > dataLen) { vfail(v, "fen resource ring leaves the image"); return; }
            if (npa_hrd32(img + cur + 8) > 2) { vfail(v, "fen resource type %u", npa_hrd32(img + cur + 8)); return; }
            count++;
            cur = npa_hrd32(img + cur);
        } while (cur != pkgRes && cur != 0xFFFFFFFFu && count <= nres + 1);
        if (count != nres) { vfail(v, "fen resource ring has %u nodes, package says %u", count, nres); return; }
    }
    /* library objects: text names, finite attributes */
    if (lib != 0xFFFFFFFFu)
    {
        uint32_t cur = lib;
        guard = 0;
        do
        {
            size_t len;
            int j;
            if (cur + 0x78 > dataLen) { vfail(v, "fen library ring leaves the image"); return; }
            if (npa_hrd32(img + cur + 0x74) > 4) { vfail(v, "fen library object type %u", npa_hrd32(img + cur + 0x74)); return; }
            if (!printable_name(img + cur + 0x54, 32, &len)) { vfail(v, "fen library object name at 0x%x is not text", cur + 0x54); return; }
            for (j = 0; j < 12; j++)
            {
                float f = hf(img + cur + 8 + 4 * j);
                if (!finite_f(f) || fabsf(f) > 1e7f) { vfail(v, "fen library object 0x%x attribute %g", cur, f); return; }
            }
            cur = npa_hrd32(img + cur);
        } while (cur != lib && cur != 0xFFFFFFFFu && ++guard < 100000);
    }
}


/* ---------------------------------------------------------------- byte code, trg, loc, bundle */

static int v_bytecode_data(V* v, const uint8_t* p, size_t size)
{
    uint32_t nf, tw, gs, dsz, csz, ssz, ng, nv, nsr, i, count;
    uint64_t ft, twoff, gi, ds, cs, ss;
    const uint8_t* q;
    if (size < 0x48 || npa_hrd32(p) != 0xE11C2112u) { vfail(v, "byte code signature"); return -1; }
    nf = npa_hrd32(p + 4); tw = npa_hrd32(p + 8); gs = npa_hrd32(p + 0xC); dsz = npa_hrd32(p + 0x10); csz = npa_hrd32(p + 0x14);
    ssz = npa_hrd32(p + 0x18); ng = npa_hrd32(p + 0x1C); nv = npa_hrd32(p + 0x28); nsr = npa_hrd32(p + 0x2C);
    ft = 0x48; twoff = ft + 12ull * nf; gi = twoff + tw; ds = gi + gs; cs = ds + dsz; ss = cs + csz;
    if (ss + ssz > size || ss + ssz + 32 < size) { vfail(v, "byte code sections end at %llu of %zu bytes", (unsigned long long)(ss + ssz), size); return -1; }
    if (nsr * 4 > gs) { vfail(v, "byte code string refs %u vs %u global bytes", nsr, gs); return -1; }
    {
        uint32_t last = 0;
        for (i = 0; i < nf; i++)
        {
            const uint8_t* f = p + ft + 12ull * i;
            uint32_t h = npa_hrd32(f), off = npa_hrd32(f + 4);
            if (i && h <= last) { vfail(v, "byte code function table not sorted at %u", i); return -1; }
            last = h;
            if (off >= csz && csz) { vfail(v, "function %u offset 0x%x beyond the code segment 0x%x", i, off, csz); return -1; }
            if (off & 1) { vfail(v, "function %u offset 0x%x is odd", i, off); return -1; }
        }
    }
    q = p + twoff; count = nv - ng;
    for (i = 0; i < count; i++)
    {
        uint8_t fl;
        int b;
        if (q >= p + gi) { vfail(v, "tweak %u beyond tweak data", i); return -1; }
        fl = *q++;
        while (q < p + gi && *q) { if (*q < 32 || *q > 126) { vfail(v, "tweak %u name is not text", i); return -1; } q++; }
        if (q >= p + gi) { vfail(v, "tweak %u name unterminated", i); return -1; }
        q++;
        for (b = 0; b < 4; b++) if (fl & (1u << b)) q += 4;
    }
    if (q != p + gi) { vfail(v, "tweak data parse ends %lld bytes off the section end", (long long)(p + gi - q)); return -1; }
    for (i = 0; i < csz / 2; i++)
        if ((npa_hrd16(p + cs + 2 * i) >> 11) > 31) { vfail(v, "code word %u", i); return -1; }
    return 0;
}

static void v_trg(V* v, const uint8_t* p, size_t size)
{
    uint32_t nanim, bco, ntrig, i;
    if (size < 12 || memcmp(p, "NLBT", 4) != 0 || npa_hrd16(p + 4) != 1) { vfail(v, "trg header"); return; }
    nanim = npa_hrd16(p + 6); bco = npa_hrd16(p + 8);
    if (bco < 12 + 8 * nanim || bco > size) { vfail(v, "trg byte code offset"); return; }
    ntrig = (bco - 12 - 8 * nanim) / 12;
    {
        uint32_t last = 0;
        for (i = 0; i < nanim; i++)
        {
            const uint8_t* a = p + 12 + 8 * i;
            uint32_t h = npa_hrd32(a), tc = npa_hrd16(a + 4), to = npa_hrd16(a + 6);
            if (i && h < last) { vfail(v, "trg animation table not sorted at %u", i); return; }
            last = h;
            if (tc + to > ntrig) { vfail(v, "trg animation %u: triggers %u+%u > %u", i, to, tc, ntrig); return; }
        }
    }
    for (i = 0; i < ntrig; i++)
    {
        const uint8_t* t = p + 12 + 8 * nanim + 12 * i;
        float fr = hf(t);
        if (!finite_f(fr) || fr < 0 || fr > 100000) { vfail(v, "trigger %u frame %g", i, fr); return; }
    }
    if (bco < size) v_bytecode_data(v, p + bco, size - bco);
}

static void v_loc(V* v, const uint8_t* p, size_t size)
{
    uint32_t count, i, tail, last = 0;
    const uint8_t *lt, *str;
    if (size < 20 || memcmp(p, "NLOC", 4) != 0 || npa_hrd32(p + 4) != 1) { vfail(v, "loc header"); return; }
    count = npa_hrd32(p + 12);
    if (20 + 8ull * count > size) { vfail(v, "loc lookup table"); return; }
    lt = p + 20; str = lt + 8 * count; tail = (uint32_t)(size - 20 - 8 * count) / 2;
    for (i = 0; i < count; i++)
    {
        uint32_t h = npa_hrd32(lt + 8 * i), off = npa_hrd32(lt + 8 * i + 4), j;
        if (i && h < last) { vfail(v, "loc lookup table not sorted at %u", i); return; }
        last = h;
        if (off >= tail) { vfail(v, "loc string %u offset %u of %u", i, off, tail); return; }
        for (j = off; j < tail && npa_hrd16(str + 2 * j); j++)
        {
            uint16_t ch = npa_hrd16(str + 2 * j);
            if (ch < 9 || (ch >= 0xDC00 && ch < 0xE000 && j == off) || (ch > 0xFFEF)) { vfail(v, "loc string %u has code unit 0x%04x", i, ch); return; }
        }
        if (j >= tail) { vfail(v, "loc string %u unterminated", i); return; }
    }
}

static void v_bundlefile(V* v, const uint8_t* p, size_t size)
{
    uint32_t sector, n, diroff, dataoff, i;
    const uint8_t* dir;
    if (size < 16) { vfail(v, "bundle tiny"); return; }
    sector = npa_hrd32(p); n = npa_hrd32(p + 4); diroff = npa_hrd32(p + 8); dataoff = npa_hrd32(p + 12);
    if (sector != 32) { vfail(v, "bundle sector size %u", sector); return; }
    if (n == 0) return;
    if ((uint64_t)diroff * sector + 12ull * n > size) { vfail(v, "bundle directory"); return; }
    dir = p + (size_t)diroff * sector;
    for (i = 0; i < n; i++)
    {
        uint32_t hash = npa_hrd32(dir + 12 * i), block = npa_hrd32(dir + 12 * i + 4), len = npa_hrd32(dir + 12 * i + 8);
        const uint8_t* e = p + (size_t)block * sector;
        if ((uint64_t)block * sector < (uint64_t)dataoff * sector || (uint64_t)block * sector + len > size) { vfail(v, "bundle entry %u block %u length %u", i, block, len); return; }
        if (len >= 0x20 && npa_hrd32(e) >= 1 && npa_hrd32(e) <= 13)
        {
            uint32_t lv = npa_hrd32(e), fmt = npa_hrd32(e + 4), w = npa_hrd16(e + 0xE), h = npa_hrd16(e + 0x10), pal = npa_hrd32(e + 0x14), want;
            if (fmt > 8 || w == 0 || h == 0 || w > 2048 || h > 2048) { vfail(v, "bundle texture 0x%08x: %ux%u fmt %u", hash, w, h, fmt); return; }
            want = 0x20 + gc_texture_size(fmt, w, h, lv) + 2 * pal;
            if (len < want || len - want > 64) { vfail(v, "bundle texture 0x%08x: %u bytes, header implies %u", hash, len, want); return; }
        }
        else if (len >= 0x24 && npa_hrd32(e) == 0x5F6C6669u) { /* texture animation */ }
        else if (!(len >= 8 && memcmp(e, "NLG Font", 8) == 0)) { vfail(v, "bundle entry %u (0x%08x) is neither texture nor font text", i, hash); return; }
    }
}


/* ---------------------------------------------------------------- bun (effects) */

static void v_effects(V* v, const NpaChunk* root)
{
    NpaKids it;
    NpaChunk k;
    int r;
    uint32_t nT = 0, nG = 0, haveT = 0, haveG = 0, templates = 0, groups = 0;
    const NpaChunk *hdr = NULL;
    NpaChunk hdrc;
    uint32_t tslots = 0, gslots = 0;
    npa_kids_begin(&v->c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id == 0x24001)
        {
            if (k.dsize != 0x18) { vfail(v, "effects header %u bytes", k.dsize); return; }
            hdrc = k; hdr = &hdrc;
            nT = npa_hrd32(k.data + 8); nG = npa_hrd32(k.data + 0x10);
            if (nT > 4096 || nG > 4096) { vfail(v, "effects counts %u templates %u groups", nT, nG); return; }
        }
        else if (k.id == 0x24025) { tslots = k.dsize; haveT = 1; }
        else if (k.id == 0x24026) { gslots = k.dsize; haveG = 1; }
        else if (k.id == 0x80024002)
        {
            NpaKids sub;
            NpaChunk s;
            int sr, nrange = 0;
            templates++;
            npa_kids_begin(&v->c, &sub, &k);
            while ((sr = npa_kids_next(&sub, &s)) > 0)
            {
                if (s.id == 0x24003)
                {
                    const uint8_t* t = s.data;
                    int j;
                    if (s.dsize != 0xDC && s.dsize != 0xE0) { vfail(v, "effects template %u bytes", s.dsize); return; }
                    if (t[0x34] > 4 || t[0x35] > 1 || t[0x36] > 2) { vfail(v, "effects template emitter %u blend %u billboard %u", t[0x34], t[0x35], t[0x36]); return; }
                    for (j = 0; j < 11; j++)
                    {
                        float f = hf(t + 4 + 4 * j);
                        if (j == 0 || j == 9 || j == 10) continue;
                        (void)f;
                    }
                    {
                        float life = hf(t + 0x04), plb = hf(t + 0x10);
                        if (!finite_f(life) || !finite_f(plb) || plb < 0 || plb > 1e8f) { vfail(v, "effects template life %g particle life %g", life, plb); return; }
                    }
                }
                else if (s.id == 0x80024004)
                {
                    NpaKids sub2;
                    NpaChunk t;
                    int tr;
                    npa_kids_begin(&v->c, &sub2, &s);
                    nrange++;
                    while ((tr = npa_kids_next(&sub2, &t)) > 0)
                    {
                        if (t.id == 0x24005)
                        {
                            uint32_t uc = npa_hrd32(t.data), nk = npa_hrd32(t.data + 12);
                            if (t.dsize != 0x14 || uc > 1 || !finite_f(hf(t.data + 4)) || !finite_f(hf(t.data + 8))) { vfail(v, "fxAnimatedRange useCurve %u base %g", uc, hf(t.data + 4)); return; }
                            if (uc && nk == 0) { vfail(v, "curve range with no keys"); return; }
                        }
                        else if (t.id == 0x24006)
                        {
                            uint32_t j;
                            if (t.dsize % 0x14) { vfail(v, "fxCurveKey array %u bytes", t.dsize); return; }
                            for (j = 0; j < t.dsize / 4; j++)
                                if (!finite_f(hf(t.data + 4 * j))) { vfail(v, "curve key value %g", hf(t.data + 4 * j)); return; }
                        }
                    }
                    if (tr < 0) { vfail(v, "effects range chunk: %s", v->res.msg); return; }
                }
            }
            if (sr < 0) { vfail(v, "effects template: %s", v->res.msg); return; }
            if (nrange != 8) { vfail(v, "effects template has %d property ranges", nrange); return; }
        }
        else if (k.id == 0x80024020)
        {
            NpaKids sub;
            NpaChunk s;
            int sr;
            uint32_t nspec = 0, nuser = 0;
            groups++;
            npa_kids_begin(&v->c, &sub, &k);
            while ((sr = npa_kids_next(&sub, &s)) > 0)
            {
                if (s.id == 0x24021) { nspec = npa_hrd32(s.data + 8); nuser = npa_hrd32(s.data + 0x14); }
            }
            if (sr < 0) { vfail(v, "effects group: %s", v->res.msg); return; }
            npa_kids_begin(&v->c, &sub, &k);
            while ((sr = npa_kids_next(&sub, &s)) > 0)
            {
                if (s.id == 0x24022)
                {
                    uint32_t j;
                    if (s.dsize != nspec * 0x58) { vfail(v, "effects group %u specs in %u bytes", nspec, s.dsize); return; }
                    for (j = 0; j < nspec; j++)
                    {
                        const uint8_t* e = s.data + 0x58 * j;
                        if (npa_hrd32(e + 4) >= nT && nT) { vfail(v, "effects spec template index %u of %u", npa_hrd32(e + 4), nT); return; }
                        if (npa_hrd32(e + 8) > 8) { vfail(v, "effects spec attach %u", npa_hrd32(e + 8)); return; }
                        if (!finite_f(hf(e + 0x10)) || !finite_f(hf(e + 0x28))) { vfail(v, "effects spec float"); return; }
                    }
                }
                else if (s.id == 0x24023 && s.dsize != nuser * 8) { vfail(v, "effects user spec sources %u bytes for %u", s.dsize, nuser); return; }
            }
        }
    }
    if (r < 0) { vfail(v, "effects: %s", v->res.msg); return; }
    if (!hdr || !haveT || !haveG) { vfail(v, "effects bundle lacks header or slot arrays"); return; }
    if (tslots != 4 * nT || gslots != 4 * nG || templates != nT || groups != nG)
        vfail(v, "effects counts: header %u/%u, slot arrays %u/%u bytes, %u templates %u groups present", nT, nG, tslots, gslots, templates, groups);
}

static void v_audio_chunk(V* v, const NpaChunk* c);

static void v_bun_top(V* v, const NpaChunk* top)
{
    NpaKids it;
    NpaChunk s;
    int r;
    if (top->id != 0x80000001u) { vfail(v, "bun root 0x%x", top->id); return; }
    npa_kids_begin(&v->c, &it, top);
    while ((r = npa_kids_next(&it, &s)) > 0)
    {
        if (s.id == 0x80024000u) v_effects(v, &s);
        else if (s.id == 0x24100u) v_rlt_data(v, s.data, s.dsize);
        else if (s.id == 0x8001B000u || s.id == 0x8001B100u) v_rlg_top(v, &s);
        else if ((s.id & 0xFFFFF000u) == 0x80023000u || s.id == 0x1200 || s.id == 0x23704) v_audio_chunk(v, &s);
        else vfail(v, "bun chunk 0x%x", s.id);
        if (v->bad) return;
    }
    if (r < 0) vfail(v, "bun: %s", v->res.msg);
}


/* ---------------------------------------------------------------- audio */

static void v_sp_table(V* v, const NpaChunk* k)
{
    /* the payload must still be BIG-endian: the SDK layer (NpSpFixEndian) swaps it, see np_audio.c */
    uint32_t n = npa_rd32(k->data), i, nad = 0;
    if (4 + 28ull * n > k->dsize) { vfail(v, "SP table: %u entries in %u bytes (payload must stay big-endian)", n, k->dsize); return; }
    for (i = 0; i < n; i++)
    {
        const uint8_t* e = k->data + 4 + 28 * i;
        uint32_t t = npa_rd32(e), sr = npa_rd32(e + 4);
        if (t > 5) { vfail(v, "SP entry %u type %u", i, t); return; }
        if (sr < 4000 || sr > 96000) { vfail(v, "SP entry %u sample rate %u", i, sr); return; }
        if (t < 2) nad++;
    }
    if (k->dsize != 4 + 28 * n + 46 * nad) { vfail(v, "SP table size %u vs %u entries + %u adpcm", k->dsize, n, nad); return; }
}

static void v_resource_bundle(V* v, const NpaChunk* root)
{
    NpaChunk k[8192];
    int n, i = 0, a;
    uint32_t cueN, voiceN, seqN, sevN, hitN, parN, g;
    const NpaChunk *cues, *voices, *seqs, *sevs;
    if (kids_array(v, root, k, 8192, &n)) return;
    if (n < 7 || k[0].id != 0x23301 || k[1].id != 0x23302 || k[2].id != 0x23303 || k[3].id != 0x23304 || k[4].id != 0x23305 || k[5].id != 0x23306 || k[6].id != 0x23307)
    { vfail(v, "resource bundle chunk order"); return; }
    cueN = npa_hrd32(k[0].data + 8); voiceN = npa_hrd32(k[0].data + 0x10); seqN = npa_hrd32(k[0].data + 0x18);
    sevN = npa_hrd32(k[0].data + 0x20); hitN = npa_hrd32(k[0].data + 0x28); parN = npa_hrd32(k[0].data + 0x30);
    if (k[1].dsize != 0x28 * cueN || k[2].dsize != 0x2C * voiceN || k[3].dsize != 0xC * seqN || k[4].dsize != 0x30 * sevN || k[5].dsize != 0x10 * hitN || k[6].dsize != 0x18 * parN)
    { vfail(v, "resource bundle arrays vs counts cues %u voices %u sequences %u sound events %u hit %u param %u", cueN, voiceN, seqN, sevN, hitN, parN); return; }
    cues = &k[1]; voices = &k[2]; seqs = &k[3]; sevs = &k[4];
    i = 7;
    for (g = 0; g < cueN; g++)
    {
        uint32_t vc = npa_hrd32(cues->data + 0x28 * g + 4), e;
        if (i >= n || k[i].id != 0x23308 || k[i].dsize != 0x14 * vc) { vfail(v, "cue %u: entries chunk (%u voices)", g, vc); return; }
        for (e = 0; e < vc; e++)
        {
            float mn = hf(k[i].data + 0x14 * e + 4), w = hf(k[i].data + 0x14 * e + 8);
            if (!finite_f(mn) || !finite_f(w)) { vfail(v, "cue %u entry %u float", g, e); return; }
        }
        i++;
    }
    for (g = 0; g < voiceN; g++)
    {
        uint32_t sc = npa_hrd32(voices->data + 0x2C * g + 0x10), rg = npa_hrd32(voices->data + 0x2C * g + 0x18);
        float vol = hf(voices->data + 0x2C * g + 4), pit = hf(voices->data + 0x2C * g + 8);
        if (!finite_f(vol) || !finite_f(pit) || fabsf(vol) > 1000 || fabsf(pit) > 10000) { vfail(v, "voice %u volume %g pitch %g", g, vol, pit); return; }
        if (i + 1 >= n || k[i].id != 0x23309 || k[i].dsize != 4 * sc || k[i + 1].id != 0x2330C || k[i + 1].dsize != 4 * rg) { vfail(v, "voice %u: sequence/rpc chunks (%u/%u)", g, sc, rg); return; }
        i += 2;
    }
    for (g = 0; g < seqN; g++)
    {
        uint32_t ec = npa_hrd32(seqs->data + 0xC * g + 4), e;
        if (i >= n || k[i].id != 0x2330A || k[i].dsize != 8 * ec) { vfail(v, "sequence %u: events chunk (%u)", g, ec); return; }
        for (e = 0; e < ec; e++)
        {
            int32_t t = hs32(k[i].data + 8 * e);
            if (t < 1 || t > 3) { vfail(v, "sequence %u event %u type %d", g, e, t); return; }
        }
        i++;
    }
    for (g = 0; g < sevN; g++)
    {
        uint32_t cc = npa_hrd32(sevs->data + 0x30 * g + 8), j;
        float f[6];
        if (i >= n || k[i].id != 0x2330B || k[i].dsize != 8 * cc) { vfail(v, "sound event %u: choices chunk (%u)", g, cc); return; }
        for (j = 0; j < 6; j++) { f[j] = hf(sevs->data + 0x30 * g + 0x18 + 4 * j); if (!finite_f(f[j])) { vfail(v, "sound event %u float %u", g, j); return; } }
        i++;
    }
    (void)a;
    if (i != n) vfail(v, "resource bundle has %d chunks, the loader's walk consumes %d", n, i);
}

static void v_reg_container(V* v, const uint8_t* img, size_t size, uint32_t off, int depth)
{
    const uint8_t* p;
    uint32_t nn, nu, tn, tu, i, last = 0;
    const uint8_t *tyn, *tyu, *en, *eu;
    if (depth > 16 || (off & 3) || (size_t)off + 8 > size) { vfail(v, "registry container +0x%x", off); return; }
    p = img + off;
    if (npa_hrd32(p) != 0xDA07AB1Eu) { vfail(v, "registry marker at +0x%x is 0x%08x", off, npa_hrd32(p)); return; }
    nn = npa_hrd16(p + 4); nu = npa_hrd16(p + 6); tn = (nn + 15) / 16; tu = (nu + 15) / 16;
    if ((size_t)off + 8 + 4ull * (tn + tu + 2 * nn + nu) > size) { vfail(v, "registry container +0x%x overflows", off); return; }
    tyn = p + 8; tyu = tyn + 4 * tn; en = tyu + 4 * tu; eu = en + 8 * nn;
    for (i = 0; i < nn; i++)
    {
        uint32_t h = npa_hrd32(en + 8 * i), ty = (npa_hrd32(tyn + 4 * (i / 16)) >> ((i % 16) * 2)) & 3;
        if (i && h < last) { vfail(v, "registry named entries not sorted at %u", i); return; }
        last = h;
        if (ty == 3) { v_reg_container(v, img, size, off + npa_hrd32(en + 8 * i + 4), depth + 1); if (v->bad) return; }
        else if (ty == 2 && npa_hrd32(en + 8 * i + 4) >= size) { vfail(v, "registry string offset"); return; }
    }
    for (i = 0; i < nu; i++)
    {
        uint32_t ty = (npa_hrd32(tyu + 4 * (i / 16)) >> ((i % 16) * 2)) & 3;
        if (ty == 3) { v_reg_container(v, img, size, off + npa_hrd32(eu + 4 * i), depth + 1); if (v->bad) return; }
    }
}

static void v_audio_script(V* v, const uint8_t* d, size_t size)
{
    uint32_t nent, bco, i, last = 0, bind_off;
    if (size < 0x18 || npa_hrd32(d) != 0x4D4D4742u) { vfail(v, "audio script signature"); return; }
    nent = npa_hrd32(d + 8); bco = npa_hrd32(d + 12); bind_off = 0x10 + 8 * nent;
    if (bco > size || bind_off + 8 > bco) { vfail(v, "audio script layout"); return; }
    for (i = 0; i < nent; i++)
    {
        uint32_t h = npa_hrd32(d + 0x10 + 8 * i), o = npa_hrd32(d + 0x10 + 8 * i + 4), cnt, ccnt, j, co;
        if (i && h < last) { vfail(v, "audio script entries not sorted at %u", i); return; }
        last = h;
        if (o + 4 > bco) { vfail(v, "audio script entry %u offset", i); return; }
        cnt = npa_hrd16(d + o); ccnt = npa_hrd16(d + o + 2);
        co = o + 4 + 4 * cnt;
        for (j = 0; j < ccnt; j++)
        {
            if (co + 8 > bco) { vfail(v, "audio script entry %u condition %u", i, j); return; }
            co += 8 + 4 * npa_hrd16(d + co + 6);
        }
        if (co > bco) { vfail(v, "audio script entry %u overruns", i); return; }
    }
    v_bytecode_data(v, d + bco, size - bco);
}

static void v_audio_chunk(V* v, const NpaChunk* c)
{
    NpaChunk k[8192];
    int n, i;
    switch (c->id)
    {
    case 0x80023000u:
        if (kids_array(v, c, k, 8192, &n)) return;
        if (n != 2 || k[0].id != 0x23001 || k[1].id != 0x23003 || k[1].dsize != 20 * npa_hrd32(k[0].data)) vfail(v, "sound map: cue chunk vs count");
        break;
    case 0x80023300u: v_resource_bundle(v, c); break;
    case 0x80023200u:
        if (kids_array(v, c, k, 8192, &n)) return;
        if (n != 2 || k[0].id != 0x23201 || k[1].id != 0x23202 || k[1].dsize != 28 * npa_hrd32(k[0].data)) vfail(v, "bank loader: source infos %u bytes for %u sources", n > 1 ? k[1].dsize : 0, npa_hrd32(k[0].data));
        break;
    case 0x00023703u: v_sp_table(v, c); break;
    case 0x80023500u:
    {
        uint32_t ng, nsrc, nname, g, sum = 0;
        if (kids_array(v, c, k, 8192, &n)) return;
        if (n < 4 || k[0].id != 0x23501) { vfail(v, "bank table layout"); return; }
        ng = npa_hrd32(k[0].data); nsrc = npa_hrd32(k[0].data + 8); nname = npa_hrd32(k[0].data + 0x10);
        if (k[1].id != 0x23502 || k[1].dsize != 0x14 * ng) { vfail(v, "bank groups"); return; }
        i = 2;
        for (g = 0; g < ng; g++)
        {
            uint32_t gc = npa_hrd32(k[1].data + 0x14 * g + 8);
            if (i >= n || k[i].id != 0x23503 || k[i].dsize != 4 * gc) { vfail(v, "bank group %u slot chunk", g); return; }
            sum += gc; i++;
        }
        if (sum != nsrc) { vfail(v, "bank groups hold %u sources, table says %u", sum, nsrc); return; }
        if (i + 1 >= n || k[i].id != 0x23504 || k[i].dsize != 0x18 * nsrc || k[i + 1].id != 0x23505 || k[i + 1].dsize != 8 * nname) { vfail(v, "bank sources/names"); return; }
        i += 2;
        for (g = 0; g < nname; g++, i++)
            if (i >= n || k[i].id != 0x23506 || !printable_name(k[i].data, k[i].dsize, NULL)) { vfail(v, "bank name %u", g); return; }
        if (i != n) vfail(v, "bank table has trailing chunks");
        break;
    }
    case 0x80023100u:
        if (kids_array(v, c, k, 8192, &n)) return;
        if (n != 3 || k[0].id != 0x23101 || k[1].id != 0x23102 || k[2].id != 0x23103) { vfail(v, "slider table layout"); return; }
        { uint32_t gc = npa_hrd32(k[0].data + 8), lc = npa_hrd32(k[0].data + 0x10);
          if (k[1].dsize != 0x20 * (gc + lc) && k[1].dsize != 0x20 * gc) { vfail(v, "slider definitions %u bytes for %u+%u", k[1].dsize, gc, lc); return; } }
        break;
    case 0x80023400u:
        if (kids_array(v, c, k, 8192, &n)) return;
        if (n != 2 || k[0].id != 0x23401 || k[1].id != 0x23402 || k[1].dsize != 0x18 * npa_hrd32(k[0].data)) vfail(v, "calculation table layout");
        break;
    case 0x80023600u:
    {
        uint32_t gcn, g, d;
        if (kids_array(v, c, k, 8192, &n)) return;
        if (n < 2 || k[0].id != 0x23601 || k[1].id != 0x23602) { vfail(v, "rpc layout"); return; }
        gcn = npa_hrd32(k[0].data);
        if (k[1].dsize != 0x1C * gcn) { vfail(v, "rpc groups %u bytes for %u", k[1].dsize, gcn); return; }
        i = 2;
        for (g = 0; g < gcn; g++)
        {
            uint32_t dc = npa_hrd32(k[1].data + 0x1C * g + 4);
            const NpaChunk* defs;
            if (i >= n || k[i].id != 0x23603 || k[i].dsize != 0x24 * dc) { vfail(v, "rpc group %u definitions", g); return; }
            defs = &k[i]; i++;
            for (d = 0; d < dc; d++, i++)
            {
                uint32_t pc = npa_hrd32(defs->data + 0x24 * d + 0x18), q;
                if (i >= n || k[i].id != 0x23604 || k[i].dsize != 8 * pc) { vfail(v, "rpc group %u definition %u points (%u)", g, d, pc); return; }
                for (q = 0; q < 2 * pc; q++)
                    if (!finite_f(hf(k[i].data + 4 * q))) { vfail(v, "rpc curve point %g", hf(k[i].data + 4 * q)); return; }
            }
        }
        if (i != n) vfail(v, "rpc controller has trailing chunks");
        break;
    }
    case 0x00001200u:
        if (c->dsize < 0x14) { vfail(v, "registry"); return; }
        v_reg_container(v, c->data, c->dsize, 0x0C, 0);
        break;
    case 0x00023704u: v_audio_script(v, c->data, c->dsize); break;
    default: vfail(v, "audio chunk 0x%x", c->id);
    }
}

static void v_resbun_top(V* v, const NpaChunk* top)
{
    NpaKids it;
    NpaChunk s;
    int r;
    if (top->id != 0x80000001u) { vfail(v, "resbun root 0x%x", top->id); return; }
    npa_kids_begin(&v->c, &it, top);
    while ((r = npa_kids_next(&it, &s)) > 0)
    {
        v_audio_chunk(v, &s);
        if (v->bad) return;
    }
    if (r < 0) vfail(v, "resbun: %s", v->res.msg);
}

/* ---------------------------------------------------------------- driver */

static void walk_top(V* v, uint32_t only, void (*fn)(V*, const NpaChunk*))
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_range(&v->c, &it, v->c.base, v->c.end);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (only && k.id != only) continue;
        fn(v, &k);
        if (v->bad) return;
    }
    if (r < 0) vfail(v, "top-level walk: %s", v->res.msg);
}

static void v_nis_top(V* v, const NpaChunk* k)
{
    if (k->id == 0x80017000u) v_sanim(v, k);
    else if (k->id == 0x8002500Bu) v_cam(v, k);
    else vfail(v, "nis top-level chunk 0x%x", k->id);
}

int npav_verify(NpaKind kind, const void* data, size_t size, char* msg, size_t cap)
{
    V v;
    memset(&v, 0, sizeof v);
    v.msg = msg;
    v.cap = cap;
    msg[0] = 0;
    v.res.status = 0;
    v.c.res = &v.res;
    v.c.base = (uint8_t*)data;
    v.c.end = v.c.base + size;
    switch (kind)
    {
    case NPA_K_SANIM: walk_top(&v, 0x80017000u, v_sanim); break;
    case NPA_K_SHIER: walk_top(&v, 0x80018000u, v_shier); break;
    case NPA_K_CAM: walk_top(&v, 0x8002500Bu, v_cam); break;
    case NPA_K_NIS: walk_top(&v, 0, v_nis_top); break;
    case NPA_K_CPH: walk_top(&v, 0x8001D000u, v_cph); break;
    case NPA_K_RETARGET: walk_top(&v, 0x80017104u, v_retarget); break;
    case NPA_K_RLG: walk_top(&v, 0, v_rlg_top); break;
    case NPA_K_RLT: v_rlt_data(&v, (const uint8_t*)data, size); break;
    case NPA_K_WORLD_RES: walk_top(&v, 0, v_world_res_top); break;
    case NPA_K_WORLD_TMP: walk_top(&v, 0, v_world_tmp_top); break;
    case NPA_K_FEN: v_fen(&v, (const uint8_t*)data, size); break;
    case NPA_K_BUN: walk_top(&v, 0, v_bun_top); break;
    case NPA_K_RESBUN: walk_top(&v, 0, v_resbun_top); break;
    case NPA_K_BYTECODE: v_bytecode_data(&v, (const uint8_t*)data, size); break;
    case NPA_K_TRG: v_trg(&v, (const uint8_t*)data, size); break;
    case NPA_K_LOC: v_loc(&v, (const uint8_t*)data, size); break;
    case NPA_K_BUNDLEFILE: v_bundlefile(&v, (const uint8_t*)data, size); break;
    default: break;
    }
    if (!v.bad && v.qbad > v.qkeys / 50 + 2)
        vfail(&v, "%lu of %lu quaternions are not unit length, first: %s", v.qbad, v.qkeys, v.first_q);
    return v.bad;
}
