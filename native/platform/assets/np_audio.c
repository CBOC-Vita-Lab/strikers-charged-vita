/*
 * np_audio.c - audio resource tables: .resbun files (one per sound bank) and the global audio bundle
 * audio/nlxgs.bun. The wave data itself (.nlxwb) is not converted: it is consumed by the DSP /
 * software mixer (NPS), together with the SP sound table below.
 *
 * .resbun (root 0x80000001), parsed by OnAudioResourceBundleLoaded / ParseAudioResourceBundle:
 *   0x80023000 SoundMap:  0x23001 SoundMap (3 words), 0x23003 SoundCue[] (5 words)
 *   0x80023300 resource bundle: 0x23301 AudioResourceBundle, 0x23302 AudioCueDefinition[], 0x23303
 *       AudioVoiceDefinition[], 0x23304 AudioSequenceDefinition[], 0x23305 SoundEventDefinition[],
 *       0x23306 HitMarkerEventDefinition[], 0x23307 ParameterChangeEventDefinition[], then per
 *       cue 0x23308 AudioCueEntry[], per voice 0x23309 u32 sequence slots + 0x2330C u32 rpc indices,
 *       per sequence 0x2330A AudioSequenceEventDefinition[], per sound event 0x2330B SoundChoice[]
 *   0x23703  SPSoundTable (Wii SDK "sp" library): u32 entries, SPSoundEntry[] (7 words), then
 *            SPADPCM[] (46 bytes = 23 u16 each) for the type 0/1 entries. LEFT BIG-ENDIAN: the SDK
 *            layer's SPInitSoundTable patch swaps it (NpSpFixEndian); swapping it here as well would
 *            swap it back (the SP library crashed in SPInitSoundTable that way).
 *   0x80023200 bank loader: 0x23201 AudioSourceData {u32, u32, u8}, 0x23202 AudioSourceInfo[] (7 words)
 * nlxgs.bun: 0x80023500 bank table (0x23501 AudioBankTable, 0x23502 AudioBankGroup[], 0x23503 slots,
 *   0x23504 AudioResourceSource[], 0x23505 AudioResourceName[], 0x23506 names), 0x80023100 sliders
 *   (0x23101, 0x23102 AudioSliderDefinition[], 0x23103 u32 indices), 0x80023400 calculations (0x23401,
 *   0x23402), 0x80023600 rpc (0x23601 controller, 0x23602 groups, 0x23603 definitions, 0x23604 curve
 *   points), 0x1200 packed registry, 0x23704 audio script.
 * Field widths come from np_layout_audio.inc (generated from the decomp headers): byte members such as
 * AudioResourceSource::useCompactCallback keep their bytes.
 */
#include <stdio.h>
#include <string.h>

#include "np_internal.h"

#include "np_layout_audio.inc"

enum
{
    A_SOUNDMAP, A_SOUNDCUE, A_BUNDLE, A_CUE, A_CUEENTRY, A_VOICE, A_SEQUENCE, A_SEQEVENT, A_SOUNDEVENT, A_HITMARKER,
    A_PARAMEVENT, A_SOUNDCHOICE, A_BANKTABLE, A_BANKGROUP, A_RESSOURCE, A_RESNAME, A_SLIDERTABLE, A_SLIDERDEF,
    A_CALCTABLE, A_CALCDEF, A_RPCCTRL, A_RPCGROUP, A_RPCDEF, A_RPCPOINT, A_COUNT
};
static const char* const kAudioStructs[A_COUNT] = {
    "SoundMap", "SoundCue", "AudioResourceBundle", "AudioCueDefinition", "AudioCueEntry", "AudioVoiceDefinition",
    "AudioSequenceDefinition", "AudioSequenceEventDefinition", "SoundEventDefinition", "HitMarkerEventDefinition",
    "ParameterChangeEventDefinition", "SoundChoice", "AudioBankTable", "AudioBankGroup", "AudioResourceSource",
    "AudioResourceName", "AudioSliderTable", "AudioSliderDefinition", "AudioCalculationTable", "AudioCalculationDefinition",
    "AudioRpcController", "AudioRpcGroup", "AudioRpcDefinition", "AudioRpcCurvePoint"
};

static const NpaStruct* astruct(int a)
{
    return npa_find_struct(np_structs, (int)(sizeof np_structs / sizeof np_structs[0]), kAudioStructs[a]);
}

/* an array of records of struct a; allow_pad: chunk may be larger than records (alignment padding) */
static int cvt_array(NpaCtx* c, const NpaChunk* k, int a, int exact)
{
    const NpaStruct* st = astruct(a);
    uint32_t i, n;
    if (!st) { npa_err(c, NPA_ERR_FORMAT, "audio layout %s missing", kAudioStructs[a]); return NPA_ERR_FORMAT; }
    n = k->dsize / st->size;
    if (exact && k->dsize % st->size)
        npa_warn(c, "audio chunk 0x%x: %u bytes is not a multiple of %s (%u)", k->id, k->dsize, st->name, st->size);
    for (i = 0; i < n; i++)
        if (npa_swap_struct(c, k->data + (size_t)i * st->size, k->dsize - (size_t)i * st->size, st)) return c->res->status;
    return 0;
}

