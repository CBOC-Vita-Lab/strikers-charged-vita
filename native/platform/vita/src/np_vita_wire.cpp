// Wires the Vita platform layer (this directory) into the pieces the native game links:
//   * NPS's SDK layer (platform/sdk): audio sink, input source, rumble, quit hook, thread-plan hook;
//   * NPR's renderer (vgx on vitaGL, platform/gx): the services of native/vgx_native_vita.h.
// Everything is a weak reference, so the smoke application (no SDK, no renderer) links the same file.
// Called once from main() (np_vita_main.cpp) before the game's main; the SDK itself was started by
// its static constructor (platform/compat/np_sdk_boot.cpp), which only configures from the
// environment, so every hook registered here is in place before the game opens a device.
#if defined(__vita__)

#include "np_vita.h"
#include "np_vita_internal.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <psp2/kernel/threadmgr.h>

// ---- NPS (platform/sdk/include/np/np_sdk.h, np_plat.h): redeclared, so that this file needs neither
// the SDK headers nor their force-included companions. A change there is caught by NpWireSelfCheck().
extern "C" {
struct NpAudioSink {
    int (*open)(uint32_t sample_rate);
    void (*write)(const int16_t* stereo_lr, uint32_t frames);
    void (*close)(void);
};
struct NpRawPad {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    int touch_count;
    struct { int id; float x, y; } touch[6];
    int has_motion;
    float accel[3];
    float gyro[3];
    int rear_count;                                // W5 CTL: the rear panel and the sample's source (np_sdk.h)
    struct { int id; float x, y; } rear[6];
    int source;
};
typedef int (*NpInputSourceFn)(int pad, NpRawPad* out);
typedef void (*NpRumbleFn)(int pad, int on);
void NpSdk_SetAudioSink(const NpAudioSink* sink) __attribute__((weak));
void NpSdk_SetInputSource(NpInputSourceFn fn) __attribute__((weak));
void NpSdk_SetQuitHook(void (*hook)(int reason)) __attribute__((weak));
int NpSdk_QuitRequested(void) __attribute__((weak));
void NpSdk_SetInputBlocked(int blocked) __attribute__((weak));   // NPS2: 1 = the platform owns the display (the controller reads neutral)

enum NpRole { NP_ROLE_GAME = 0, NP_ROLE_OSTHREAD, NP_ROLE_AUDIO, NP_ROLE_IO, NP_ROLE_TIMER, NP_ROLE_HELPER, NP_ROLE_COUNT };
typedef void (*NpThreadHook)(const char* name, NpRole role);
void np_plat_set_thread_hook(NpThreadHook hook) __attribute__((weak));
int NpVirtualTimeActive(void) __attribute__((weak));   // platform/harness/include/np_time.h

// ---- NPR (platform/gx/include/np_gx.h and vgx native/vgx_native_vita.h)
enum { VGXN_ROLE_COLLECTOR = 0, VGXN_ROLE_DISPLAY_QUEUE = 1, VGXN_ROLE_PLAYER = 2, VGXN_ROLE_RENDER = 3 };
struct vgxn_vita_services {
    unsigned int size;
    void (*boot_stage)(const char* what);
    void (*mem_checkpoint)(const char* what);
    int (*core_mask)(int role);
    void (*pin_current_thread)(int role);
    int (*console_owns_display)(void);
    void (*console_set_display_owner)(int console);
    const unsigned int* (*console_pixels)(void);
    int (*read_buttons)(unsigned int* sce_ctrl_buttons);
    int (*boot_compose)(void* framebuffer);                          // W5 BOOT: the loading screen (vita_bootscreen.cpp)
    void (*boot_progress)(int kind, unsigned done, unsigned total);
};
void vgxn_vita_set_services(const vgxn_vita_services* services) __attribute__((weak));
// np_gx.h (the glue): the renderer is started here, with the surface the picture is shown on, instead of by GXInit with defaults.
struct np_gx_config {
    unsigned int size;
    int render_width, render_height;
    int surface_width, surface_height;
    const char* dump_dir;
    int array_default_le;
};
int np_gx_start(const np_gx_config* config) __attribute__((weak));
}

