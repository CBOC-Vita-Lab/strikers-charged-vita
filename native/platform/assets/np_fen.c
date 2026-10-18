/*
 * np_fen.c - .fen front-end packages (FEScene::LoadPackage in src/Game/FE/feScene.cpp).
 *
 * File: FE_FILE_HEADER {char 'FENL'[4], u32 version (1), u32 DataLength, u32 PointerTableLength}
 * (16 bytes), then DataLength bytes of memory image (an FEPackage object graph built by the
 * authoring tool) and PointerTableLength bytes of u32 offsets of every pointer slot in the image.
 * The loader copies the image and the table, adds the image base to each listed slot (0xFFFFFFFF
 * becomes null) - that stays the loader's job. This converter swaps the multi-byte fields of every
 * object of the graph and the pointer table.
 *
 * The graph is discovered the way the engine's types link it (FEPackage -> TLComponent / FEPresentation
 * / FEResourceHandle ring / FELibObject ring -> TLSlide -> TLInstance -> FELibObject ...), the concrete
 * class of a node comes from its type field. The set of pointer slots of the discovered objects must
 * equal the file's pointer table exactly; any difference means the object model is wrong and the file
 * is rejected, so a layout mistake cannot pass silently. Field widths come from np_layout_fe.inc
 * (generated from the decomp headers; byte-typed members such as names, bools and colours keep their
 * bytes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_internal.h"

#include "np_layout_fe.inc"

enum
{
    K_PACKAGE, K_PRESENT, K_SLIDE, K_INST, K_IMGINST, K_TXTINST, K_COMPINST,
    K_LAYER, K_IMAGE, K_TEXT, K_COMPONENT, K_GROUP, K_TEXRES, K_FONTRES, K_SCENERES,
    K_ANIM, K_KF1, K_KF3, K_COUNT
};

static const char* const kKindStruct[K_COUNT] = {
    "FEPackage", "FEPresentation", "TLSlide", "TLInstance", "TLImageInstance", "TLTextInstance", "TLComponentInstance",
    "FELayer", "FEImage", "FEText", "TLComponent", "FEGroup", "FETextureResource", "FEFontResource", "FESceneResource",
    "FEAnimation", "fAnimationKeyframe", "v3AnimationKeyframe"
};

typedef struct { uint32_t off; uint8_t kind; } Obj;

typedef struct
{
    NpaCtx* c;
    const uint8_t* data; /* big-endian image */
    uint32_t len;
    Obj* objs;
    uint32_t n, cap;
    uint8_t* word_kind; /* per 4-byte word: kind+1 of the object that starts/covers it, 0 = free */
    uint32_t* slots;
    uint32_t nslots, slotcap;
    const NpaStruct* st[K_COUNT];
    uint32_t* stack;
    uint32_t sp, stackcap;
    int failed;
} G;

static uint32_t rd(const G* g, uint32_t off) { return npa_rd32(g->data + off); }

static int fail(G* g, const char* fmt, uint32_t a, uint32_t b)
{
    if (!g->failed) npa_err(g->c, NPA_ERR_FORMAT, fmt, a, b);
    g->failed = 1;
    return -1;
}

static int push_slot(G* g, uint32_t slot)
{
    if (g->nslots == g->slotcap)
    {
        uint32_t cap = g->slotcap ? g->slotcap * 2 : 1024;
        uint32_t* p = (uint32_t*)realloc(g->slots, cap * sizeof(uint32_t));
        if (!p) return fail(g, "out of memory%u%u", 0, 0);
        g->slots = p;
        g->slotcap = cap;
    }
    g->slots[g->nslots++] = slot;
    return 0;
}

