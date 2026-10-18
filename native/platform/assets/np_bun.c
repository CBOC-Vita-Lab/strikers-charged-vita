/*
 * np_bun.c - .bun bundles (root 0x80000001): effects, texture bundles, model sets, audio tables.
 *
 * Effects (src/Game/Effects/EffectsBundleData.cpp, EffectsTemplate.cpp, EffectsGroup.cpp), one
 * 0x80024000 chunk per effect:
 *   0x24001 EffectsBundleData (0x18)   0x24025 EffectsTemplate*[] slots   0x24026 EffectsGroup*[] slots
 *   per template 0x80024002: 0x24003 EffectsTemplate (0xE0), 8 x 0x80024004 { 0x24005 fxAnimatedRange
 *                (0x14), optional 0x24006 fxCurveKey[] (0x14 each) }
 *   per group 0x80024020: 0x24021 EffectsGroup (0x1C), 0x24022 EffectsSpec[] (0x58), 0x24023
 *                UserEffectSource[] (8), then one text chunk per user effect (bytes)
 * Field widths from np_layout_fx.inc (generated from the decomp headers; nlColour entries, the
 * emitter/blend/billboard/flag bytes stay).
 * Other children: 0x24100 texture bundle (np_rlt.c), 0x8001B000 / 0x8001B100 models (np_rlg.c); the
 * audio tables (0x1200, 0x2310x ...) are in np_audio.c.
 */
#include <string.h>

#include "np_internal.h"

#include "np_layout_fx.inc"

enum { S_BUNDLE, S_TEMPLATE, S_RANGE, S_KEY, S_GROUP, S_SPEC, S_USERSRC, S_COUNT };
static const char* const kStructNames[S_COUNT] = { "EffectsBundleData", "EffectsTemplate", "fxAnimatedRange", "fxCurveKey",
                                                    "EffectsGroup", "EffectsSpec", "UserEffectSource" };

static const NpaStruct* fx_struct(int s)
{
    return npa_find_struct(np_structs, (int)(sizeof np_structs / sizeof np_structs[0]), kStructNames[s]);
}

static int cvt_records(NpaCtx* c, const NpaChunk* k, int s)
{
    const NpaStruct* st = fx_struct(s);
    uint32_t i, n;
    if (!st) { npa_err(c, NPA_ERR_FORMAT, "fx layout %s missing", kStructNames[s]); return NPA_ERR_FORMAT; }
    if (s == S_TEMPLATE)
    {
        /* the file stores 0xDC bytes: the 26th nlColour of EffectsTemplate overlaps the next chunk header
         * and is never part of the payload; only the 25 stored colours (bytes) are missing */
        if (k->dsize != 0xDC && k->dsize != st->size)
        {
            npa_err(c, NPA_ERR_FORMAT, "effects template chunk is %u bytes", k->dsize);
            return NPA_ERR_FORMAT;
        }
        return npa_swap_struct_ex(c, k->data, k->dsize, st, 1);
    }
    n = k->dsize / st->size;
    NPA_EXPECT(c, k->dsize % st->size == 0, "chunk 0x%x: %u bytes is not a multiple of %s (%u)", k->id, k->dsize, st->name, st->size);
    for (i = 0; i < n; i++)
        if (npa_swap_struct(c, k->data + (size_t)i * st->size, k->dsize - (size_t)i * st->size, st)) return c->res->status;
    return 0;
}