namespace {

// ---- audio -----------------------------------------------------------------------------------
// The SDK's audio thread delivers a 3 ms block (96 frames at 32 kHz) per call and treats the call's
// blocking as the audio clock. The platform ring (np_vita_audio.cpp) is a queue with room for ~120 ms
// (elastic policy: NpAudioSpaceFrames says how many frames fit): wait here until the block fits, so that
// the SDK's mixer runs at the pace the device drains the ring and nothing is dropped.
int SinkOpen(uint32_t rate) { return NpAudioOpen(rate) ? 0 : -1; }

void SinkWrite(const int16_t* lr, uint32_t frames) {
    // Virtual time (MSC_VIRTUAL_TIME=1: emulator and harness runs): the game's audio clock is virtual, the device's is real. Waiting for ring
    // space would pace a game that runs several times faster than real time to the device (up to 200 ms per 3 ms block), and nobody listens to a run
    // that is not in real time: the block is dropped.
    if (NpVirtualTimeActive && NpVirtualTimeActive()) return;
    // (17 Oct: wait for the queue's pace level, not for room under its 320 ms limit: see vita_audio_queue.h PaceFrames)
    for (int waited = 0; waited < 200; ++waited) {            // at most ~200 ms: a stalled device must not hang the game
        const int space = NpAudioPaceFrames();
        if (space < 0 || space >= static_cast<int>(frames)) break;
        sceKernelDelayThread(1000);
    }
    NpAudioPushLE16(lr, frames);
}

void SinkClose() {}

// ---- input -----------------------------------------------------------------------------------
// NpRawPad is described in the Vita's terms already (SCE_CTRL_* bits, sticks 0..255, touch in display pixels).
// With a nav script running (MSC_INPUT_SCRIPT) the script's pad (GameCube bits, +-72 sticks: np_harness.h) is converted back to
// raw terms the way NPB2's host source does (platform/harness/src/np_host_main.cpp; the SDK layer converts forwards: cross = A,
// square = B, circle = X, triangle = Y, select = Z, a stick of 72 units is full deflection = raw offset 102, dead zone 16),
// and the console's real controls are ignored while it presses something; a finished script hands the controls back.
void ScriptStick(int sx, int sy, uint8_t* ox, uint8_t* oy) {
    const float x = sx / 72.0f, y = sy / 72.0f;
    const float m = std::sqrt(x * x + y * y);
    *ox = *oy = 128;
    if (m < 1e-4f) return;
    const float mag = 16.0f + (m > 1.0f ? 1.0f : m) * 86.0f;
    const float rx = 128.0f + x / m * mag, ry = 128.0f - y / m * mag;
    *ox = static_cast<uint8_t>(rx < 0 ? 0 : rx > 255 ? 255 : std::lround(rx));
    *oy = static_cast<uint8_t>(ry < 0 ? 0 : ry > 255 ? 255 : std::lround(ry));
}

int InputSource(int pad, NpRawPad* out) {
    if (pad != 0 || !out) return 0;                            // one controller: the console's own
    std::memset(out, 0, sizeof(*out));
    out->lx = out->ly = out->rx = out->ry = 128;
    if (NpHarnessActive() && NpHarnessOwnsPad()) {   // a script plays; logging alone (MSC_WATCH, MSC_FRAME_TIMING) keeps the real pad
        NpPadState st;
        std::memset(&st, 0, sizeof(st));
        if (NpHarnessFillPad(&st)) {
            static const struct { uint16_t gc; uint32_t raw; } kMap[] = {
                {0x0001, 0x0080}, {0x0002, 0x0020}, {0x0004, 0x0040}, {0x0008, 0x0010},   // left right down up
                {0x0010, 0x0001}, {0x0020, 0x0200}, {0x0040, 0x0100}, {0x0100, 0x4000},   // Z=select R L A=cross
                {0x0200, 0x8000}, {0x0400, 0x2000}, {0x0800, 0x1000}, {0x1000, 0x0008}};  // B=square X=circle Y=triangle START
            for (const auto& m : kMap) if (st.buttons & m.gc) out->buttons |= m.raw;
            ScriptStick(st.stickX, st.stickY, &out->lx, &out->ly);
            ScriptStick(st.substickX, st.substickY, &out->rx, &out->ry);
            return 1;
        }
        return 1;                                              // a script owns the pad: untouched controller between its commands
    }
    NpVitaInput in;
    if (!NpVitaReadInput(&in)) return 1;
    out->buttons = in.buttons;
    out->lx = in.lx; out->ly = in.ly; out->rx = in.rx; out->ry = in.ry;
    int n = in.touchCount;
    if (n > 6) n = 6;
    out->touch_count = n;
    for (int i = 0; i < n; ++i) {
        out->touch[i].id = in.touch[i].id;
        out->touch[i].x = in.touch[i].x;
        out->touch[i].y = in.touch[i].y;
    }
    n = in.rearCount;
    if (n > 6) n = 6;
    out->rear_count = n;
    for (int i = 0; i < n; ++i) {
        out->rear[i].id = in.rear[i].id;
        out->rear[i].x = in.rear[i].x;
        out->rear[i].y = in.rear[i].y;
    }
    return 1;
}

void QuitHook(int reason) {
    std::fprintf(stderr, "[vita] the game asked to leave (OS_SHUTDOWN reason %d)\n", reason);
    VitaPlan::Heartbeat(VitaPlatform::FrameCount() >= 120 ? 900 : 0);   // (a clean leave after 4 s of play counts as a start that worked)
    NpPlatformExit(0);
}

void ThreadHook(const char* name, NpRole role) {
    static const char* const names[NP_ROLE_COUNT] = {"game", "os thread", "audio", "io", "timer", "helper"};
    std::fprintf(stderr, "[vita] sdk thread '%s' role %s\n", name ? name : "", (unsigned)role < NP_ROLE_COUNT ? names[role] : "?");
}

// ---- renderer services -----------------------------------------------------------------------
VitaThreads::Role RoleOf(int vgxRole) {
    switch (vgxRole) {
    case VGXN_ROLE_COLLECTOR: return VitaThreads::kCollector;
    case VGXN_ROLE_DISPLAY_QUEUE: return VitaThreads::kDisplayQueue;
    case VGXN_ROLE_PLAYER: return VitaThreads::kPlayer;
    default: return VitaThreads::kRender;
    }
}
void VgxBootStage(const char* what) { VitaPlatform::BootStage(what ? what : ""); }
void VgxMemCheckpoint(const char* what) { if (VitaMem::Checkpoint) VitaMem::Checkpoint(what ? what : ""); }
int VgxCoreMask(int role) { return VitaThreads::CoreMask(RoleOf(role)); }
void VgxPin(int role) { VitaThreads::PinCurrentThread(RoleOf(role), true); }
int VgxConsoleOwns() { return VitaConsole::OwnsDisplay() ? 1 : 0; }
void VgxSetOwner(int console) {
    VitaConsole::SetDisplayOwner(console != 0);
    if (NpSdk_SetInputBlocked) NpSdk_SetInputBlocked(console != 0 ? 1 : 0);   // the debug chord shows the console: the game must not read the pad meanwhile
}
const unsigned int* VgxPixels() { return VitaConsole::Pixels(); }
int VgxBootCompose(void* framebuffer) { return VitaBootScreen::Compose(framebuffer); }
void VgxBootProgress(int kind, unsigned done, unsigned total) { VitaBootScreen::Progress(kind, done, total); }
int VgxButtons(unsigned int* buttons) {
    VitaInput::Raw raw;
    if (!buttons || !VitaInput::Read(raw)) return 0;
    *buttons = raw.buttons;
    return 1;
}

}  // namespace

