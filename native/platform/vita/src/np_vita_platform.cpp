// Platform-layer logic that is the same on the console and on the armhf/x86 check builds: boot
// stages, per-frame accounting and the status lines of the text console, fatal-error presentation.
// Ported from the recompiled build's vita_platform.cpp + vita_frame.cpp (one/vita/src); the
// harness side of the frame hook is np_harness.cpp (NpOnFramePresented calls NpVitaOnFramePresented
// first). The Vita-only parts are inside `#if defined(__vita__)`.
#include "np_vita.h"
#include "np_vita_internal.h"
#include "plat_realclock.h"
#include "vita_lab.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#else
#include <ctime>
#endif

#ifndef NP_BUILD_VERSION
#define NP_BUILD_VERSION "unversioned"
#endif
// What the title line of the console calls this executable.
#ifndef NP_VITA_VARIANT
#define NP_VITA_VARIANT "native"
#endif

// Multiplayer (platform/sdk/net/np_lan.cpp, wave 5 NET): a LAN match's input delay, starved steps, steps per second and round trip, for the frame line.
extern "C" int NpLanNetBrief(char* out, int size) __attribute__((weak));

namespace VitaPlatform {
namespace {

std::atomic<uint64_t> g_frames{0};
char g_stage[96] = "starting";

uint64_t NowUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    return PlatRealMonotonicUs();   // not the harness's virtual clock: plat_realclock.h
#endif
}

// CPU time consumed by the calling (game) thread, in microseconds.
uint64_t ThreadCpuUs() {
#if defined(__vita__)
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
    return info.runClocks;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
#endif
}

void UpdateStatusLines(double fps, double cpuMsPerFrame, bool haveCpu) {
    VitaConsole::SetLine(0, 0x80D0FF, "Mario Strikers Charged - PS Vita native port, " NP_VITA_VARIANT " (%s)", NP_BUILD_VERSION);
    VitaConsole::SetLine(1, 0xFFFFFF, "stage : %s", g_stage);
    if (haveCpu)
        VitaConsole::SetLine(2, 0xA0FFA0, "frames: %llu   %.1f fps   game thread %.1f ms CPU/frame",
                             static_cast<unsigned long long>(g_frames.load()), fps, cpuMsPerFrame);
    else
        VitaConsole::SetLine(2, 0xA0FFA0, "frames: %llu   %.1f fps",
                             static_cast<unsigned long long>(g_frames.load()), fps);
    const VitaAudio::Stats audio = VitaAudio::GetStats();
    if (audio.open)
        VitaConsole::SetLine(3, 0xE0E0A0, "audio : %u Hz, %u ms queued, %u dropped block(s), %u underrun(s)",
                             audio.sampleRate, audio.sampleRate ? audio.queuedFrames * 1000u / audio.sampleRate : 0u,
                             audio.droppedBlocks, audio.underruns);
    else
        VitaConsole::SetLine(3, 0xE0E0A0, "audio : not started by the game yet");
    uint16_t buttons = 0;
    int stickX = 0, stickY = 0;
    VitaInput::LastState(&buttons, &stickX, &stickY);
    const struct mallinfo heap = mallinfo();
    VitaConsole::SetLine(4, 0xC0C0FF, "pad   : buttons %04X stick %+4d,%+4d    heap: %u KiB in use of %u KiB",
                         buttons, stickX, stickY,
                         static_cast<unsigned>(heap.uordblks / 1024), static_cast<unsigned>(heap.arena / 1024));
    VitaConsole::SetLine(5, 0x606060, "--------------------------------------------------------------------------------");
}

} // namespace

uint64_t FrameCount() { return g_frames.load(std::memory_order_relaxed); }

// Milliseconds since the process started (W5 BOOT: every boot line carries it, "[+N ms]").
unsigned long BootMs() {
#if defined(__vita__)
    return static_cast<unsigned long>(sceKernelGetProcessTimeWide() / 1000u);
#else
    static const uint64_t t0 = NowUs();
    return static_cast<unsigned long>((NowUs() - t0) / 1000u);
#endif
}

void BootStage(const char* text) {
    std::snprintf(g_stage, sizeof(g_stage), "%s", text);
    VitaBootScreen::Stage(text);
    if (!NP_LOGGING) return;
    char line[200];
    std::snprintf(line, sizeof line, "[vita] stage: %s  [+%lu ms]\n", text, BootMs());
    std::fputs(line, stderr);   // (one piece: the loading screen's and the clock's threads write too)
    std::fflush(stderr);
    VitaLog::Flush();
    UpdateStatusLines(0.0, 0.0, false);
    VitaConsole::Refresh();
}

