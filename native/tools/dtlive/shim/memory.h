// dtlive shim (host32): the recompiler's translation of the RETAIL functions runs on the NATIVE game's own objects.
// Guest address == host address for everything outside the retail static range (the native MEM1/MEM2 arenas sit at
// 0x80700000.. / 0x90000000.. when NP_MEM1_BASE is set); the retail static range 0x80004000..0x806E8000 is mapped by
// dtl.cpp: mutable statics with a native counterpart -> the native object, everything else -> a shadow copy of the DOL.
// No byte swapping (the native objects are little endian); vtable slot loads are shifted by the 8-byte MWCC header;
// 32-bit stores of retail static addresses into native memory are converted to the native addresses (function pointers, data).
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#define MKW_MEMORY_FORCE_INLINE inline __attribute__((always_inline))
namespace dtl {
constexpr uint32_t kStatLo = 0x80004000u, kStatLen = 0x006E4000u;
uint8_t* xlate_static(uint32_t a, uint32_t n, bool write);
uint32_t conv_store(uint32_t v, const uint8_t* dst);
bool in_vtable(const uint8_t* p);
const uint8_t* vt_resolve(const uint8_t* p);       // inside a native vtable: the native slot for the retail slot (see dtl.cpp)
extern uintptr_t g_vt_lo, g_vt_hi, g_shadow_base, g_gstack_base, g_gstack_size;
// bit-field view (MWCC MSB-first retail layout over the clang LSB-first native objects), active in the retail child only; see dtl.cpp / gen_bf.py
extern bool* g_bf_onp;
uint32_t bf_view(uintptr_t p, uint32_t n, uint32_t v);       // native memory word -> value as retail code sees it
uint32_t bf_unview(uintptr_t p, uint32_t n, uint32_t v);     // value stored by retail code -> native memory word
void bf_note_store(uintptr_t p, uint32_t v);                 // a vtable pointer of a tabulated class was stored at p: a new object
MKW_MEMORY_FORCE_INLINE bool is_gstack(const uint8_t* p) { return (uintptr_t)p - g_gstack_base < g_gstack_size; }   // retail scratch: doubles there are big-endian word pairs (stw/stw/lfd conversion trick)

MKW_MEMORY_FORCE_INLINE bool is_shadow(const uint8_t* p) { return (uintptr_t)p - g_shadow_base < kStatLen; }   // the DOL image is big endian
MKW_MEMORY_FORCE_INLINE uint8_t* P(uint32_t a, uint32_t n, bool w) {
    if (a - kStatLo < kStatLen) return xlate_static(a, n, w);
    return (uint8_t*)(uintptr_t)a;
}
MKW_MEMORY_FORCE_INLINE const uint8_t* PR(uint32_t a, uint32_t n) {
    const uint8_t* p = P(a, n, false);
    if ((uintptr_t)p - g_vt_lo < g_vt_hi - g_vt_lo && in_vtable(p)) p = vt_resolve(p);
    return p;
}
}
namespace MemoryInline {
MKW_MEMORY_FORCE_INLINE uint8_t FlatRead8(uint32_t a) { const uint8_t* p = dtl::PR(a, 1); uint8_t v = *p; if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint8_t)dtl::bf_view((uintptr_t)p, 1, v); return v; }
MKW_MEMORY_FORCE_INLINE uint16_t FlatRead16(uint32_t a) { const uint8_t* p = dtl::PR(a, 2); uint16_t v; memcpy(&v, p, 2); if (dtl::is_shadow(p)) return __builtin_bswap16(v); if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint16_t)dtl::bf_view((uintptr_t)p, 2, v); return v; }
MKW_MEMORY_FORCE_INLINE uint32_t FlatRead32(uint32_t a) { const uint8_t* p = dtl::PR(a, 4); uint32_t v; memcpy(&v, p, 4); if (dtl::is_shadow(p)) return __builtin_bswap32(v); if (__builtin_expect((*dtl::g_bf_onp), 0)) v = dtl::bf_view((uintptr_t)p, 4, v); return v; }
MKW_MEMORY_FORCE_INLINE float FlatReadFloat32(uint32_t a) { uint32_t v = FlatRead32(a); float f; memcpy(&f, &v, 4); return f; }
MKW_MEMORY_FORCE_INLINE double FlatReadFloat64(uint32_t a) { const uint8_t* p = dtl::PR(a, 8); uint64_t v; memcpy(&v, p, 8); if (dtl::is_shadow(p)) v = __builtin_bswap64(v); else if (dtl::is_gstack(p)) v = (v << 32) | (v >> 32); double d; memcpy(&d, &v, 8); return d; }
MKW_MEMORY_FORCE_INLINE void FlatWrite8(uint32_t a, uint8_t v) { uint8_t* p = dtl::P(a, 1, true); if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint8_t)dtl::bf_unview((uintptr_t)p, 1, v); *p = v; }
MKW_MEMORY_FORCE_INLINE void FlatWrite16(uint32_t a, uint16_t v) { uint8_t* p = dtl::P(a, 2, true); if (dtl::is_shadow(p)) v = __builtin_bswap16(v); else if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint16_t)dtl::bf_unview((uintptr_t)p, 2, v); memcpy(p, &v, 2); }
MKW_MEMORY_FORCE_INLINE void FlatWrite32(uint32_t a, uint32_t v) { uint8_t* p = dtl::P(a, 4, true); v = dtl::conv_store(v, p); if (dtl::is_shadow(p)) v = __builtin_bswap32(v); else if (__builtin_expect((*dtl::g_bf_onp), 0)) { dtl::bf_note_store((uintptr_t)p, v); v = dtl::bf_unview((uintptr_t)p, 4, v); } memcpy(p, &v, 4); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat32(uint32_t a, double d) { float f = (float)d; uint32_t v; memcpy(&v, &f, 4); uint8_t* p = dtl::P(a, 4, true); if (dtl::is_shadow(p)) v = __builtin_bswap32(v); memcpy(p, &v, 4); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat64(uint32_t a, double d) { uint64_t v; memcpy(&v, &d, 8); uint8_t* p = dtl::P(a, 8, true); if (dtl::is_shadow(p)) v = __builtin_bswap64(v); else if (dtl::is_gstack(p)) v = (v << 32) | (v >> 32); memcpy(p, &v, 8); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam8(uint32_t a, uint8_t v) { FlatWrite8(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam16(uint32_t a, uint16_t v) { FlatWrite16(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam32(uint32_t a, uint32_t v) { FlatWrite32(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat32(uint32_t a, double d) { FlatWriteFloat32(a, d); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat64(uint32_t a, double d) { FlatWriteFloat64(a, d); }
}
namespace MemoryInline {
// "resolved range" accessors: the translation resolves a guest range once and then addresses it by offset
inline uint8_t* ResolveRangeHost(uint32_t base, int32_t minOffset, uint32_t length, bool, bool w) { return dtl::P(base + (uint32_t)minOffset, length, true); }
inline const uint8_t* RR(const uint8_t* r, uint32_t o) { const uint8_t* p = r + o; if ((uintptr_t)p - dtl::g_vt_lo < dtl::g_vt_hi - dtl::g_vt_lo && dtl::in_vtable(p)) p = dtl::vt_resolve(p); return p; }
inline uint8_t ReadResolved8(uint8_t* r, uint32_t o, uint32_t) { const uint8_t* p = RR(r, o); uint8_t v = *p; if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint8_t)dtl::bf_view((uintptr_t)p, 1, v); return v; }
inline uint16_t ReadResolved16(uint8_t* r, uint32_t o, uint32_t) { const uint8_t* p = RR(r, o); uint16_t v; memcpy(&v, p, 2); if (dtl::is_shadow(r)) return __builtin_bswap16(v); if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint16_t)dtl::bf_view((uintptr_t)p, 2, v); return v; }
inline uint32_t ReadResolved32(uint8_t* r, uint32_t o, uint32_t) { const uint8_t* p = RR(r, o); uint32_t v; memcpy(&v, p, 4); if (dtl::is_shadow(r)) return __builtin_bswap32(v); if (__builtin_expect((*dtl::g_bf_onp), 0)) v = dtl::bf_view((uintptr_t)p, 4, v); return v; }
inline float ReadResolvedFloat32(uint8_t* r, uint32_t o, uint32_t) { uint32_t v = ReadResolved32(r, o, 0); float f; memcpy(&f, &v, 4); return f; }
inline double ReadResolvedFloat64(uint8_t* r, uint32_t o, uint32_t) { uint64_t v; memcpy(&v, RR(r, o), 8); if (dtl::is_shadow(r)) v = __builtin_bswap64(v); else if (dtl::is_gstack(r)) v = (v << 32) | (v >> 32); double d; memcpy(&d, &v, 8); return d; }
struct ResolvedLoadPair { uint32_t first = 0, second = 0; bool valid = false; };
inline ResolvedLoadPair ReadResolvedPair32(uint8_t* r, uint32_t o) { return { ReadResolved32(r, o, 0), ReadResolved32(r, o + 4, 0), true }; }
inline void WriteResolved8(uint8_t* r, uint32_t o, uint32_t, uint8_t v) { if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint8_t)dtl::bf_unview((uintptr_t)(r + o), 1, v); r[o] = v; }
inline void WriteResolved16(uint8_t* r, uint32_t o, uint32_t, uint16_t v) { if (dtl::is_shadow(r)) v = __builtin_bswap16(v); else if (__builtin_expect((*dtl::g_bf_onp), 0)) v = (uint16_t)dtl::bf_unview((uintptr_t)(r + o), 2, v); memcpy(r + o, &v, 2); }
inline void WriteResolved32(uint8_t* r, uint32_t o, uint32_t, uint32_t v) { v = dtl::conv_store(v, r + o); if (dtl::is_shadow(r)) v = __builtin_bswap32(v); else if (__builtin_expect((*dtl::g_bf_onp), 0)) { dtl::bf_note_store((uintptr_t)(r + o), v); v = dtl::bf_unview((uintptr_t)(r + o), 4, v); } memcpy(r + o, &v, 4); }
inline void WriteResolvedFloat32(uint8_t* r, uint32_t o, uint32_t, double d) { float f = (float)d; uint32_t v; memcpy(&v, &f, 4); if (dtl::is_shadow(r)) v = __builtin_bswap32(v); memcpy(r + o, &v, 4); }
inline void WriteResolvedFloat64(uint8_t* r, uint32_t o, uint32_t, double d) { uint64_t v; memcpy(&v, &d, 8); if (dtl::is_shadow(r)) v = __builtin_bswap64(v); else if (dtl::is_gstack(r)) v = (v << 32) | (v >> 32); memcpy(r + o, &v, 8); }
inline bool WriteResolvedPair32(uint8_t* r, uint32_t o, uint64_t v) { WriteResolved32(r, o, 0, (uint32_t)(v >> 32)); WriteResolved32(r, o + 4, 0, (uint32_t)v); return true; }
}
namespace Memory {
inline uint8_t Read8(uint32_t a) { return MemoryInline::FlatRead8(a); }
inline uint16_t Read16(uint32_t a) { return MemoryInline::FlatRead16(a); }
inline uint32_t Read32(uint32_t a) { return MemoryInline::FlatRead32(a); }
inline float ReadFloat32(uint32_t a) { return MemoryInline::FlatReadFloat32(a); }
inline double ReadFloat64(uint32_t a) { return MemoryInline::FlatReadFloat64(a); }
inline void Write8(uint32_t a, uint8_t v) { MemoryInline::FlatWrite8(a, v); }
inline void Write16(uint32_t a, uint16_t v) { MemoryInline::FlatWrite16(a, v); }
inline void Write32(uint32_t a, uint32_t v) { MemoryInline::FlatWrite32(a, v); }
}
#define MKW_SDA2_CONST_F32(read, address, bits, literal) (read)
#define MKW_SDA2_CONST_F64(read, address, bits, literal) (read)