static int cvt_one(NpaCtx* c, const NpaChunk* k, int a)
{
    const NpaStruct* st = astruct(a);
    if (!st) { npa_err(c, NPA_ERR_FORMAT, "audio layout %s missing", kAudioStructs[a]); return NPA_ERR_FORMAT; }
    if (k->dsize < st->size)
    {
        npa_err(c, NPA_ERR_FORMAT, "audio chunk 0x%x: %u bytes cannot hold a %s (%u)", k->id, k->dsize, st->name, st->size);
        return NPA_ERR_FORMAT;
    }
    return npa_swap_struct(c, k->data, k->dsize, st);
}

/* ---- SP sound table
 * NOT converted. The table is read by the Wii SDK "sp" library (SPInitSoundTable), which the SDK layer
 * (platform/sdk, NPS) ports with a patch that swaps the entry words and the ADPCM records in place
 * (NpSpFixEndian, once per bank load, not idempotent). The payload therefore stays big-endian here;
 * only the chunk header is swapped by the generic pass. The structure is still validated (big-endian
 * reads) so a broken file is reported. */
static int cvt_sp_table(NpaCtx* c, const NpaChunk* k)
{
    uint32_t n, i, nad = 0;
    if (k->dsize < 4)
    {
        npa_err(c, NPA_ERR_FORMAT, "SP sound table too small");
        return NPA_ERR_FORMAT;
    }
    n = npa_rd32(k->data);
    if ((uint64_t)4 + 28ull * n > k->dsize)
    {
        npa_err(c, NPA_ERR_BOUNDS, "SP sound table: %u entries do not fit %u bytes", n, k->dsize);
        return NPA_ERR_BOUNDS;
    }
    for (i = 0; i < n; i++)
    {
        uint32_t t = npa_rd32(k->data + 4 + 28 * i);
        if (t == 0 || t == 1) nad++;
    }
    if (k->dsize != 4 + 28 * n + 46 * nad)
        npa_warn(c, "SP sound table: %u bytes, %u entries and %u ADPCM sets imply %u", k->dsize, n, nad, 4 + 28 * n + 46 * nad);
    return 0;
}

/* children of a 0x80023xxx container by id */
static int audio_kids(NpaCtx* c, const NpaChunk* container, int (*fn)(NpaCtx*, const NpaChunk*))
{
    NpaKids it;
    NpaChunk k;
    int r;
    npa_kids_begin(c, &it, container);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (fn(c, &k) < 0) return c->res->status;
        if (c->res->status < 0) return c->res->status;
    }
    return r < 0 ? c->res->status : 0;
}

static int fn_soundmap(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x23001: return cvt_one(c, k, A_SOUNDMAP);
    case 0x23003: return cvt_array(c, k, A_SOUNDCUE, 1);
    }
    npa_err(c, NPA_ERR_FORMAT, "sound map: unexpected chunk 0x%x", k->id);
    return NPA_ERR_FORMAT;
}

static int fn_resource(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x23301: return cvt_one(c, k, A_BUNDLE);
    case 0x23302: return cvt_array(c, k, A_CUE, 1);
    case 0x23303: return cvt_array(c, k, A_VOICE, 1);
    case 0x23304: return cvt_array(c, k, A_SEQUENCE, 1);
    case 0x23305: return cvt_array(c, k, A_SOUNDEVENT, 1);
    case 0x23306: return cvt_array(c, k, A_HITMARKER, 1);
    case 0x23307: return cvt_array(c, k, A_PARAMEVENT, 1);
    case 0x23308: return cvt_array(c, k, A_CUEENTRY, 1);
    case 0x23309: npa_chunk_swap32(c, k); return 0; /* AudioSequenceDefinition* slots */
    case 0x2330C: npa_chunk_swap32(c, k); return 0; /* rpc group indices */
    case 0x2330A: return cvt_array(c, k, A_SEQEVENT, 1);
    case 0x2330B: return cvt_array(c, k, A_SOUNDCHOICE, 1);
    }
    npa_err(c, NPA_ERR_FORMAT, "audio resource bundle: unexpected chunk 0x%x", k->id);
    return NPA_ERR_FORMAT;
}

static int fn_bankloader(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x23201: /* AudioSourceData {u32 count, u32 streamBlockSize, u8 isStream} */
        if (k->dsize < 9) { npa_err(c, NPA_ERR_FORMAT, "audio source data %u bytes", k->dsize); return NPA_ERR_FORMAT; }
        return npa_swap32_range(c, k->data, 2, k->data, k->data + k->dsize);
    case 0x23202: /* AudioSourceInfo[]: 7 words */
        NPA_EXPECT(c, k->dsize % 28 == 0, "audio source infos %u bytes", k->dsize);
        return npa_swap32_range(c, k->data, (k->dsize / 28) * 7, k->data, k->data + k->dsize);
    }
    npa_err(c, NPA_ERR_FORMAT, "audio bank loader: unexpected chunk 0x%x", k->id);
    return NPA_ERR_FORMAT;
}

