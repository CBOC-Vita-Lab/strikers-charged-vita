// Default services of the native Vita library (see vgx_native_vita.h) and the registration function.
#include "vgx_native_vita.h"

#include <cstdio>
#include <cstring>

#include "vgx_vita_services.h"

namespace {
vgxn_vita_services g_services = {};
bool g_set = false;
} // namespace

namespace vgxn_vita {
const vgxn_vita_services& Services() { return g_services; }
} // namespace vgxn_vita

namespace VitaThreads {
int CoreMask(int role) {
    const auto& s = vgxn_vita::Services();
    if (s.core_mask) return s.core_mask(role);
    // Standalone plan: the game thread has core 0; the render thread and sceGxm's display queue core 1; vitaGL's collector and the
    // record player (MSC_VGX_BACKEND_THREAD) core 2.
    switch (role) {
    case kCollector: case kPlayer: return 1 << 2;
    case kDisplayQueue: case kRender: return 1 << 1;
    default: return 0;
    }
}
} // namespace VitaThreads

namespace VitaPlatform {
void BootStage(const char* what) {
    const auto& s = vgxn_vita::Services();
    if (s.boot_stage) s.boot_stage(what);
    else std::fprintf(stderr, "[vgx-vita] boot: %s\n", what);
}
} // namespace VitaPlatform

namespace VitaMem {
void (*Checkpoint)(const char* what) = nullptr;
} // namespace VitaMem

extern "C" void vgxn_vita_set_services(const vgxn_vita_services* services) {
    if (!services) return;
    std::memset(&g_services, 0, sizeof g_services);
    std::memcpy(&g_services, services, services->size < sizeof g_services ? services->size : sizeof g_services);
    g_set = true;
    VitaMem::Checkpoint = g_services.mem_checkpoint;
}
