// difftest shim: flat big-endian guest memory (24 MB MEM1 at 0x80000000) instead of the recompiler's runtime
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
extern uint8_t* g_dt_guest;
#define MKW_MEMORY_FORCE_INLINE inline __attribute__((always_inline))
namespace dtmem {
inline uint8_t* P(uint32_t a, uint32_t n) {
    uint32_t o = a & 0x01FFFFFFu;
    if ((a < 0x80000000u && a != 0) || o + n > 0x01800000u) { fprintf(stderr, "DT: guest access out of range %08x\n", a); abort(); }
    return g_dt_guest + o;
}
}
namespace MemoryInline {
MKW_MEMORY_FORCE_INLINE uint8_t FlatRead8(uint32_t a) { return *dtmem::P(a, 1); }
MKW_MEMORY_FORCE_INLINE uint16_t FlatRead16(uint32_t a) { uint16_t v; memcpy(&v, dtmem::P(a, 2), 2); return __builtin_bswap16(v); }
MKW_MEMORY_FORCE_INLINE uint32_t FlatRead32(uint32_t a) { uint32_t v; memcpy(&v, dtmem::P(a, 4), 4); return __builtin_bswap32(v); }
MKW_MEMORY_FORCE_INLINE float FlatReadFloat32(uint32_t a) { uint32_t b = FlatRead32(a); float f; memcpy(&f, &b, 4); return f; }
MKW_MEMORY_FORCE_INLINE double FlatReadFloat64(uint32_t a) { uint32_t hi = FlatRead32(a), lo = FlatRead32(a + 4); uint64_t b = ((uint64_t)hi << 32) | lo; double d; memcpy(&d, &b, 8); return d; }
MKW_MEMORY_FORCE_INLINE void FlatWrite8(uint32_t a, uint8_t v) { *dtmem::P(a, 1) = v; }
MKW_MEMORY_FORCE_INLINE void FlatWrite16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(dtmem::P(a, 2), &v, 2); }
MKW_MEMORY_FORCE_INLINE void FlatWrite32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(dtmem::P(a, 4), &v, 4); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat32(uint32_t a, double d) { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); FlatWrite32(a, b); }
MKW_MEMORY_FORCE_INLINE void FlatWriteFloat64(uint32_t a, double d) { uint64_t b; memcpy(&b, &d, 8); FlatWrite32(a, (uint32_t)(b >> 32)); FlatWrite32(a + 4, (uint32_t)b); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam8(uint32_t a, uint8_t v) { FlatWrite8(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam16(uint32_t a, uint16_t v) { FlatWrite16(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRam32(uint32_t a, uint32_t v) { FlatWrite32(a, v); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat32(uint32_t a, double d) { FlatWriteFloat32(a, d); }
MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat64(uint32_t a, double d) { FlatWriteFloat64(a, d); }
}
namespace MemoryInline {
// "resolved range" accessors of the recompiler's runtime: ResolveRangeHost gives the host pointer of a guest range
inline uint8_t* ResolveRangeHost(uint32_t base, int32_t minOffset, uint32_t length, bool, bool) { return dtmem::P(base + (uint32_t)minOffset, length); }
inline uint8_t ReadResolved8(uint8_t* r, uint32_t o, uint32_t) { return r[o]; }
inline uint16_t ReadResolved16(uint8_t* r, uint32_t o, uint32_t) { uint16_t v; memcpy(&v, r + o, 2); return __builtin_bswap16(v); }
inline uint32_t ReadResolved32(uint8_t* r, uint32_t o, uint32_t) { uint32_t v; memcpy(&v, r + o, 4); return __builtin_bswap32(v); }
inline float ReadResolvedFloat32(uint8_t* r, uint32_t o, uint32_t) { uint32_t b = ReadResolved32(r, o, 0); float f; memcpy(&f, &b, 4); return f; }
inline double ReadResolvedFloat64(uint8_t* r, uint32_t o, uint32_t) { uint64_t v; memcpy(&v, r + o, 8); v = __builtin_bswap64(v); double d; memcpy(&d, &v, 8); return d; }
struct ResolvedLoadPair { uint32_t first = 0, second = 0; bool valid = false; };
inline ResolvedLoadPair ReadResolvedPair32(uint8_t* r, uint32_t o) { uint64_t v; memcpy(&v, r + o, 8); v = __builtin_bswap64(v); return { (uint32_t)(v >> 32), (uint32_t)v, true }; }
inline void WriteResolved8(uint8_t* r, uint32_t o, uint32_t, uint8_t v) { r[o] = v; }
inline void WriteResolved16(uint8_t* r, uint32_t o, uint32_t, uint16_t v) { v = __builtin_bswap16(v); memcpy(r + o, &v, 2); }
inline void WriteResolved32(uint8_t* r, uint32_t o, uint32_t, uint32_t v) { v = __builtin_bswap32(v); memcpy(r + o, &v, 4); }
inline void WriteResolvedFloat32(uint8_t* r, uint32_t o, uint32_t, double d) { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); WriteResolved32(r, o, 0, b); }
inline void WriteResolvedFloat64(uint8_t* r, uint32_t o, uint32_t, double d) { uint64_t b; memcpy(&b, &d, 8); b = __builtin_bswap64(b); memcpy(r + o, &b, 8); }
inline bool WriteResolvedPair32(uint8_t* r, uint32_t o, uint64_t v) { v = __builtin_bswap64(v); memcpy(r + o, &v, 8); return true; }
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
