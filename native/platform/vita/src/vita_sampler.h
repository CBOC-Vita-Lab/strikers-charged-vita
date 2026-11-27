// diag13 (MSC_SAMPLER=1, default OFF): an on-console pc sampling profiler, see vita_sampler.cpp.
#pragma once
namespace VitaSampler {
// Call once from the game thread, after the platform is up. Does nothing unless MSC_SAMPLER=1.
void Start();
}