// ---- stall diagnosis -------------------------------------------------------------------------------
// Lab runs have the watchdog of vita_lab.cpp; an ordinary start (and Vita3K runs, where the lab server is off) had nothing, and the first
// post-merge run just stopped writing its log (a lost wake-up in the SDK scheduler). This thread notices that no frame was presented for
// 15 s and says what the game thread is waiting for; it never touches the game. MSC_VITA_STALLWATCH=0 turns it off.
extern "C" uint32_t VIGetRetraceCount(void) __attribute__((weak));
SceUID g_gameThread = 0;
int StallWatch(SceSize, void*) {
    uint64_t lastFrames = ~0ull;
    int quiet = 0;
    for (;;) {
        sceKernelDelayThread(5 * 1000 * 1000);
        const uint64_t frames = VitaPlatform::FrameCount();
        VitaPlan::Heartbeat(frames);   // (the core plan's guard file: cleared once 300 frames were presented)
        if (frames != lastFrames) { lastFrames = frames; quiet = 0; continue; }
        quiet += 5;
        if (quiet == 30) VitaPlan::Failed("no frame for 30 s");
        if (quiet != 15 && !(quiet > 15 && quiet % 30 == 0)) continue;
        SceKernelThreadInfo info;
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        const int r = sceKernelGetThreadInfo(g_gameThread, &info);
        std::fprintf(stderr, "[vita] STALL: no frame presented for %d s after frame %llu; game thread info 0x%X: status 0x%X, wait type 0x%X, wait id 0x%X, "
                             "run clocks %llu; retraces %u; audio queued %u frames\n",
                     quiet, static_cast<unsigned long long>(frames), static_cast<unsigned>(r), static_cast<unsigned>(info.status),
                     static_cast<unsigned>(info.waitType), static_cast<unsigned>(info.waitId), static_cast<unsigned long long>(info.runClocks),
                     VIGetRetraceCount ? VIGetRetraceCount() : 0u, VitaAudio::GetStats().queuedFrames);
        std::fflush(stderr);
    }
    return 0;
}