/* Register an object of `kind` at off (once). Returns 1 if new, 0 if already known, -1 on error. */
static int add_obj(G* g, uint32_t off, int kind)
{
    uint32_t size = g->st[kind]->size, w;
    if (off & 3) return fail(g, "FEN object at 0x%x is not word aligned (kind %u)", off, (uint32_t)kind);
    if ((uint64_t)off + size > g->len) return fail(g, "FEN object at 0x%x (kind %u) runs past the image", off, (uint32_t)kind);
    if (g->word_kind[off / 4])
    {
        if (g->word_kind[off / 4] != kind + 1 || g->objs == NULL)
            return fail(g, "FEN object at 0x%x reached as two kinds (%u vs another)", off, (uint32_t)kind);
        return 0;
    }
    for (w = off / 4; w < (off + size) / 4 + ((off + size) & 3 ? 1 : 0); w++)
        if (g->word_kind[w]) return fail(g, "FEN object at 0x%x (kind %u) overlaps another object", off, (uint32_t)kind);
    for (w = off / 4; w < (off + size) / 4 + ((off + size) & 3 ? 1 : 0); w++) g->word_kind[w] = (uint8_t)(kind + 1);
    if (g->n == g->cap)
    {
        uint32_t cap = g->cap ? g->cap * 2 : 1024;
        Obj* p = (Obj*)realloc(g->objs, cap * sizeof(Obj));
        if (!p) return fail(g, "out of memory%u%u", 0, 0);
        g->objs = p;
        g->cap = cap;
    }
    g->objs[g->n].off = off;
    g->objs[g->n].kind = (uint8_t)kind;
    g->n++;
    if (g->sp == g->stackcap)
    {
        uint32_t cap = g->stackcap ? g->stackcap * 2 : 1024;
        uint32_t* p = (uint32_t*)realloc(g->stack, cap * sizeof(uint32_t));
        if (!p) return fail(g, "out of memory%u%u", 0, 0);
        g->stack = p;
        g->stackcap = cap;
    }
    g->stack[g->sp++] = g->n - 1;
    return 1;
}

static int libobj_kind(G* g, uint32_t target)
{
    uint32_t t;
    if ((uint64_t)target + 0x78 > g->len) return fail(g, "FEN lib object 0x%x outside the image%u", target, 0);
    t = rd(g, target + 0x74);
    switch (t)
    {
    case 0: return K_LAYER;
    case 1: return K_IMAGE;
    case 2: return K_TEXT;
    case 3: return K_COMPONENT;
    case 4: return K_GROUP;
    }
    return fail(g, "FEN lib object 0x%x has type %u", target, t);
}

static int inst_kind(G* g, uint32_t target)
{
    uint32_t t;
    if ((uint64_t)target + 0x90 > g->len) return fail(g, "FEN instance 0x%x outside the image%u", target, 0);
    t = rd(g, target + 0x88);
    switch (t)
    {
    case 1: case 5: return K_INST;
    case 2: return K_IMGINST;
    case 3: return K_TXTINST;
    case 4: return K_COMPINST;
    }
    return fail(g, "FEN instance 0x%x has type %u", target, t);
}

static int res_kind(G* g, uint32_t target)
{
    uint32_t t;
    if ((uint64_t)target + 0x18 > g->len) return fail(g, "FEN resource 0x%x outside the image%u", target, 0);
    t = rd(g, target + 8);
    switch (t)
    {
    case 0: return K_TEXRES;
    case 1: return K_FONTRES;
    case 2: return K_SCENERES;
    }
    return fail(g, "FEN resource 0x%x has type %u", target, t);
}

/* follow the pointer in slot (object at base, field off): target kind decided by `want` */
enum { W_NONE, W_SLIDE, W_INST, W_LIB, W_RES, W_ANIM, W_KF, W_PRESENT, W_COMP, W_TEXRES, W_FONTRES, W_SCENERES };

