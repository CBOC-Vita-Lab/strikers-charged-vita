// Platform-layer logic that is the same on the console and on the armhf/x86 check builds: boot
// stages, per-frame accounting and the status lines of the text console, fatal-error presentation.
// Ported from the recompiled build's vita_platform.cpp + vita_frame.cpp (one/vita/src); the
// harness side of the frame hook is np_harness.cpp (NpOnFramePresented calls NpVitaOnFramePresented
// first). The Vita-only parts are inside `#if defined(__vita__)`.
#include "np_vita.h"
#include "np_vita_internal.h"
#include "np_vita_core3.h"   // (mp18) NpVitaCore3::GuardOnline
#include "plat_realclock.h"
#include "vita_lab.h"
#include "np_vita_settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#if defined(__vita__)
extern "C" unsigned int _newlib_heap_size_user;   // np_vita_process.cpp: the C heap's size
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

// 1.2 (STR-102): the run header of the log (the settings that decide the pace, so logs of two runs can be compared)
extern "C" int NpFpsDescribe(char* out, int size) __attribute__((weak));
extern "C" int VgxDynResDescribe(char* out, int size) __attribute__((weak));
extern "C" int VgxGfxDescribe(char* out, int size) __attribute__((weak));
extern "C" int NpSettingsDescribe(char* out, int size) __attribute__((weak));
extern "C" int NpInputDescribe(char* out, int size) __attribute__((weak));
extern "C" int vgx_vita_describe(char* out, int size) __attribute__((weak));
extern "C" size_t vgx_host_memory_describe(char* out, size_t cap) __attribute__((weak));   // 1.2: the renderer's frame buffers (vgx_arena.h)

// Multiplayer (platform/sdk/net/np_lan.cpp, wave 5 NET): a LAN match's input delay, starved steps, steps per second and round trip, for the frame line.
extern "C" int NpLanNetBrief(char* out, int size) __attribute__((weak));
bool NpLanActive() __attribute__((weak));   // platform/sdk/net/np_lan.cpp (T8: the core 3 guard)
bool NpOnlineFlowActive() __attribute__((weak));   // platform/sdk/dwc/np_online_flow.cpp
bool NpLanEntryActive() __attribute__((weak));   // platform/sdk/net/np_lan.cpp (mp18: the multiplayer entry before a session: room, STUN, TURN, the wait)

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

// One block of lines: the build, the clocks the system reports, the frame-rate policy, the picture size range, the picture path and the queue.
void LogRunHeader(const char* when) {
    char fps[320] = "fps=?", dyn[360] = "dynres=?", host[300] = "picture=?", gfx[320] = "gfx: ?", set[260] = "settings: ?", inp[300] = "controls: ?";
    if (NpFpsDescribe) NpFpsDescribe(fps, sizeof fps);
    if (VgxDynResDescribe) VgxDynResDescribe(dyn, sizeof dyn);
    if (vgx_vita_describe) vgx_vita_describe(host, sizeof host);
    if (VgxGfxDescribe) VgxGfxDescribe(gfx, sizeof gfx);
    if (NpSettingsDescribe) NpSettingsDescribe(set, sizeof set);
    if (NpInputDescribe) NpInputDescribe(inp, sizeof inp);
#if defined(__vita__)
    char clocks[96];
    std::snprintf(clocks, sizeof clocks, "ARM %d MHz, GPU %d MHz, bus %d MHz, GPU xbar %d MHz", scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(),
                  scePowerGetBusClockFrequency(), scePowerGetGpuXbarClockFrequency());
#else
    const char clocks[] = "host";
#endif
    std::fprintf(stderr, "[run] %s: build %s (%s); clocks %s\n[run] %s\n[run] %s\n[run] %s\n[run] %s\n[run] %s\n[run] %s\n", when, NP_BUILD_VERSION, NP_VITA_VARIANT, clocks, fps, dyn, host, gfx, set, inp);
}

