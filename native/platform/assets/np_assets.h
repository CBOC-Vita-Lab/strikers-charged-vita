/*
 * np_assets.h - Mario Strikers Charged native port: disc asset conversion (workstream NPA).
 *
 * The disc stores everything big-endian. The native port is little-endian (Vita, host32, armhf)
 * and ILP32, so every disc struct keeps the layout the decompiled loaders expect and only the byte
 * order of its fields changes. The loaders keep doing their own pointer relocation (offsets turned
 * into 32-bit pointers); this library swaps the bytes FIRST, in place, before the loader looks at
 * the buffer.
 *
 * What is NOT converted (stays big-endian, consumed by the GX layer / vgx):
 *   - vertex arrays (rlg chunk 0x1B006, vertex-animation frames)
 *   - texel data and palettes (rlt payloads, GXTextureHeader excludes the header itself)
 *   - display lists
 * What IS converted: chunk headers, index data, model/packet/stream records, material parameter
 * blocks, skin data, animation keys, hierarchies, cameras, FE package object graph, bundle
 * directories, texture headers... see docs/assets.md for the complete table.
 *
 * Usage (load time, the hook in nlFile.cpp):
 *     NpaKind k = npa_classify(path, &isZlib);        // once, when the load is requested
 *     ... file read (and zlib inflate when isZlib) ...
 *     NpaResult r; npa_convert(k, buffer, size, &r);  // before the loader parses the buffer
 *
 * All conversions are in place, never write outside [data, data+size), and are idempotent
 * (a buffer that is already host-order is detected and left alone, status NPA_ALREADY).
 */
#ifndef NP_ASSETS_H
#define NP_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#include "np_endian.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NpaStatus
{
    NPA_OK = 0,
    NPA_ALREADY = 1,           /* buffer was already host-order (or conversion not needed) */
    NPA_ERR_ARG = -1,
    NPA_ERR_FORMAT = -2,       /* structure is not what the loader expects */
    NPA_ERR_BOUNDS = -3,       /* a size or offset points outside the buffer */
    NPA_ERR_UNSUPPORTED = -4   /* kind known but converter not implemented */
} NpaStatus;

/* Every file kind the game loads. The first entry NPA_K_NONE means "no conversion needed". */
typedef enum NpaKind
{
    NPA_K_NONE = 0,    /* text, ini, csv, audio streams, movies, Nintendo library files */
    NPA_K_RLG,         /* .rlg model container (chunk tree 0x8001B000 / 0x8001B100) */
    NPA_K_RLT,         /* .rlt texture bundle ('PTLG') */
    NPA_K_SANIM,       /* .sanim skeletal animation (0x80017000) */
    NPA_K_SHIER,       /* .shier skeleton hierarchy (0x80018000) */
    NPA_K_CAM,         /* .cam camera animation (0x8002500B) */
    NPA_K_NIS,         /* .nis cut scene (0x80017000 + 0x8002500B chunks) */
    NPA_K_CPH,         /* .cph character physics (0x8001D000) */
    NPA_K_RETARGET,    /* animretarget bin files (0x80017104) */
    NPA_K_WORLD_RES,   /* gameworld.res (root 0x80000001: hierarchies, anims, objects, visibility) */
    NPA_K_WORLD_TMP,   /* gameworld.tmp (root 0x80000001: texture bundle + models) */
    NPA_K_BUN,         /* effects / misc bundles .bun (root 0x80000001) */
    NPA_K_RESBUN,      /* audio resource bundles .resbun (root 0x80000001) */
    NPA_K_FEN,         /* .fen front-end package ('FENL') */
    NPA_K_BUNDLEFILE,  /* FE .res / .dmn BundleFile archives */
    NPA_K_LOC,         /* .loc localisation ('NLOC') */
    NPA_K_TRG,         /* .trg animation triggers ('NLBT') */
    NPA_K_BYTECODE,    /* .byte_code interpreter programs (0xE11C2112) */
    NPA_K_COUNT
} NpaKind;

typedef struct NpaResult
{
    int status;            /* NpaStatus */
    uint32_t chunks;       /* chunk headers converted */
    uint32_t warnings;     /* soft inconsistencies (size differs from what the decoder implies) */
    size_t bytes_swapped;  /* payload bytes whose order changed */
    char msg[192];         /* first error / warning text, empty when clean */
} NpaResult;

/* Classify a disc path ("art/fe/main.fen", "Art/environments/x/gameworld.res.zlib", ...) by its
 * name (case-insensitive). *is_zlib is set when the file is an NLG zlib container (BE u32
 * uncompressed size + zlib stream); the returned kind is then the kind of the inflated data. */
NpaKind npa_classify(const char* path, int* is_zlib);
const char* npa_kind_name(NpaKind kind);

/* Convert a whole file image in place. `data` must stay at the alignment the game loads it with
 * (32 bytes) so aligned chunk payloads land where the loader finds them. */
int npa_convert(NpaKind kind, void* data, size_t size, NpaResult* res);

/* Individual converters (same contract as npa_convert). */
int npa_convert_rlg(void* data, size_t size, NpaResult* res);
int npa_convert_rlt(void* data, size_t size, NpaResult* res);
int npa_convert_sanim(void* data, size_t size, NpaResult* res);
int npa_convert_shier(void* data, size_t size, NpaResult* res);
int npa_convert_cam(void* data, size_t size, NpaResult* res);
int npa_convert_nis(void* data, size_t size, NpaResult* res);
int npa_convert_cph(void* data, size_t size, NpaResult* res);
int npa_convert_retarget(void* data, size_t size, NpaResult* res);
int npa_convert_world_res(void* data, size_t size, NpaResult* res);
int npa_convert_world_tmp(void* data, size_t size, NpaResult* res);
int npa_convert_fen(void* data, size_t size, NpaResult* res);
int npa_convert_bun(void* data, size_t size, NpaResult* res);
int npa_convert_resbun(void* data, size_t size, NpaResult* res);
int npa_convert_loc(void* data, size_t size, NpaResult* res);
int npa_convert_trg(void* data, size_t size, NpaResult* res);
int npa_convert_bytecode(void* data, size_t size, NpaResult* res);
int npa_convert_bundlefile(void* data, size_t size, NpaResult* res);

/* Piecewise API for BundleFile, which the engine reads with nlRead/nlSeek rather than whole:
 * swap the 16-byte header, the directory, and each texture entry after it was read. */
int npa_bundle_fix_header(void* header16); /* swaps if big-endian; returns 1 when it did */
void npa_bundle_swap_header(void* header16);
void npa_bundle_swap_directory(void* dir, uint32_t numFiles);
/* One texture entry (GXTextureHeader + texels, or a texture animation) read from a bundle; returns
 * NPA_OK, NPA_ALREADY (already host order) or an error. */
int npa_texture_entry_swap(void* entry, size_t size);

/* NLG zlib container: first four bytes are the big-endian uncompressed size. */
static inline uint32_t npa_zlib_container_size(const void* file) { return npa_rd32(file); }

#ifdef __cplusplus
}
#endif

#endif /* NP_ASSETS_H */