void OnFramePresented() {
    const uint64_t frames = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    VitaBootScreen::Frame(frames);
    if (!NP_LOGGING) return;
    static uint64_t lastUs = NowUs();
    static uint64_t lastCpuUs = ThreadCpuUs();
    static uint64_t lastFrames = 0;

    VitaLog::FlushIfDue();
    // Memory figures (vita_memreport.cpp): the C heap's high-water mark is a sample every 30 frames.
    if (VitaMem::SampleHeap && frames % 30 == 0) VitaMem::SampleHeap();

    if (frames == 1) {
        std::snprintf(g_stage, sizeof(g_stage), "the game is presenting frames");
        char line[80];
        std::snprintf(line, sizeof line, "[vita] first frame presented  [+%lu ms]\n", BootMs());
        std::fputs(line, stderr);
        // Start-up is over: from here the log is written in batches.
        VitaLog::SetSynchronous(false);
    }
    const uint64_t now = NowUs();
    // Status lines twice a second; always on the frames that get a log line.
    if (now - lastUs < 500000u && frames != 1 && frames % 600 != 0) return;
    const uint64_t cpu = ThreadCpuUs();
    const uint64_t deltaFrames = frames - lastFrames;
    const double seconds = static_cast<double>(now - lastUs) / 1e6;
    const double fps = seconds > 0.0 ? static_cast<double>(deltaFrames) / seconds : 0.0;
    const bool haveCpu = cpu != 0 && deltaFrames != 0;
    const double cpuMs = haveCpu ? static_cast<double>(cpu - lastCpuUs) / 1000.0 / static_cast<double>(deltaFrames) : 0.0;
    UpdateStatusLines(fps, cpuMs, haveCpu);
    VitaConsole::Refresh();
    // A line in the log every 600 frames, so a run leaves a performance record.
    if (frames % 600 == 0) {
        const VitaAudio::Stats audio = VitaAudio::GetStats();
        const struct mallinfo heap = mallinfo();
        std::fprintf(stderr, "[vita] frame %llu: %.1f fps, game thread %.2f ms CPU/frame, audio underruns %u, dropped %u, C heap %u KiB in use (%u KiB taken from the system)\n",
                     static_cast<unsigned long long>(frames), fps, cpuMs, audio.underruns, audio.droppedBlocks,
                     static_cast<unsigned>(heap.uordblks / 1024), static_cast<unsigned>(heap.arena / 1024));
        char lan[160];
        if (NpLanNetBrief && NpLanNetBrief(lan, sizeof lan) > 0)
            std::fprintf(stderr, "[vita] frame %llu:%s\n", static_cast<unsigned long long>(frames), lan);
        char when[32];
        std::snprintf(when, sizeof(when), "frame %llu", static_cast<unsigned long long>(frames));
        VitaAudio::LogReport(when);
        NpJobsLogStats(when);
        if (VitaMem::Checkpoint) VitaMem::Checkpoint(when);
    }
    lastUs = now;
    lastCpuUs = cpu;
    lastFrames = frames;
}

void ShowFatal(const char* category, const char* details) noexcept {
    VitaLab::EndReason("fatal error: %s", category ? category : "");
    std::fprintf(stderr, "[vita] FATAL: %s\n[vita] %s\n", category ? category : "", details ? details : "");
    std::fflush(stderr);
    VitaLog::Flush();
    VitaConsole::ShowFatal(category, details);
    VitaLog::Flush();
}

} // namespace VitaPlatform

// Multiplayer's ad hoc radio (platform/sdk/net/vlink_vita_radio.cpp): its state machine advances one step per call, on the game thread
// that owns it. Weak: a build without the net layer has no radio.
extern "C" void np_vita_radio_pump(void) __attribute__((weak));

// The hook np_harness.cpp's NpOnFramePresented calls first, on the game thread at every frame end.
extern "C" void NpVitaOnFramePresented(void) {
    if (np_vita_radio_pump) np_vita_radio_pump();   // once per presented frame (the game polls the link's receive ring itself)
    VitaPlatform::OnFramePresented();
    VitaLab::OnFramePresented();   // lab builds: the deliberate-failure switches
}

extern "C" void NpPlatformFatal(const char* category, const char* details) { VitaPlatform::ShowFatal(category, details); }

// W5 BOOT: a moment of the game's start (np/boot.h NpBootMark: the boot screens, the title screen), with the time since the process started.
// The game's boot screens end the loading screen.
extern "C" void NpPlatformBootMark(const char* what) {
    if (what && std::strncmp(what, "boot screens", 12) == 0) VitaBootScreen::Finish("the game shows its boot screens");
    if (!NP_LOGGING) return;
    char line[200];
    std::snprintf(line, sizeof line, "[boot] %s  [+%lu ms, frame %llu]\n", what ? what : "", VitaPlatform::BootMs(),
                  static_cast<unsigned long long>(VitaPlatform::FrameCount()));
    std::fputs(line, stderr);
}

#if defined(__vita__)
extern "C" int NpVitaCpuMHz(void) { return scePowerGetArmClockFrequency(); }
extern "C" void NpVitaKeepAwake(void) { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }
extern "C" void NpPlatformExit(int code) {
    VitaLab::EndReason("the game left through NpPlatformExit(%d)", code);
    VitaLog::Flush();
    sceKernelExitProcess(code);
    for (;;) sceKernelDelayThread(1000000);
}
#endif