static int follow(G* g, uint32_t slot, int want, int anim_cast)
{
    uint32_t t = rd(g, slot);
    int kind;
    /* A slot holding 0 is not in the pointer table: the loader leaves it 0 = null (the component list
     * head and the runtime font / scene context slots). Pointers to offset 0 do not occur. */
    if (t == 0) return 0;
    if (push_slot(g, slot)) return -1;
    if (t == 0xFFFFFFFFu) return 0;
    switch (want)
    {
    case W_SLIDE: kind = K_SLIDE; break;
    case W_INST: kind = inst_kind(g, t); break;
    case W_LIB: kind = libobj_kind(g, t); break;
    case W_RES: kind = res_kind(g, t); break;
    case W_ANIM: kind = K_ANIM; break;
    case W_KF: kind = anim_cast == 1 ? K_KF3 : K_KF1; break;
    case W_PRESENT: kind = K_PRESENT; break;
    case W_COMP: kind = K_COMPONENT; break;
    case W_TEXRES: kind = K_TEXRES; break;
    case W_FONTRES: kind = K_FONTRES; break;
    case W_SCENERES: kind = K_SCENERES; break;
    default: return 0;
    }
    if (kind < 0) return -1;
    return add_obj(g, t, kind) < 0 ? -1 : 0;
}

static int visit(G* g, const Obj* o)
{
    uint32_t b = o->off;
    switch (o->kind)
    {
    case K_PACKAGE:
        follow(g, b + 0x00, W_COMP, 0);
        follow(g, b + 0x04, W_PRESENT, 0);
        follow(g, b + 0x08, W_RES, 0);
        follow(g, b + 0x0C, W_LIB, 0);
        break;
    case K_PRESENT:
        follow(g, b + 0x00, W_SLIDE, 0);
        follow(g, b + 0x04, W_SLIDE, 0);
        break;
    case K_SLIDE:
        follow(g, b + 0x00, W_SLIDE, 0);
        follow(g, b + 0x04, W_SLIDE, 0); /* the decomp names it pad0; the pointer table lists it: m_prev */
        follow(g, b + 0x08, W_INST, 0);
        follow(g, b + 0x0C, W_ANIM, 0);
        break;
    case K_INST: case K_IMGINST: case K_TXTINST: case K_COMPINST:
        follow(g, b + 0x00, W_INST, 0);
        follow(g, b + 0x04, W_INST, 0);
        follow(g, b + 0x08, W_INST, 0);
        follow(g, b + 0x0C, W_LIB, 0);
        if (o->kind == K_IMGINST) follow(g, b + 0x90, W_TEXRES, 0);
        break;
    case K_LAYER: case K_GROUP: case K_IMAGE: case K_TEXT: case K_COMPONENT:
        follow(g, b + 0x00, W_LIB, 0);
        follow(g, b + 0x04, W_LIB, 0);
        if (o->kind == K_IMAGE) follow(g, b + 0x78, W_TEXRES, 0);
        if (o->kind == K_TEXT) follow(g, b + 0x78, W_FONTRES, 0);
        if (o->kind == K_COMPONENT)
        {
            follow(g, b + 0x78, W_SLIDE, 0);
            follow(g, b + 0x7C, W_SLIDE, 0);
        }
        break;
    case K_TEXRES: case K_FONTRES: case K_SCENERES:
        follow(g, b + 0x00, W_RES, 0);
        follow(g, b + 0x04, W_RES, 0);
        if (o->kind == K_FONTRES) follow(g, b + 0x18, W_NONE, 0);
        if (o->kind == K_SCENERES) follow(g, b + 0x18, W_NONE, 0);
        break;
    case K_ANIM:
    {
        uint32_t cast = npa_rd16(g->data + b + 0x10);
        follow(g, b + 0x04, W_ANIM, 0);
        follow(g, b + 0x08, W_ANIM, 0);
        follow(g, b + 0x0C, W_INST, 0);
        follow(g, b + 0x18, W_KF, (int)cast);
        if (cast > 1) return fail(g, "FEN animation at 0x%x has cast type %u", b, cast);
        break;
    }
    case K_KF1:
        follow(g, b + 0x10, W_KF, 0);
        follow(g, b + 0x14, W_KF, 0);
        break;
    case K_KF3:
        follow(g, b + 0x30, W_KF, 1);
        follow(g, b + 0x34, W_KF, 1);
        break;
    }
    return g->failed ? -1 : 0;
}