void OnFramePresented() {
    const uint64_t frames = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    VitaBootScreen::Frame(frames);
    if ((frames & 7u) == 0 && NpLanActive && NpOnlineFlowActive)   // (T8) the fourth core's online guard: nothing happens unless the core rules are on; (mp18) the entry counts too
        VitaThreads::Core3OnlineStep(NpVitaCore3::GuardOnline(NpLanActive(), NpOnlineFlowActive(), NpLanEntryActive && NpLanEntryActive()));
    if (frames == 1) LogRunHeader("first frame");   // (also in the quiet player build: its log has no other line of what the run was set to)
    // 1.2 (MSC_VITA_CORE0_ALONE, on by default in the player build): threads that may run on core 0 with the game; library threads can appear late, so it looks again a few times
    // 1.3 MSC_VITA_SWEEP_BG=1: the sweep every 600 frames goes to the slow watch thread (np_vita_wire.cpp), the first three stay here. Logging builds time a few sweeps, to judge it.
    if (frames == 5 || frames == 30 || frames == 120 || (frames % 600 == 0 && !VitaThreads::SweepInBackground())) {
        const uint64_t t0 = NP_LOGGING && frames <= 1800 ? NowUs() : 0;
        VitaThreads::KeepCoreZeroForGame(frames == 5);
        if (t0) std::fprintf(stderr, "[vita] thread sweep at frame %llu took %u us on the game thread\n", static_cast<unsigned long long>(frames), static_cast<unsigned>(NowUs() - t0));
    }
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
        {   // crash26b: the log path's own line (a stall shows as its absence; the counts say whether the format lock ever waited or gave up)
            char when[48];
            std::snprintf(when, sizeof when, "alive at frame %llu", static_cast<unsigned long long>(frames));
            VitaLog::StdioReport(when);
        }
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
        if (frames % 6000 == 0) LogRunHeader(when);   // (again every 6000 frames, so a cut piece of a long log still says what it ran with)
        VitaAudio::LogReport(when);
        NpJobsLogStats(when);
        if (frames == 600 || frames == 7200) VitaThreads::LogThreadTable(when);   // (1.2) priorities and cores as the system has them
        if (VitaThreads::ThreadCpuOn()) VitaThreads::LogThreadCpu(when);           // (1.3) MSC_VITA_THREAD_CPU: CPU ms by core mask since the last line
        if (VitaMem::Checkpoint) VitaMem::Checkpoint(when);
    }
    lastUs = now;
    lastCpuUs = cpu;
    lastFrames = frames;
}

