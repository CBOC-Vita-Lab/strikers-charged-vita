#pragma once
// dtlive: paired-single loads/stores (psq_l / psq_st) with the GQR quantisation types the game uses (float, u8, u16, s8, s16), on the shim's memory model.
// GQR: LD_TYPE bits 16-18, LD_SCALE bits 24-29 (signed); ST_TYPE bits 0-2, ST_SCALE bits 8-13 (signed).
#include "memory.h"
#include <cmath>
namespace dtl {
inline int gqr_scale(uint32_t v) { int s = (int)(v & 0x3F); return s >= 32 ? s - 64 : s; }
inline double psq_load_elem(uint32_t type, int scale, uint32_t& addr) {
    double v; float f;
    switch (type) {
    case 0: f = MemoryInline::FlatReadFloat32(addr); addr += 4; return f;
    case 4: v = (double)MemoryInline::FlatRead8(addr); addr += 1; break;
    case 5: v = (double)MemoryInline::FlatRead16(addr); addr += 2; break;
    case 6: v = (double)(int8_t)MemoryInline::FlatRead8(addr); addr += 1; break;
    case 7: v = (double)(int16_t)MemoryInline::FlatRead16(addr); addr += 2; break;
    default: v = 0; break;
    }
    return (double)(float)(v * std::ldexp(1.0, -scale));
}
inline void psq_store_elem(uint32_t type, int scale, uint32_t& addr, double d) {
    switch (type) {
    case 0: MemoryInline::FlatWriteFloat32(addr, d); addr += 4; return;
    default: break;
    }
    double x = std::ldexp((double)(float)d, scale);
    if (type == 4) { double c = std::min(255.0, std::max(0.0, x)); MemoryInline::FlatWrite8(addr, (uint8_t)(int)c); addr += 1; }
    else if (type == 5) { double c = std::min(65535.0, std::max(0.0, x)); MemoryInline::FlatWrite16(addr, (uint16_t)(int)c); addr += 2; }
    else if (type == 6) { double c = std::min(127.0, std::max(-128.0, x)); MemoryInline::FlatWrite8(addr, (uint8_t)(int8_t)(int)c); addr += 1; }
    else if (type == 7) { double c = std::min(32767.0, std::max(-32768.0, x)); MemoryInline::FlatWrite16(addr, (uint16_t)(int16_t)(int)c); addr += 2; }
}
template <uint32_t W> inline double psq_load(uint32_t gqr, uint32_t addr) {
    uint32_t type = (gqr >> 16) & 7u; int scale = gqr_scale(gqr >> 24);
    float a = (float)psq_load_elem(type, scale, addr);
    float b = (W == 0u) ? (float)psq_load_elem(type, scale, addr) : 1.0f;
    return PpcPackPairedInline(a, b);
}
template <uint32_t W> inline void psq_store(uint32_t gqr, uint32_t addr, double value) {
    uint32_t type = gqr & 7u; int scale = gqr_scale(gqr >> 8);
    psq_store_elem(type, scale, addr, PpcGetPs0Inline(value));
    if constexpr (W == 0u) psq_store_elem(type, scale, addr, PpcGetPs1Inline(value));
}
}
template <uint32_t W, uint32_t I, uint32_t GQR>
inline void PPC_PsqStKnownStackInline(CpuContext*, uint32_t addr, double value) { dtl::psq_store<W>(GQR, addr, value); }
template <uint32_t W, uint32_t I, uint32_t GQR>
inline double PPC_PsqLKnownStackInline(CpuContext*, uint32_t addr) { return dtl::psq_load<W>(GQR << 0, addr); }
template <uint32_t W, uint32_t I, uint32_t GQR>
inline void PPC_PsqStKnownInline(CpuContext*, uint32_t addr, double value) { dtl::psq_store<W>(GQR, addr, value); }
template <uint32_t W, uint32_t I, uint32_t GQR>
inline double PPC_PsqLKnownInline(CpuContext*, uint32_t addr) { return dtl::psq_load<W>(GQR, addr); }
template <uint32_t W, uint32_t I>
inline void PPC_PsqStStackInline(CpuContext* c, uint32_t addr, double value) { dtl::psq_store<W>(c->gqr[I], addr, value); }
template <uint32_t W, uint32_t I>
inline double PPC_PsqLStackInline(CpuContext* c, uint32_t addr) { return dtl::psq_load<W>(c->gqr[I], addr); }
template <uint32_t W, uint32_t I>
inline double PPC_PsqLGqrInline(CpuContext*, uint32_t gqr, uint32_t addr) { return dtl::psq_load<W>(gqr, addr); }
template <uint32_t W, uint32_t I>
inline void PPC_PsqStGqrInline(CpuContext*, uint32_t gqr, uint32_t addr, double value) { dtl::psq_store<W>(gqr, addr, value); }
