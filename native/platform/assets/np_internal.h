/*
 * np_internal.h - shared internals of the NPA converters (not part of the public API).
 */
#ifndef NP_INTERNAL_H
#define NP_INTERNAL_H

#include <stdarg.h>
#include <stdio.h>

#include "np_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NpaCtx
{
    uint8_t* base;
    uint8_t* end;
    NpaResult* res;
} NpaCtx;

/* nlChunk geometry, exactly as include/NL/nlChunk.h computes it (alignment is relative to the
 * ABSOLUTE address, the game loads files at 32-byte alignment). */
typedef struct NpaChunk
{
    uint8_t* hdr;     /* the 8-byte header */
    uint32_t rawid;   /* m_ID */
    uint32_t id;      /* m_ID & 0x80FFFFFF, what nlChunk::GetID returns */
    uint32_t size;    /* m_Size */
    uint8_t* data;    /* GetData() (aligned payload) */
    uint32_t dsize;   /* GetDataSize() */
    uint8_t* next;    /* GetNextChunk() */
    int container;    /* bit 31 of the id */
} NpaChunk;

#define NPA_IS_CONTAINER(id) (((id) & 0x80000000u) != 0)

void npa_err(NpaCtx* c, int status, const char* fmt, ...);
void npa_warn(NpaCtx* c, const char* fmt, ...);

/* Read the chunk header at p (limit = end of the enclosing range). be=1: header still big-endian
 * (the header-swap pass), be=0: header already host-order. Returns 0 on success. */
int npa_chunk_geom(NpaCtx* c, uint8_t* p, uint8_t* limit, int be, NpaChunk* out);

/* Pass 1 for every chunk file: converts all chunk headers in [p,limit) to host order, recursing
 * into containers (bit 31). Returns 0 or an NpaStatus error. */
int npa_chunks_swap_headers(NpaCtx* c, uint8_t* p, uint8_t* limit, int depth);

/* Iterate the children of a container (host-order headers). */
typedef struct NpaKids
{
    NpaCtx* c;
    uint8_t* p;
    uint8_t* limit;
} NpaKids;
void npa_kids_begin(NpaCtx* c, NpaKids* it, const NpaChunk* parent);
void npa_kids_range(NpaCtx* c, NpaKids* it, uint8_t* p, uint8_t* limit);
/* Returns 1 with *out filled, 0 at the end, -1 on a malformed chunk (error recorded). */
int npa_kids_next(NpaKids* it, NpaChunk* out);

/* Payload helpers (count bytes in res->bytes_swapped). All check dsize against the element
 * size; a ragged size is a warning and the whole elements are converted. */
void npa_chunk_swap32(NpaCtx* c, const NpaChunk* k);
void npa_chunk_swap16(NpaCtx* c, const NpaChunk* k);
/* swap n 32-bit / 16-bit values starting at byte offset off inside buffer p (range checked
 * against [lo, hi)); used for struct fields. */
int npa_swap32_range(NpaCtx* c, uint8_t* p, size_t n, const uint8_t* lo, const uint8_t* hi);
int npa_swap16_range(NpaCtx* c, uint8_t* p, size_t n, const uint8_t* lo, const uint8_t* hi);

/* Soft check: records a warning (first message kept) when cond is false. */
#define NPA_EXPECT(c, cond, ...) do { if (!(cond)) npa_warn((c), __VA_ARGS__); } while (0)

/* Wrapper used by every converter: sets up the context, clears *res, runs fn. */
int npa_begin(NpaCtx* c, void* data, size_t size, NpaResult* res);
int npa_finish(NpaCtx* c, int status);

/* Field-level swap tables for in-place C++ structs, generated from the decomp headers by
 * tools/assets/layout/gen_layout.py (clang -m32 record layouts). Only multi-byte scalars are listed. */
typedef struct NpaFld { uint16_t off; uint8_t size; uint16_t count; } NpaFld;
typedef struct NpaStruct { const char* name; uint32_t size; const NpaFld* flds; int nflds; } NpaStruct;

/* Swap every listed field of one struct instance at obj (avail = bytes available from obj). */
int npa_swap_struct(NpaCtx* c, uint8_t* obj, size_t avail, const NpaStruct* st);
/* partial != 0: fields lying past `avail` are skipped with a warning (records stored shorter than sizeof) */
int npa_swap_struct_ex(NpaCtx* c, uint8_t* obj, size_t avail, const NpaStruct* st, int partial);
const NpaStruct* npa_find_struct(const NpaStruct* table, int n, const char* name);

/* Converters for chunk bodies (host-order headers), reused by file kinds that embed them. */
int npa_cvt_sanim_chunk(NpaCtx* c, const NpaChunk* k);
int npa_cvt_shier_chunk(NpaCtx* c, const NpaChunk* k);
int npa_cvt_cam_chunk(NpaCtx* c, const NpaChunk* k);
int npa_cvt_rlg_chunk(NpaCtx* c, const NpaChunk* k);       /* 0x8001B000 */
int npa_cvt_rlg_multi_chunk(NpaCtx* c, const NpaChunk* k); /* 0x8001B100 */
int npa_cvt_rlt_data(NpaCtx* c, uint8_t* p, size_t size);  /* 'PTLG' texture bundle bytes */
int npa_convert_fen_impl(NpaCtx* c, uint8_t* p, size_t size); /* np_fen.c */
int npa_cvt_texture_entry(NpaCtx* c, uint8_t* e, uint32_t size, uint32_t hash); /* one texture / texture anim entry */
int npa_cvt_bytecode(NpaCtx* c, uint8_t* p, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NP_INTERNAL_H */