namespace VitaPlatform {

// Returns how many of the five hooks were available (5 = SDK layer and renderer both linked).
int WireSdkAndRenderer() {
    int wired = 0;
    if (NpSdk_SetAudioSink) {
        static const NpAudioSink sink = {SinkOpen, SinkWrite, SinkClose};
        NpSdk_SetAudioSink(&sink);
        ++wired;
    }
    if (NpSdk_SetInputSource) { NpSdk_SetInputSource(InputSource); ++wired; }
    if (NpSdk_SetQuitHook) { NpSdk_SetQuitHook(QuitHook); ++wired; }
    if (np_plat_set_thread_hook) np_plat_set_thread_hook(ThreadHook);
    if (vgxn_vita_set_services) {
        static const vgxn_vita_services services = {
            sizeof(vgxn_vita_services), VgxBootStage, VgxMemCheckpoint, VgxCoreMask, VgxPin,
            VgxConsoleOwns, VgxSetOwner, VgxPixels, VgxButtons, VgxBootCompose, VgxBootProgress};
        vgxn_vita_set_services(&services);
        ++wired;
    }
    // The 960 x 544 surface (NPR2's request, docs/wave2-NPR2.md section 3): the glue's dynamic aspect widens the game's 16:9 3D projections to the
    // shape of the surface the picture is shown on (np_gx_surface_size), and vgx's Vita host takes "wider than 4:3" for the game's widescreen
    // mode: picture stretched over the whole screen. With the game in 4:3 (NP_WIDESCREEN=0) the surface is the 640 x 480 box (bars at the sides).
    // Without this call GXInit starts the renderer with a NULL config = 640 x 480: the full-screen picture would be a 725 x 544 box.
    if (np_gx_start) {
        const char* w = std::getenv("NP_WIDESCREEN");
        const bool wide = !w || !*w || std::atoi(w) != 0;
        np_gx_config cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.size = sizeof(cfg);
        cfg.surface_width = wide ? 960 : 640;
        cfg.surface_height = wide ? 544 : 480;
        cfg.array_default_le = 0;
        std::fprintf(stderr, "[vita] renderer start-up: surface %d x %d (the game's widescreen mode is %s)\n", cfg.surface_width, cfg.surface_height, wide ? "on" : "off");
        const int ok = np_gx_start(&cfg);
        std::fprintf(stderr, "[vita] renderer start-up %s\n", ok ? "done" : "FAILED");
        ++wired;
    }
    if (const char* e = std::getenv("MSC_VITA_STALLWATCH"); !e || std::strcmp(e, "0") != 0) {
        g_gameThread = sceKernelGetThreadId();
        // (17 Oct: the priority was default + 40 = 200, past the lowest a user thread may have (191): the thread was never created, so the
        //  core plan's guard was never cleared and every start stepped down a plan, C -> Cn -> A -> S, on the maintainer console.)
        const SceUID t = sceKernelCreateThread("np_stallwatch", StallWatch, 0x10000100 + 30, 16 * 1024, 0, 0x40000, nullptr);   // core 2, priority 190
        const int r = t >= 0 ? sceKernelStartThread(t, 0, nullptr) : t;
        if (r < 0) std::fprintf(stderr, "[vita] the stall watch thread did not start (0x%08X): no stall report, and the core plan's guard is not cleared\n", static_cast<unsigned>(r));
    }
    std::fprintf(stderr, "[vita] wiring: SDK audio sink %s, input source %s, quit hook %s, renderer services %s\n",
                 NpSdk_SetAudioSink ? "set" : "(SDK layer not linked)", NpSdk_SetInputSource ? "set" : "-",
                 NpSdk_SetQuitHook ? "set" : "-", vgxn_vita_set_services ? "set" : "(renderer not linked)");
    return wired;
}

}  // namespace VitaPlatform

#endif  // __vita__
