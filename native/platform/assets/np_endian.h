/*
 * np_endian.h - byte order helpers for the asset converters (NPA).
 *
 * The disc is big-endian. The native port runs little-endian. Every helper here works on bytes in
 * memory and never dereferences a misaligned wide pointer, so it is safe on any host.
 *
 *   npa_rd*()        read a big-endian value from memory (file bytes not converted yet)
 *   npa_swap*_in()   reverse the bytes of an array in place (BE -> host, or host -> BE)
 *
 * The converters assume a little-endian host. On a big-endian host the whole library is a no-op
 * (npa_host_is_le() is 0 and npa_convert() reports NPA_ALREADY).
 */
#ifndef NP_ENDIAN_H
#define NP_ENDIAN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define NPA_HOST_LE 0
#else
#define NPA_HOST_LE 1
#endif

static inline uint16_t npa_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t npa_bswap32(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#else
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
#endif
}

/* Read a big-endian value from raw bytes. */
static inline uint16_t npa_rd16(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)((b[0] << 8) | b[1]);
}
static inline uint32_t npa_rd32(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

/* Big-endian float read, and big-endian stores: for the data that stays big-endian because the GX layer
 * reads it (vertex arrays, display lists, runtime-built texels) when the CPU touches it. */
static inline float npa_rdf32(const void* p)
{
    uint32_t u = npa_rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline void npa_wr16(void* p, uint16_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}
static inline void npa_wr32(void* p, uint32_t v)
{
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}
static inline void npa_wrf32(void* p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    npa_wr32(p, u);
}

/* Read a host-order value (after conversion). */
static inline uint16_t npa_hrd16(const void* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t npa_hrd32(const void* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Reverse the bytes of one value in place. */
static inline void npa_swap16_one(void* p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    v = npa_bswap16(v);
    memcpy(p, &v, 2);
}
static inline void npa_swap32_one(void* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    v = npa_bswap32(v);
    memcpy(p, &v, 4);
}

/* Reverse an array of n 16-/32-bit values in place. memcpy keeps it alignment-safe; compilers
 * lower it to plain loads and stores. */
static inline void npa_swap16_in(void* p, size_t n)
{
    uint8_t* b = (uint8_t*)p;
    size_t i;
    for (i = 0; i < n; i++, b += 2) npa_swap16_one(b);
}
static inline void npa_swap32_in(void* p, size_t n)
{
    uint8_t* b = (uint8_t*)p;
    size_t i;
    for (i = 0; i < n; i++, b += 4) npa_swap32_one(b);
}

#ifdef __cplusplus
}
#endif

#endif /* NP_ENDIAN_H */
