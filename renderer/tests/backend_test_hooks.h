#pragma once
#include <cstdint>

// Standalone back-end tests have no game telemetry, player thread or GPU allocator.
extern "C" {
int NpDiagActive(void) { return 0; }
uint64_t NpDiagNow(void) { return 0; }
void NpDiagGpuFrame(uint64_t) {}
void NpDiagEvent(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
int NpFpsDisplayInterval(void) { return 2; }
int vgx_vram_free_kib(void) { return -1; }
int vgx_tex_has_storage(void) { return 1; }
#ifdef __VGX_VITA__
int vglStrikersBindFramebufferDepth(unsigned int) { return 0; }
#ifndef VGX_TEST_PACKED_SHADER_LOADER
int vgx_vita_load_packed_shader(unsigned int, int, uint64_t) { return 0; }
#endif
#endif
}
namespace vgx::rec {
double TakePlayerIdleMs() { return 0; }
bool TakePipeWaitMs(double*, double*) { return false; }
}
// Standalone fixtures have neither foreground recording nor an installed HD pack.
namespace vgx {
bool FgRequested() { return false; }
}
namespace vgx::hdtex {
int Requested() { return 1; }
int Mode() { return 0; }
}