static int fn_banktable(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x23501: return cvt_one(c, k, A_BANKTABLE);
    case 0x23502: return cvt_array(c, k, A_BANKGROUP, 1);
    case 0x23503: npa_chunk_swap32(c, k); return 0; /* AudioResourceSource* slots per group */
    case 0x23504: return cvt_array(c, k, A_RESSOURCE, 1);
    case 0x23505: return cvt_array(c, k, A_RESNAME, 1);
    case 0x23506: return 0; /* bank names: chars */
    }
    npa_err(c, NPA_ERR_FORMAT, "audio bank table: unexpected chunk 0x%x", k->id);
    return NPA_ERR_FORMAT;
}

static int fn_sliders(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x23101: return cvt_one(c, k, A_SLIDERTABLE);
    case 0x23102: return cvt_array(c, k, A_SLIDERDEF, 1);
    case 0x23103: npa_chunk_swap32(c, k); return 0;
    }
    npa_err(c, NPA_ERR_FORMAT, "audio slider table: unexpected chunk 0x%x", k->id);
    return NPA_ERR_FORMAT;
}

static int fn_calc(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x23401: return cvt_one(c, k, A_CALCTABLE);
    case 0x23402: return cvt_array(c, k, A_CALCDEF, 1);
    }
    npa_err(c, NPA_ERR_FORMAT, "audio calculation table: unexpected chunk 0x%x", k->id);
    return NPA_ERR_FORMAT;
}

static int fn_rpc(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x23601: return cvt_one(c, k, A_RPCCTRL);
    case 0x23602: return cvt_array(c, k, A_RPCGROUP, 1);
    case 0x23603: return cvt_array(c, k, A_RPCDEF, 1);
    case 0x23604: return cvt_array(c, k, A_RPCPOINT, 1);
    }
    npa_err(c, NPA_ERR_FORMAT, "audio rpc controller: unexpected chunk 0x%x", k->id);
    return NPA_ERR_FORMAT;
}

int npa_cvt_registry_image(NpaCtx* c, const NpaChunk* k); /* np_registry.c */
int npa_cvt_audio_script(NpaCtx* c, const NpaChunk* k); /* np_audio_data.c */

/* returns 0 handled, 1 not an audio chunk, <0 error */
int npa_cvt_audio_bundle_chunk(NpaCtx* c, const NpaChunk* k)
{
    switch (k->id)
    {
    case 0x80023000u: return audio_kids(c, k, fn_soundmap);
    case 0x80023300u: return audio_kids(c, k, fn_resource);
    case 0x80023200u: return audio_kids(c, k, fn_bankloader);
    case 0x80023500u: return audio_kids(c, k, fn_banktable);
    case 0x80023100u: return audio_kids(c, k, fn_sliders);
    case 0x80023400u: return audio_kids(c, k, fn_calc);
    case 0x80023600u: return audio_kids(c, k, fn_rpc);
    case 0x00023703u: return cvt_sp_table(c, k);
    case 0x00001200u: return npa_cvt_registry_image(c, k);
    case 0x00023704u: return npa_cvt_audio_script(c, k);
    }
    return 1;
}

int npa_convert_resbun(void* data, size_t size, NpaResult* res)
{
    NpaCtx c;
    NpaKids it, ik;
    NpaChunk k, s;
    int r, ir;
    if (npa_begin(&c, data, size, res) != NPA_OK) return res->status;
    if ((npa_hrd32(c.base) & 0x80FFFFFFu) == 0x80000001u) { res->status = NPA_ALREADY; return res->status; }
    if ((npa_rd32(c.base) & 0x80FFFFFFu) != 0x80000001u)
    {
        npa_err(&c, NPA_ERR_FORMAT, "resbun: unexpected root chunk id 0x%08x", npa_rd32(c.base));
        return res->status;
    }
    if (npa_chunks_swap_headers(&c, c.base, c.end, 0)) return npa_finish(&c, res->status);
    npa_kids_range(&c, &it, c.base, c.end);
    while ((r = npa_kids_next(&it, &k)) > 0)
    {
        if (k.id != 0x80000001u)
        {
            npa_err(&c, NPA_ERR_FORMAT, "resbun: unexpected top-level chunk 0x%x", k.id);
            r = NPA_ERR_FORMAT;
            break;
        }
        npa_kids_begin(&c, &ik, &k);
        while ((ir = npa_kids_next(&ik, &s)) > 0)
        {
            r = npa_cvt_audio_bundle_chunk(&c, &s);
            if (r > 0)
            {
                npa_err(&c, NPA_ERR_FORMAT, "resbun: unexpected chunk 0x%x", s.id);
                r = NPA_ERR_FORMAT;
            }
            if (r < 0 || c.res->status < 0) break;
        }
        if (ir < 0) r = c.res->status;
        if (r < 0 || c.res->status < 0) break;
    }
    return npa_finish(&c, r < 0 ? c.res->status : 0);
}