static int cmp_u32(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int npa_convert_fen_impl(NpaCtx* c, uint8_t* p, size_t size)
{
    G g;
    uint32_t dataLen, tabLen, i;
    uint8_t* image;
    uint8_t* table;
    uint32_t ntab;
    int k;
    memset(&g, 0, sizeof g);
    g.c = c;
    if (size < 16)
    {
        npa_err(c, NPA_ERR_FORMAT, "fen smaller than its header");
        return NPA_ERR_FORMAT;
    }
    if (memcmp(p, "FENL", 4) != 0)
    {
        npa_err(c, NPA_ERR_FORMAT, "fen thumbprint is not 'FENL'");
        return NPA_ERR_FORMAT;
    }
    if (npa_hrd32(p + 4) == 1 && npa_rd32(p + 4) != 1) return NPA_ALREADY;
    if (npa_rd32(p + 4) != 1)
    {
        npa_err(c, NPA_ERR_FORMAT, "fen version %u", npa_rd32(p + 4));
        return NPA_ERR_FORMAT;
    }
    dataLen = npa_rd32(p + 8);
    tabLen = npa_rd32(p + 12);
    if ((uint64_t)16 + dataLen + tabLen > size || (dataLen & 3) || (tabLen & 3))
    {
        npa_err(c, NPA_ERR_BOUNDS, "fen sizes: data %u table %u in a %zu byte file", dataLen, tabLen, size);
        return NPA_ERR_BOUNDS;
    }
    NPA_EXPECT(c, (uint64_t)16 + dataLen + tabLen == size, "fen has %zu trailing bytes", size - 16 - dataLen - tabLen);
    image = p + 16;
    table = image + dataLen;
    ntab = tabLen / 4;
    for (k = 0; k < K_COUNT; k++)
    {
        g.st[k] = npa_find_struct(np_structs, (int)(sizeof np_structs / sizeof np_structs[0]), kKindStruct[k]);
        if (!g.st[k])
        {
            npa_err(c, NPA_ERR_FORMAT, "fen layout table lacks %s", kKindStruct[k]);
            return NPA_ERR_FORMAT;
        }
    }
    g.data = image;
    g.len = dataLen;
    g.word_kind = (uint8_t*)calloc(dataLen / 4 + 2, 1);
    if (!g.word_kind) { npa_err(c, NPA_ERR_ARG, "out of memory"); return NPA_ERR_ARG; }

    /* discovery on the untouched big-endian image */
    if (add_obj(&g, 0, K_PACKAGE) < 0) goto done;
    while (g.sp && !g.failed)
    {
        Obj o = g.objs[g.stack[--g.sp]];
        if (visit(&g, &o) < 0) break;
    }
    if (g.failed) goto done;

    /* every slot of the discovered objects must be in the pointer table and vice versa */
    {
        uint32_t* tab = (uint32_t*)malloc((ntab ? ntab : 1) * sizeof(uint32_t));
        uint32_t extra = 0, missing = 0, a = 0, b = 0;
        if (!tab) { npa_err(c, NPA_ERR_ARG, "out of memory"); goto done; }
        for (i = 0; i < ntab; i++) tab[i] = npa_rd32(table + 4 * i);
        qsort(tab, ntab, sizeof(uint32_t), cmp_u32);
        qsort(g.slots, g.nslots, sizeof(uint32_t), cmp_u32);
        while (a < g.nslots || b < ntab)
        {
            if (b >= ntab || (a < g.nslots && g.slots[a] < tab[b])) { extra++; if (getenv("NPA_DEBUG")) { uint32_t w = g.slots[a] / 4; int kk = g.word_kind[w] - 1; while (kk >= 0 && w > 0 && g.word_kind[w - 1] == kk + 1) w--; fprintf(stderr, "  extra slot 0x%x (value 0x%x) kind %d object ~0x%x\n", g.slots[a], rd(&g, g.slots[a]), kk, w * 4); } if (extra == 1) npa_warn(c, "fen slot 0x%x of the graph is not in the pointer table", g.slots[a]); a++; }
            else if (a >= g.nslots || tab[b] < g.slots[a]) { missing++; if (getenv("NPA_DEBUG")) { uint32_t w = tab[b] / 4; int kk = (tab[b] < g.len) ? g.word_kind[w] - 1 : -2; uint32_t base = tab[b]; while (kk >= 0 && w > 0 && g.word_kind[w - 1] == kk + 1) w--; fprintf(stderr, "  table entry 0x%x (value 0x%x) owner kind %d starting around 0x%x\n", tab[b], rd(&g, tab[b]), kk, w * 4); (void)base; } if (missing == 1) npa_warn(c, "fen pointer table entry 0x%x is not a slot of any discovered object", tab[b]); b++; }
            else { a++; b++; }
        }
        free(tab);
        if (extra || missing)
        {
            npa_err(c, NPA_ERR_FORMAT, "fen object model disagrees with the pointer table: %u slots not in table, %u table entries unaccounted (%s)", extra, missing, c->res->msg);
            goto done;
        }
    }

    if (getenv("NPA_DEBUG"))
    {
        uint32_t covered = 0, gaps = 0, gapstart = 0, w;
        for (w = 0; w < dataLen / 4; w++)
        {
            if (g.word_kind[w]) { covered += 4; if (gaps) { fprintf(stderr, "  gap 0x%x..0x%x (%u bytes)\n", gapstart, w * 4, w * 4 - gapstart); gaps = 0; } }
            else if (!gaps) { gaps = 1; gapstart = w * 4; }
        }
        if (gaps) fprintf(stderr, "  gap 0x%x..0x%x (%u bytes)\n", gapstart, dataLen, dataLen - gapstart);
        fprintf(stderr, "  %u objects cover %u of %u bytes\n", g.n, covered, dataLen);
    }
    if (getenv("NPA_DEBUG2"))
    {
        /* which layout-uncovered bytes carry data? */
        for (i = 0; i < g.n; i++)
        {
            const NpaStruct* st = g.st[g.objs[i].kind];
            uint8_t cov[512];
            uint32_t b;
            int f;
            memset(cov, 0, sizeof cov);
            for (f = 0; f < st->nflds; f++)
            {
                uint32_t q;
                for (q = 0; q < (uint32_t)st->flds[f].size * st->flds[f].count; q++) if (st->flds[f].off + q < 512) cov[st->flds[f].off + q] = 1;
            }
            if (g.objs[i].kind == K_SLIDE) { cov[4] = cov[5] = cov[6] = cov[7] = 1; }
            for (b = 0; b < st->size && b < 512; b++)
                if (!cov[b] && image[g.objs[i].off + b]) fprintf(stderr, "NZ %s +0x%x\n", st->name, b);
        }
    }
    /* convert */
    for (i = 0; i < g.n; i++)
    {
        const NpaStruct* st = g.st[g.objs[i].kind];
        if (npa_swap_struct(c, image + g.objs[i].off, dataLen - g.objs[i].off, st)) goto done;
        /* TLSlide +4 is declared pad0[4] but is m_prev (the pointer table lists it): the layout generator
         * treats pad/unidentified byte blobs as words, which covers it */
    }
    if (npa_swap32_range(c, table, ntab, p, p + size)) goto done;
    npa_swap32_one(p + 4);
    npa_swap32_one(p + 8);
    npa_swap32_one(p + 12);
    c->res->chunks = g.n;

done:
    free(g.objs);
    free(g.word_kind);
    free(g.slots);
    free(g.stack);
    return c->res->status;
}

int npa_convert_fen(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    int r;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    r = npa_convert_fen_impl(&c, c.base, size);
    if (r == NPA_ALREADY) { res->status = NPA_ALREADY; return NPA_ALREADY; }
    return npa_finish(&c, r < 0 ? res->status : 0);
}