static int cvt_effects_chunk(NpaCtx* c, const NpaChunk* root)
{
    NpaKids it, sub, sub2;
    NpaChunk k, s, t;
    int r, sr, tr;
    npa_kids_begin(c, &it, root);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        switch (k.id)
        {
        case 0x24001:
            if (cvt_records(c, &k, S_BUNDLE)) return c->res->status;
            break;
        case 0x24025: /* template pointer slots */
        case 0x24026: /* group pointer slots */
            npa_chunk_swap32(c, &k);
            break;
        case 0x80024002: /* template */
            npa_kids_begin(c, &sub, &k);
            while ((sr = npa_kids_next(&sub, &s)) > 0)
            {
                if (s.id == 0x24003) { if (cvt_records(c, &s, S_TEMPLATE)) return c->res->status; }
                else if (s.id == 0x80024004)
                {
                    npa_kids_begin(c, &sub2, &s);
                    while ((tr = npa_kids_next(&sub2, &t)) > 0)
                    {
                        if (t.id == 0x24005) { if (cvt_records(c, &t, S_RANGE)) return c->res->status; }
                        else if (t.id == 0x24006) { if (cvt_records(c, &t, S_KEY)) return c->res->status; }
                        else { npa_err(c, NPA_ERR_FORMAT, "fx range: unexpected chunk 0x%x", t.id); return NPA_ERR_FORMAT; }
                    }
                    if (tr < 0) return c->res->status;
                }
                else { npa_err(c, NPA_ERR_FORMAT, "fx template: unexpected chunk 0x%x", s.id); return NPA_ERR_FORMAT; }
            }
            if (sr < 0) return c->res->status;
            break;
        case 0x80024020: /* group */
            npa_kids_begin(c, &sub, &k);
            while ((sr = npa_kids_next(&sub, &s)) > 0)
            {
                if (s.id == 0x24021) { if (cvt_records(c, &s, S_GROUP)) return c->res->status; }
                else if (s.id == 0x24022) { if (cvt_records(c, &s, S_SPEC)) return c->res->status; }
                else if (s.id == 0x24023) { if (cvt_records(c, &s, S_USERSRC)) return c->res->status; }
                else { /* user effect source text: bytes */ }
            }
            if (sr < 0) return c->res->status;
            break;
        default:
            npa_err(c, NPA_ERR_FORMAT, "fx bundle: unexpected chunk 0x%x", k.id);
            return NPA_ERR_FORMAT;
        }
        if (c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

int npa_cvt_audio_bundle_chunk(NpaCtx* c, const NpaChunk* k); /* np_audio.c, returns 1 when the id is not an audio chunk */

int npa_convert_bun(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    NpaKids it, ik;
    NpaChunk k, s;
    int r, ir;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    if ((npa_hrd32(c.base) & 0x80FFFFFFu) == 0x80000001u) { res->status = NPA_ALREADY; return res->status; }
    if ((npa_rd32(c.base) & 0x80FFFFFFu) != 0x80000001u)
    {
        npa_err(&c, NPA_ERR_FORMAT, "bun: unexpected root chunk id 0x%08x", npa_rd32(c.base));
        return res->status;
    }
    if (npa_chunks_swap_headers(&c, c.base, c.end, 0)) return npa_finish(&c, res->status);
    npa_kids_range(&c, &it, c.base, c.end);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id != 0x80000001u)
        {
            npa_err(&c, NPA_ERR_FORMAT, "bun: unexpected top-level chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
            break;
        }
        npa_kids_begin(&c, &ik, &k);
        while ((ir = npa_kids_next(&ik, &s)) > 0)
        {
            switch (s.id)
            {
            case 0x80024000u: r = cvt_effects_chunk(&c, &s); break;
            case 0x00024100u:
                r = npa_cvt_rlt_data(&c, s.data, s.dsize);
                if (r == NPA_ALREADY) r = 0;
                break;
            case 0x8001B000u: r = npa_cvt_rlg_chunk(&c, &s); break;
            case 0x8001B100u: r = npa_cvt_rlg_multi_chunk(&c, &s); break;
            default:
                r = npa_cvt_audio_bundle_chunk(&c, &s);
                if (r > 0)
                {
                    npa_err(&c, NPA_ERR_FORMAT, "bun: unexpected chunk 0x%x", s.id);
                    r = NPA_ERR_FORMAT;
                }
            }
            if (r < 0 || c.res->status < 0) break;
        }
        if (ir < 0) r = c.res->status;
        if (r < 0 || c.res->status < 0) break;
    }
    return npa_finish(&c, r < 0 ? c.res->status : 0);
}
