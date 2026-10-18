#pragma once
#include "memory.h"
// psq_l / psq_st with GQR type 0 (plain floats, no scale), W = 0 (pair) or 1 (single): all the game's functions use.
template <uint32_t W, uint32_t I, uint32_t GQR>
inline void PPC_PsqStKnownStackInline(CpuContext*, uint32_t addr, double value)
{
    static_assert((GQR & 7u) == 0u, "dt: only float psq stores");
    MemoryInline::FlatWriteFloat32(addr, PpcGetPs0Inline(value));
    if constexpr (W == 0u) MemoryInline::FlatWriteFloat32(addr + 4, PpcGetPs1Inline(value));
}
template <uint32_t W, uint32_t I, uint32_t GQR>
inline double PPC_PsqLKnownStackInline(CpuContext*, uint32_t addr)
{
    static_assert(((GQR >> 16) & 7u) == 0u, "dt: only float psq loads");
    float a = MemoryInline::FlatReadFloat32(addr);
    float b = (W == 0u) ? MemoryInline::FlatReadFloat32(addr + 4) : 1.0f;
    return PpcPackPairedInline(a, b);
}
