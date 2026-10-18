// The runtime names the Vita host (vita/vgx_host_vita.cpp) uses for the platform's services, provided over the optional
// callbacks of native/vgx_native_vita.h. Included under the runtime's own header names (vita_platform.h, plat_frame_stats.h,
// plat_split.h) by the native build, so that the host file needs no change for them.
#pragma once
// The renderer's copies of the runtime's names must not collide with the platform layer's own (np/platform/vita uses the same
// namespaces with other signatures and structs: its VitaInput::Raw has five fields, this one has one, and both have a
// Read(Raw&); its VitaPlatform::BootStage is a real function, defined here too). A same-named strong and weak/inline pair is
// resolved silently by the linker, and a call through the wrong definition would write past a Raw: so every namespace of this
// shim gets a private name. The host file (vita/vgx_host_vita.cpp) is written with the runtime's names; the macros rename its
// uses consistently, because every shim header includes this one first.
#define VitaConsole VgxnVitaConsole
#define VitaBootScreen VgxnVitaBootScreen
#define VitaThreads VgxnVitaThreads
#define VitaPlatform VgxnVitaPlatform
#define VitaMem VgxnVitaMem
#define VitaInput VgxnVitaInput
#define PlatFrameStats VgxnPlatFrameStats
#define PlatSplit VgxnPlatSplit
#include <psp2/ctrl.h>

#include <cstdint>

#include "vgx_native.h"
#include "vgx_native_vita.h"

namespace vgxn_vita {
const vgxn_vita_services& Services();
}

namespace VitaConsole {
inline const uint32_t* Pixels() { const auto& s = vgxn_vita::Services(); return s.console_pixels ? s.console_pixels() : nullptr; }
inline bool OwnsDisplay() { const auto& s = vgxn_vita::Services(); return s.console_owns_display && s.console_owns_display() != 0; }
inline void SetDisplayOwner(bool console) { const auto& s = vgxn_vita::Services(); if (s.console_set_display_owner) s.console_set_display_owner(console ? 1 : 0); }
} // namespace VitaConsole

namespace VitaBootScreen {
inline bool Compose(void* framebuffer) { const auto& s = vgxn_vita::Services(); return s.boot_compose && s.boot_compose(framebuffer) != 0; }
inline void Progress(int kind, unsigned done, unsigned total) { const auto& s = vgxn_vita::Services(); if (s.boot_progress) s.boot_progress(kind, done, total); }
} // namespace VitaBootScreen

namespace VitaThreads {
enum Role { kCollector = VGXN_ROLE_COLLECTOR, kDisplayQueue = VGXN_ROLE_DISPLAY_QUEUE, kPlayer = VGXN_ROLE_PLAYER, kRender = VGXN_ROLE_RENDER };
int CoreMask(int role);   // services' value, or the standalone plan (render + display queue core 1, collector + player core 2)
inline void PinCurrentThread(int role) { const auto& s = vgxn_vita::Services(); if (s.pin_current_thread) s.pin_current_thread(role); }
} // namespace VitaThreads

namespace VitaPlatform {
void BootStage(const char* what);
} // namespace VitaPlatform

namespace VitaMem {
extern void (*Checkpoint)(const char* what);
} // namespace VitaMem

namespace VitaInput {
struct Raw { uint32_t buttons = 0; };
constexpr uint32_t kSelect = SCE_CTRL_SELECT, kStart = SCE_CTRL_START, kUp = SCE_CTRL_UP, kDown = SCE_CTRL_DOWN, kRight = SCE_CTRL_RIGHT;
inline bool Read(Raw& raw) {
    const auto& s = vgxn_vita::Services();
    unsigned int b = 0;
    if (!s.read_buttons || !s.read_buttons(&b)) return false;
    raw.buttons = b;
    return true;
}
} // namespace VitaInput

namespace PlatFrameStats {
inline unsigned long long FrameCount() { return vgxn_frame_count(); }
} // namespace PlatFrameStats

namespace PlatSplit {
inline bool OnWorker() { return false; }   // (the recompiled build's two-core split: not part of the native port)
} // namespace PlatSplit