// The memory line: figures only, built on the caller's stack (it runs from the 30-second timer of the stall-watch thread and from a failing allocation).
size_t MemoryLine(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    const struct mallinfo heap = mallinfo();
    char vgx[160] = "";
    if (vgx_host_memory_describe) vgx_host_memory_describe(vgx, sizeof(vgx));
    int n;
#if defined(__vita__)
    SceKernelFreeMemorySizeInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    const bool haveFree = sceKernelGetFreeMemorySize(&info) >= 0;
    n = std::snprintf(out, cap, "C heap in use %u KiB (extent %u KiB of %u, free %u KiB in %u pieces, releasable at the top %u KiB); system free main %d KiB, cdram %d KiB; %s",
                      static_cast<unsigned>(heap.uordblks / 1024), static_cast<unsigned>(heap.arena / 1024), _newlib_heap_size_user / 1024u,
                      static_cast<unsigned>(heap.fordblks / 1024), static_cast<unsigned>(heap.ordblks), static_cast<unsigned>(heap.keepcost / 1024),
                      haveFree ? static_cast<int>(info.size_user / 1024) : -1, haveFree ? static_cast<int>(info.size_cdram / 1024) : -1, vgx);
#else
    n = std::snprintf(out, cap, "C heap in use %u KiB (extent %u KiB, free %u KiB in %u pieces); %s", static_cast<unsigned>(heap.uordblks / 1024),
                      static_cast<unsigned>(heap.arena / 1024), static_cast<unsigned>(heap.fordblks / 1024), static_cast<unsigned>(heap.ordblks), vgx);
#endif
    return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

void ShowFatal(const char* category, const char* details) noexcept {
    VitaLab::EndReason("fatal error: %s", category ? category : "");
    if (!NP_LOGGING) {
        // 1.2: the quiet build keeps the reason too (logging builds write the lines below to log.txt). The details' line breaks become " | ".
        char text[560];
        size_t n = 0;
        const char* c = details ? details : "";
        n = static_cast<size_t>(std::snprintf(text, sizeof(text), "%s: ", category ? category : ""));
        if (n >= sizeof(text)) n = sizeof(text) - 1;
        for (; *c && n + 4 < sizeof(text); ++c) {
            if (*c == '\n') { text[n++] = ' '; text[n++] = '|'; text[n++] = ' '; }
            else if (*c != '\r') text[n++] = *c;
        }
        text[n] = '\0';
        VitaLog::Event(text);
    }
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
#if defined(__vita__) && defined(NP_PLAYER)
    NpSettingsScreen::Pump();
#endif
    VitaPlatform::OnFramePresented();
    VitaLab::OnFramePresented();   // lab builds: the deliberate-failure switches
}

extern "C" void NpPlatformFatal(const char* category, const char* details) { VitaPlatform::ShowFatal(category, details); }

// 1.2: a request the host heap could not meet (the game's operator new on a thread other than the game thread, nlMemory.cpp): the program ends after
// this, and without it the player's log said nothing about memory (the caller wrote through a null pointer). One FATAL line with the heap's state,
// written to the card before the abort.
extern "C" void NpPlatformHostAllocFailed(size_t size) {
    char figures[400];
    VitaPlatform::MemoryLine(figures, sizeof(figures));
    char text[560];
    std::snprintf(text, sizeof(text), "FATAL: host heap exhausted: a request for %lu KiB found no room; %s", static_cast<unsigned long>(size / 1024u), figures);
    VitaLog::Event(text);
}

// 1.3 MSC_VGX_WARM_HEAP_MIB (vgx_backend_gl.cpp): the C heap's use now.
extern "C" unsigned NpPlatformHeapUsedKiB(void) {
#if defined(__vita__)
    const struct mallinfo heap = mallinfo();
    return static_cast<unsigned>(heap.uordblks / 1024u);
#else
    return 0;
#endif
}

// 1.3 wave 10 MSC_VGX_WARM_FLOOR_MIB (vgx_backend_gl.cpp): the C heap's size (the free heap the warm-up leaves is this minus NpPlatformHeapUsedKiB). The host build has no definition and the
// renderer's weak reference then sets no floor.
extern "C" unsigned NpPlatformHeapTotalKiB(void) {
#if defined(__vita__)
    return _newlib_heap_size_user / 1024u;
#else
    return 0;
#endif
}

// 1.2 (np/fatal.h): the sink of NpFatalNote, and the stack limit its backtrace scan stays under.
extern "C" void NpPlatformFatalLine(const char* line) { VitaLog::Event(line); }
// 1.2: the renderer's and the driver's record of a failure it survived (vgx_backend_gl.cpp QuietNote, vgx_host_vita.cpp msc_vgl_printf).
extern "C" void NpPlatformNote(const char* line) { VitaLog::Note(line); }
extern "C" int NpPlatformStackHigh(uintptr_t* high) {
#if defined(__vita__)
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 || !info.stack || !info.stackSize) return 0;
    *high = reinterpret_cast<uintptr_t>(info.stack) + info.stackSize;
    return 1;
#else
    (void)high;
    return 0;
#endif
}

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
extern "C" int VgxQuiesceForExit(unsigned ms) __attribute__((weak));   // renderer/src/vgx_play.cpp
extern "C" void NpPlatformExit(int code) {
    if (VgxQuiesceForExit) VgxQuiesceForExit(200);   // (the back end finishes its picture before the process goes)
    VitaLab::EndReason("the game left through NpPlatformExit(%d)", code);
    VitaLog::FlushBlocking();   // (exit17: Flush() gives up when the writer thread is inside its own write, and the last lines are lost; this waits, bounded)
    sceKernelExitProcess(code);
    for (;;) sceKernelDelayThread(1000000);
}
#endif
