// Endianness of the two SDK container formats whose headers the game's code (or the SDK code
// below it) reads as structs: the U8 archive of the ARC module and the TPL texture palette. Both
// files are big-endian on the disc. The structure words are swapped in place on first use
// (ARCInitHandle / TPLBind call these through patches/*.sed of the unchanged ref sources); the
// payload (texels, palettes, the files inside an archive) is not touched: the GX layer reads
// texel data big-endian, and the file formats inside an archive are converted by their own
// loaders (see the notes of the assets workstream).
//
// The swap is idempotent: it triggers only when the magic or version reads byte-reversed, and it
// stores the native value back.
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define NP_FMT_LE 1
#else
#define NP_FMT_LE 0
#endif

static uint32_t bs32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}
static uint16_t bs16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void swap32_at(void* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    v = bs32(v);
    memcpy(p, &v, 4);
}
static void swap16_at(void* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    v = bs16(v);
    memcpy(p, &v, 2);
}
static uint32_t get32(const void* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

#define ARC_MAGIC 0x55AA382Du
#define TPL_VERSION 2142000u

// U8 archive: header {magic, fstStart, fstSize, fileStart}, then 12-byte FST entries
// {isDir<<24 | nameOffset, parent or file position, next entry or length}; entry 0 holds the count.
void NpArcFixEndian(void* arc) {
#if NP_FMT_LE
    uint8_t* base = (uint8_t*)arc;
    if (get32(base) != bs32(ARC_MAGIC)) return;
    swap32_at(base + 0);
    swap32_at(base + 4);
    swap32_at(base + 8);
    swap32_at(base + 12);
    uint8_t* fst = base + get32(base + 4);
    uint32_t fstSize = get32(base + 8);
    swap32_at(fst + 8);   // entry 0: the entry count
    uint32_t count = get32(fst + 8);
    if (count * 12u > fstSize) count = fstSize / 12u;   // malformed: stay inside the table
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* e = fst + 12u * i;
        if (i != 0) swap32_at(e + 8);
        swap32_at(e + 0);
        swap32_at(e + 4);
    }
#else
    (void)arc;
#endif
}

// TPL: {version, numDescriptors, descriptorArray offset}; descriptors {texture header offset, CLUT
// header offset}; headers as in revolution/tpl/TPLTypes.h.
void NpTplFixEndian(void* pal) {
#if NP_FMT_LE
    uint8_t* base = (uint8_t*)pal;
    if (get32(base) != bs32(TPL_VERSION)) return;
    swap32_at(base + 0);
    swap32_at(base + 4);
    swap32_at(base + 8);
    uint32_t n = get32(base + 4);
    uint8_t* desc = base + get32(base + 8);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* d = desc + 8u * i;
        swap32_at(d + 0);
        swap32_at(d + 4);
        uint32_t th = get32(d + 0), ch = get32(d + 4);
        if (th) {
            uint8_t* h = base + th;
            swap16_at(h + 0);    // height
            swap16_at(h + 2);    // width
            swap32_at(h + 4);    // format
            swap32_at(h + 8);    // data offset
            swap32_at(h + 12);   // wrapS
            swap32_at(h + 16);   // wrapT
            swap32_at(h + 20);   // minFilter
            swap32_at(h + 24);   // magFilter
            swap32_at(h + 28);   // LODBias (float)
            // bytes 0x20..0x23: edgeLODEnable, minLOD, maxLOD, unpacked
        }
        if (ch) {
            uint8_t* h = base + ch;
            swap16_at(h + 0);   // numEntries
            swap32_at(h + 4);   // format
            swap32_at(h + 8);   // data offset
        }
    }
#else
    (void)pal;
#endif
}

// SP sound table (the audio banks): {u32 entries; SPSoundEntry sound[entries]} followed by one SPADPCM
// record (23 halfwords: coefficients, gain, predictor/scale, history, loop context) for every entry of
// type 0 or 1. Entry words type..currentAddr are swapped; the pointer slot is filled in by SPInitSoundTable.
void NpSpFixEndian(void* table) {
#if NP_FMT_LE
    uint8_t* base = (uint8_t*)table;
    // Idempotent: the assets workstream's loader (np_audio.c cvt_sp_table) converts the same table to host order when
    // the chunk is loaded, so a table that already reads as a sane entry count is left alone; only a byte-reversed
    // count (the raw big-endian table) is converted.
    uint32_t raw = get32(base);
    if (raw <= 100000u) return;
    uint32_t entries = bs32(raw);
    if (entries > 100000u) return;   // neither: not a table
    swap32_at(base);
    uint8_t* e = base + 4;
    uint8_t* adpcm = base + 4 + 28u * entries;
    for (uint32_t i = 0; i < entries; ++i, e += 28) {
        for (int w = 0; w < 6; ++w) swap32_at(e + 4 * w);
        uint32_t type = get32(e);
        if (type == 0 || type == 1) {
            for (int h = 0; h < 23; ++h) swap16_at(adpcm + 2 * h);
            adpcm += 46;
        }
    }
#else
    (void)table;
#endif
}
