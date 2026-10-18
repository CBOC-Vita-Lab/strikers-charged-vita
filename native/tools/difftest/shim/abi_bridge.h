// difftest shim for abi_bridge.h: only what the translated functions use
#pragma once
#include <cstdint>
#include "memory.h"
#include "ppc_runtime.h"
#include "isa/ppc_isa_cr.h"
void dt_dispatch(uint32_t target, CpuContext* ctx);
inline void ApplyRuntimeCallOptions(uint32_t, CpuContext*) {}
template <uint32_t Target>
inline void InvokeDirectCpu(CpuContext* ctx) { dt_dispatch(Target, ctx); }
inline void InvokeIndirectCpu(uint32_t target, CpuContext* ctx) { dt_dispatch(target, ctx); }

// the translator's "state free" call fast path is never taken here
template <uint32_t T> struct KnownTranslatedCpuCall { static constexpr bool kAvailable = false; static constexpr bool kMustRemainDynamicallyDispatchable = true; };
template <uint32_t T> inline bool IsBaseTranslatedCpuTargetActive() { return false; }
