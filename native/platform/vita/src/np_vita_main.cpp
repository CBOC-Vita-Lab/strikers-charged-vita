// PS Vita entry point of the native port: process parameters (np_vita_process.cpp), system setup,
// first-run checks, then the game's own main (src/Game/main.cpp compiled with -Dmain=np_game_main).
// Ported from the recompiled build's vita_main.cpp (one/vita/src): no kubridge requirement, no
// guest memory layout, no runtime start-up; the log, the text console, env.txt, lab mode, the core
// plan and the clocks are unchanged.
#if defined(__vita__)

#include "np_env_cache.h"
#include "np_vita.h"
#include "np_vita_clock.h"
#include "np_vita_internal.h"
#include "np_vita_settings.h"
#include "np_settings.h"
#include "np_build_identity.h"
#include "np_startup_check.h"
#include "../../sdk/include/np/np_text.h"
#if defined(NP_PLAYER)
#include "np_settings_guard_vita.h"
#include "np_change_guard_vita.h"
#endif
#include "vita_lab.h"
#include "../../sdk/save/np_save_import.h"
#include "../../sdk/net/np_net_census.h"
#include "np_picture_presets.h"
#include "np_freeze_button.h"
#include "../../../include/np/stall.h"

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/system_param.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>

extern "C" __attribute__((weak)) int NpVitaGameDataOptional();  // smoke application: returns 1
extern "C" int np_vita_radio_system_init(void) __attribute__((weak));   // platform/sdk/net/vlink_vita_radio.cpp (W5 BOOT): AppUtil + CommonDialog set-up
int np_game_main();                                // the decomp's `int main()`, renamed by -Dmain=np_game_main (NPB's Makefile)

// MSC_VITA_FPSCR=0|1|2 (default 0: nothing changes, the value is only logged): the floating-point control register of the game's main thread.
//   1  flush-to-zero (FPSCR bit 24: a denormal operand or result is read / written as a zero), the "RunFast" half;  2  1 + default NaN (bit 25).
// The log line says what the thread started with: if FZ is already set there is nothing to gain; if it is clear, a denormal in the animation blend or the
// physics (a quaternion weight decaying, a velocity damped towards 0) is a slower operation than a normal one on the VFP, and 1 removes that. The values of
// normal numbers do not change; a denormal becomes 0. The frame-send worker takes the game thread's control bits at every job (np_framesend.cpp), so it
// follows. Both consoles of a match run the same binary and the same setting; a replay or determinism trace recorded under another setting can differ
// only where a denormal was produced, which the host cannot model (x86 SSE keeps denormals): hence default 0 until a console run shows a gain.
static void ApplyFpscr()
{
#if defined(__arm__) && !defined(__SOFTFP__)
    uint32_t before = 0;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(before));
    const char* e = std::getenv("MSC_VITA_FPSCR");
    const int mode = (e && *e) ? static_cast<int>(std::strtol(e, nullptr, 0)) : 0;
    uint32_t after = before;
    if (mode >= 1) after |= (1u << 24);
    if (mode >= 2) after |= (1u << 25);
    if (after != before) __asm__ volatile("vmsr fpscr, %0" : : "r"(after));
    uint32_t now = 0;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(now));
    std::fprintf(stderr, "[fpscr] main thread FPSCR was 0x%08X (FZ=%u DN=%u RMode=%u), MSC_VITA_FPSCR=%d, now 0x%08X (FZ=%u DN=%u)\n", before,
                 (before >> 24) & 1u, (before >> 25) & 1u, (before >> 22) & 3u, mode, now, (now >> 24) & 1u, (now >> 25) & 1u);
#endif
}
extern "C" int np_tls_fast_init(void);   // np_fasttls.cpp (MSC_TLS_FAST)
extern "C" int VgxQuiesceForExit(unsigned ms) __attribute__((weak));   // renderer/src/vgx_play.cpp
extern "C" unsigned int _newlib_heap_size_user;    // np_vita_process.cpp
extern "C" char __executable_start;                // start of the image (linker script)

// 1.3 (SC06): the language of the Vita (System Settings > Language) for the plan's default: "es", "fr", "en" or null. sceAppUtilInit is the first call of the radio set-up too
// (vlink_vita_radio.cpp takes SCE_APPUTIL_ERROR_BUSY as already done), so asking here, on the main thread at start-up, costs it nothing.
extern "C" const char* NpVitaSystemLanguageCode(void) {
    SceAppUtilInitParam init{};
    SceAppUtilBootParam boot{};
    sceAppUtilInit(&init, &boot);
    int lang = -1;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang) < 0) return nullptr;
    if (lang == SCE_SYSTEM_PARAM_LANG_SPANISH) return "es";
    if (lang == SCE_SYSTEM_PARAM_LANG_FRENCH) return "fr";
    return "en";
}

namespace {

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

[[noreturn]] void Die(const char* category, const char* details) {
    VitaLab::EndReason("fatal error at start-up: %s", category);
    VitaPlatform::ShowFatal(category, details);
    VitaLog::Flush();
    sceKernelExitProcess(1);
    for (;;) sceKernelDelayThread(1000000);
}

// First-run files (app0:/seed/...). The SDK layer could copy them itself with
// std::filesystem::copy_file, but libstdc++ finishes that with chmod(to,
// st_mode of the source), and VitaSDK's chmod passes newlib's mode bits
// straight to sceIoChstat, whose result on a file copied from the read-only
// app0: mount nobody has tested; a failure there is fatal ("Unable to
// initialize managed NAND"). Copying the tree here with plain sceIo calls
// means the runtime finds every file present and never takes that path.
//
// (From Codex.) The file is written under a temporary name and renamed when it is complete: a copy cut
// short by a crash or a power-off (the shader cache seed and the NAND seed are many files) leaves no
// half-file that Exists(to) would take for a finished one at the next start. A leftover temporary is
// simply overwritten.
bool CopyFileIfMissing(const char* from, const char* to) {
    if (Exists(to)) return true;
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    char temporary[512];
    const int length = std::snprintf(temporary, sizeof(temporary), "%s.seed-tmp", to);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(temporary)) {
        sceIoClose(in);
        return false;
    }
    const SceUID out = sceIoOpen(temporary, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) {
        sceIoClose(in);
        return false;
    }
    static char buffer[16384];
    bool ok = true;
    for (;;) {
        const int n = sceIoRead(in, buffer, sizeof(buffer));
        if (n < 0) ok = false;
        if (n <= 0) break;
        if (sceIoWrite(out, buffer, static_cast<SceSize>(n)) != n) {
            ok = false;
            break;
        }
    }
    sceIoClose(in);
    if (sceIoClose(out) < 0) ok = false;
    if (ok && sceIoRename(temporary, to) < 0) ok = false;
    if (!ok) sceIoRemove(temporary);
    return ok;
}

bool SeedTree(const char* fromDir, const char* toDir, int depth = 0) {
    if (sceIoMkdir(toDir, 0777) < 0 && !Exists(toDir)) return false;
    const SceUID dir = sceIoDopen(fromDir);
    if (dir < 0) return false;
    if (depth > 8) {
        sceIoDclose(dir);
        return false;
    }
    bool ok = true;
    SceIoDirent entry;
    for (;;) {
        std::memset(&entry, 0, sizeof(entry));
        const int read = sceIoDread(dir, &entry);
        if (read < 0) ok = false;          // a failed directory read is a failed seed, not the end of the list
        if (read <= 0) break;
        if (std::strcmp(entry.d_name, ".") == 0 || std::strcmp(entry.d_name, "..") == 0) continue;
        char from[512], to[512];
        const int fromLength = std::snprintf(from, sizeof(from), "%s/%s", fromDir, entry.d_name);
        const int toLength = std::snprintf(to, sizeof(to), "%s/%s", toDir, entry.d_name);
        if (fromLength < 0 || toLength < 0 || static_cast<size_t>(fromLength) >= sizeof(from) ||
            static_cast<size_t>(toLength) >= sizeof(to)) {
            ok = false;                    // a name that does not fit is not copied to a truncated name
            continue;
        }
        if (SCE_S_ISDIR(entry.d_stat.st_mode)) ok = SeedTree(from, to, depth + 1) && ok;
        else ok = CopyFileIfMissing(from, to) && ok;
    }
    sceIoDclose(dir);
    return ok;
}

// The native game's own data directory holds no save the first time it runs, while the recompiled build (same game data folder) usually has one. The player build
// copies that one (the original is never touched; platform/sdk/save/np_save_import.h) so that a player's cups and unlocks are there from the first start.
// NP_IMPORT_SAVE=0 turns it off (the player build), =1 turns it on (the lab build, which leaves it off so that test runs do not depend on the owner's save).
void ImportRecompiledSave() {
    const char* want = std::getenv("NP_IMPORT_SAVE");
#if defined(NP_PLAYER)
    const bool on = !want || std::strcmp(want, "0") != 0;
#else
    const bool on = want && std::strcmp(want, "1") == 0;
#endif
    if (!on) return;
    const char* dest = std::getenv("NP_DATA_DIR");
    char message[640];
    NpSaveImportRecompiled("ux0:data/mscharged", dest && *dest ? dest : "ux0:data/mscharged", message, sizeof(message));
    std::fprintf(stderr, "[vita] %s\n", message);
}

void LogSystemState() {
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0) {
        std::fprintf(stderr, "[vita] main thread: stack %u KiB at %p, priority %d, affinity 0x%X\n",
                     static_cast<unsigned>(info.stackSize / 1024), info.stack, info.currentPriority,
                     static_cast<unsigned>(info.currentCpuAffinityMask));
        if (static_cast<unsigned>(info.stackSize) < 2u * 1024u * 1024u)
            std::fprintf(stderr, "[vita] WARNING: the main stack is smaller than requested (sceUserMainThreadStackSize ignored?)\n");
    }
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    if (sceKernelGetFreeMemorySize(&free) >= 0)
        std::fprintf(stderr, "[vita] free memory now: main %d KiB, cdram %d KiB, phycont %d KiB (the %u KiB C heap is already taken)\n",
                     free.size_user / 1024, free.size_cdram / 1024, free.size_phycont / 1024, _newlib_heap_size_user / 1024);
    void* probe = std::malloc(16);
    int onStack = 0;
    std::fprintf(stderr, "[vita] addresses: code %p, static data %p, C heap %p, main stack %p\n",
                 static_cast<void*>(&__executable_start), static_cast<void*>(&_newlib_heap_size_user), probe, static_cast<void*>(&onStack));
    std::free(probe);
}

// Applies and reports the clock and affinity settings (each call's result is logged: an emulator
// or a firmware that refuses one should be visible). MSC_VITA_CPU_MHZ (env.txt) asks for a lower
// clock for an A/B run (111, 166, 222, 333, 444); 500 MHz is not an application's to set: it comes
// from the owner's PSVshellPlus profile (np/docs/vita.md, "444 or 500 MHz"). 1.3: the core is measured
// first, and a clock a plugin already holds at about 500 is left alone (np_vita_clock.h, MSC_VITA_CLOCK_POLICY).
// The keep thread's ARM request (clockfix17, F3): a kept 500 goes through RestoreArm (500, then 444 if refused); a kept 444 is asked as it is. Returns the rc of the last request.
int KeepArmRequest(int mhz, const NpVitaClock::Result& r) {
    struct Arm { int SetArm(int v) { return scePowerSetArmClockFrequency(v); } } arm;
    if (mhz != NpVitaClock::kMaxMHz) return arm.SetArm(mhz);
    const NpVitaClock::Restore back = NpVitaClock::RestoreArm(arm, r);
    return back.second ? back.secondRc : back.firstRc;
}

// The clock requests and their watch threads (SetUpCpu, or 1 s later on a thread of its own: MSC_VITA_CLOCK_DEFER).
void ApplyClocks(int affinity, long deferredMs) {
    struct PowerApi {
        int SetArm(int mhz) { return scePowerSetArmClockFrequency(mhz); }
        int SetBus(int mhz) { return scePowerSetBusClockFrequency(mhz); }
        int SetGpu(int mhz) { return scePowerSetGpuClockFrequency(mhz); }
        int SetXbar(int mhz) { return scePowerSetGpuXbarClockFrequency(mhz); }
        int GetArm() { return scePowerGetArmClockFrequency(); }
        int GetBus() { return scePowerGetBusClockFrequency(); }
        int GetGpu() { return scePowerGetGpuClockFrequency(); }
        int GetXbar() { return scePowerGetGpuXbarClockFrequency(); }
        int MeasureMHz() { return static_cast<int>(VitaProbe::MeasureMHz(3, 4) + 0.5); }   // (about 12 ms: on the game's core before any other thread of the game runs, or on the MSC_VITA_CLOCK_DEFER thread above the game's priority)
    } power;
    const NpVitaClock::Result clocks = NpVitaClock::Apply(power, std::getenv("MSC_VITA_CPU_MHZ"), std::getenv("MSC_VITA_CLOCK_POLICY") ? std::getenv("MSC_VITA_CLOCK_POLICY") : "max", std::getenv("MSC_VITA_GPU_MHZ"), std::getenv("MSC_VITA_BUS_MHZ"), std::getenv("MSC_VITA_XBAR_MHZ"), NpVitaClock::StockSimOn(std::getenv("MSC_VITA_STOCK_SIM")));
    char clockLine[400];
    NpVitaClock::Format(clocks, clockLine, sizeof(clockLine));
    VitaLog::Note(clockLine);   // (the basic record: quiet builds have this line too)
    // A plugin's clock is left alone only while it holds: a low-priority look every 2 s puts 500 back (444 if refused) if it lets go (at most 3 times).
    if (NpVitaClock::WatchUseful(clocks)) {
        static NpVitaClock::Result held;
        held = clocks;
        const SceUID watch = sceKernelCreateThread("np_clock_watch", [](SceSize, void*) -> int {
            for (int tries = 0; tries < 3;) {
                sceKernelDelayThread(2000000);
                if (NpVitaClock::NeedsRestore(held, scePowerGetArmClockFrequency())) {
                    ++tries;
                    // 500 first (the policy's own clock), 444 only if that is refused (np_vita_clock.h RestoreArm)
                    struct Arm { int SetArm(int mhz) { return scePowerSetArmClockFrequency(mhz); } } arm;
                    const NpVitaClock::Restore back = NpVitaClock::RestoreArm(arm, held);
                    char note[140];
                    if (back.second) std::snprintf(note, sizeof(note), "clocks: the plugin's clock went away; requested %d rc 0x%X, then %d rc 0x%X", back.first, static_cast<unsigned>(back.firstRc), back.second, static_cast<unsigned>(back.secondRc));
                    else std::snprintf(note, sizeof(note), "clocks: the plugin's clock went away; requested %d, rc 0x%X", back.first, static_cast<unsigned>(back.firstRc));
                    VitaLog::Note(note);
                }
            }
            return 0;
        }, 0x10000100 + 31, 16 * 1024, 0, 0, nullptr);   // (the lowest user priority, as the log writer. VitaLog::Note takes a 700-byte line, a path and printf: not a 4 KiB stack)
        if (watch < 0 || sceKernelStartThread(watch, 0, nullptr) < 0) VitaLog::Note("clocks: no thread to watch the plugin's clock");
    }
    // Logging builds: the measured clock every 30 s, a line when it moved by more than 10 MHz (Galaxy's clock watch: a PSVshellPlus profile can change while the game runs, and the
    // system's report does not follow it, it read 333 with the core at 498). MSC_VITA_CLOCK_WATCH=0: off.
    // clockfix17 (R1): the same thread, in quiet builds too, is the measured watch when nothing was asked of the ARM clock (a plugin holds it) and the getter reports under 400: two
    // readings under 430 in a row ask 500 (444 if refused), at most 3 times (np_vita_clock.h MeasuredLook). It is the lowest-priority thread of the process (191, below every game, render
    // and audio thread), wakes every 30 s and spins the existing 12 ms; MSC_VITA_CLOCK_MWATCH=0 turns the requests off. Logging only in logging builds.
    const bool logMeasure = NP_LOGGING && !(std::getenv("MSC_VITA_CLOCK_WATCH") && std::strcmp(std::getenv("MSC_VITA_CLOCK_WATCH"), "0") == 0);
    static NpVitaClock::Result mwatchResult;
    mwatchResult = clocks;
    const bool mwatch = NpVitaClock::MeasuredWatchOn(std::getenv("MSC_VITA_CLOCK_MWATCH")) && NpVitaClock::MeasuredWatchUseful(clocks);
    static bool s_logMeasure, s_mwatch;
    s_logMeasure = logMeasure;
    s_mwatch = mwatch;
    if (logMeasure || mwatch) {
        const SceUID measure = sceKernelCreateThread("np_clock_measure", [](SceSize, void*) -> int {
            int last = 0;
            NpVitaClock::MeasuredWatch watch;
            for (;;) {
                sceKernelDelayThread(NpVitaClock::kMeasureEveryS * 1000000u);
                const int mhz = static_cast<int>(VitaProbe::MeasureMHz(3, 4) + 0.5);
                if (s_logMeasure && (!last || mhz > last + 10 || mhz < last - 10)) {
                    char note[100];
                    std::snprintf(note, sizeof(note), "clocks: the ARM core measures %d MHz now (system reports %d)", mhz, scePowerGetArmClockFrequency());
                    VitaLog::Note(note);
                }
                last = mhz;
                if (s_mwatch && NpVitaClock::MeasuredLook(watch, mhz)) {
                    struct Arm { int SetArm(int v) { return scePowerSetArmClockFrequency(v); } } arm;
                    const NpVitaClock::Restore back = NpVitaClock::RestoreArm(arm, mwatchResult);
                    if (s_logMeasure) {
                        char note[140];
                        std::snprintf(note, sizeof(note), "clocks: measured %d MHz twice; requested %d rc 0x%X%s", mhz, back.first, static_cast<unsigned>(back.firstRc), back.second ? ", then 444" : "");
                        VitaLog::Note(note);
                    }
                    if (watch.restores >= NpVitaClock::kMeasureRestores && !s_logMeasure) break;
                }
            }
            return sceKernelExitDeleteThread(0);
        }, 0x10000100 + 31, 16 * 1024, 0, 0, nullptr);
        if (measure >= 0) sceKernelStartThread(measure, 0, nullptr);
    }
    // MSC_VITA_CLOCK_KEEP (default on): a clock the system took back after a suspend is asked for again (np_vita_clock.h KeepLook). Quiet builds too: it is not a diagnostic.
    if (NpVitaClock::KeepOn(std::getenv("MSC_VITA_CLOCK_KEEP"))) {
        static NpVitaClock::KeepClock kept[4];
        static NpVitaClock::Result keptResult;
        keptResult = clocks;
        kept[0] = NpVitaClock::KeepArm(clocks);
        kept[1] = NpVitaClock::KeepOther(clocks.gpuRequest, clocks.reportedGpu);
        kept[2] = NpVitaClock::KeepOther(clocks.busRequest, clocks.reportedBus);
        kept[3] = NpVitaClock::KeepOther(clocks.xbarRequest, clocks.reportedXbar);
        const SceUID keep = sceKernelCreateThread("np_clock_keep", [](SceSize, void*) -> int {
            static const char* const kName[4] = {"ARM", "GPU", "bus", "xbar"};
            for (;;) {
                sceKernelDelayThread(2000000);
                const int now[4] = {scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuXbarClockFrequency()};
                for (int i = 0; i < 4; ++i) {
                    bool fell = false, back = false;
                    const bool ask = NpVitaClock::KeepLook(kept[i], now[i], &fell, &back);
                    int rc = 0;
                    if (ask) rc = i == 0 ? KeepArmRequest(kept[0].request, keptResult) : i == 1 ? scePowerSetGpuClockFrequency(kept[i].request) : i == 2 ? scePowerSetBusClockFrequency(kept[i].request) : scePowerSetGpuXbarClockFrequency(kept[i].request);
                    if (fell || back) {
                        char note[120];
                        if (fell) std::snprintf(note, sizeof(note), "clocks: %s reports %d, was %d at the start; requested %d, rc 0x%X", kName[i], now[i], kept[i].baseline, kept[i].request, static_cast<unsigned>(rc));
                        else std::snprintf(note, sizeof(note), "clocks: %s is back at %d", kName[i], now[i]);
                        VitaLog::Note(note);
                    }
                }
            }
            return 0;
        }, 0x10000100 + 31, 16 * 1024, 0, 0, nullptr);   // (the lowest user priority, as the log writer; VitaLog::Note needs the stack)
        if (keep < 0 || sceKernelStartThread(keep, 0, nullptr) < 0) VitaLog::Note("clocks: no thread to keep the clocks");
    }
    if (deferredMs > 0) {
        char note[120];
        std::snprintf(note, sizeof(note), "clocks: requested %ld ms into the process (MSC_VITA_CLOCK_DEFER; process time %llu ms)", deferredMs,
                      static_cast<unsigned long long>(sceKernelGetProcessTimeWide() / 1000u));
        VitaLog::Note(note);
    }
    if (!NP_LOGGING) return; // Keep clock/affinity settings; omit diagnostic probe threads.
    std::fprintf(stderr, "[vita] %s\n", clockLine);
    std::fprintf(stderr, "[vita] clocks: CPU %d MHz, bus %d MHz, GPU %d MHz, GPU xbar %d MHz (set calls returned 0x%X 0x%X 0x%X 0x%X; affinity 0x%X)\n",
                 clocks.reportedArm, clocks.reportedBus, clocks.reportedGpu, clocks.reportedXbar, static_cast<unsigned>(clocks.fallbackTried ? clocks.fallbackRc : clocks.armRc),
                 static_cast<unsigned>(clocks.busRc), static_cast<unsigned>(clocks.gpuRc), static_cast<unsigned>(clocks.xbarRc), static_cast<unsigned>(affinity));
}

void SetUpCpu() {
    // Which thread runs where: vita_threads.cpp (the plan, and MSC_VITA_CORE_* in env.txt).
    // The game thread is this one, core 0 by default.
    // MSC_VITA_STOCK_SIM=1 (test only): a stock console has no fourth core; the plan reads these two right below.
    // (clockfix17: also forced in ApplyPlan, before the plan places the mixer and the file workers; vita_plan.cpp PlaceAudioAndFiles returns early under the sim.)
    if (NpVitaClock::ForceFourthCoreOff(std::getenv("MSC_VITA_STOCK_SIM"), [](const char* k, const char* v) { setenv(k, v, 1); })) {
        std::fprintf(stderr, "[vita] MSC_VITA_STOCK_SIM=1: %s, the fourth core off\n", NpVitaClock::ExplicitMHz(std::getenv("MSC_VITA_CPU_MHZ")) ? "MSC_VITA_CPU_MHZ decides the clock" : "exactly 444 MHz");
    }
    const int affinity = VitaThreads::PinCurrentThread(VitaThreads::kGame, false);
    if (NP_LOGGING) VitaThreads::LogPlan();
    // MSC_VITA_CLOCK_DEFER (np_vita_clock.h): 1 s into the process by default, from a thread above the game's priority (the measurement is best of three 4 ms windows).
    // The Vita build's policy default is "max", as below in ApplyClocks.
    const long deferMs = NpVitaClock::DeferMs(std::getenv("MSC_VITA_CLOCK_DEFER"), std::getenv("MSC_VITA_CPU_MHZ"),
                                              std::getenv("MSC_VITA_CLOCK_POLICY") ? std::getenv("MSC_VITA_CLOCK_POLICY") : "max");
    static int s_affinity;
    static long s_deferMs;
    s_affinity = affinity;
    s_deferMs = deferMs;
    SceUID deferThread = -1;
    if (deferMs > 0) {
        deferThread = sceKernelCreateThread("np_clock_defer", [](SceSize, void*) -> int {
            const unsigned long long waitUs = NpVitaClock::DeferWaitUs(sceKernelGetProcessTimeWide(), s_deferMs);
            if (waitUs) sceKernelDelayThread(static_cast<SceUInt>(waitUs));
            ApplyClocks(s_affinity, s_deferMs);
            return sceKernelExitDeleteThread(0);
        }, 0x10000100 - 32, 32 * 1024, 0, 0, nullptr);   // (above the game's 160; Format, the 400-byte line and VitaLog::Note's 700-byte line need the stack)
        if (deferThread >= 0 && sceKernelStartThread(deferThread, 0, nullptr) < 0) { sceKernelDeleteThread(deferThread); deferThread = -1; }
        if (deferThread < 0) VitaLog::Note("clocks: no thread for MSC_VITA_CLOCK_DEFER: requested at the start");
    }
    if (deferThread < 0) ApplyClocks(affinity, 0);
    if (!NP_LOGGING) return;
    // PIPE (wave 5): the real clock. A PSVshell profile (500 MHz, applied by its kernel part) is invisible to the power API.
    // W5 BOOT: the measurement (5 x 20 ms + 3 x 2M iterations, about 115 ms) runs on core 1, which has nothing to do this early, instead of
    // holding up the start; its "[clock] boot:" line comes a little later in the log. MSC_BOOT_CLOCK=main measures here as before.
    {
        const char* where = std::getenv("MSC_BOOT_CLOCK");
        SceUID clockThread = -1;
        if (!(where && std::strcmp(where, "main") == 0))
            clockThread = sceKernelCreateThread("np_clock_boot", [](SceSize, void*) -> int { VitaProbe::LogClock("boot"); return sceKernelExitDeleteThread(0); },
                                                0x10000100, 16 * 1024, 0, 0x20000, nullptr);   // core 1
        if (clockThread < 0 || sceKernelStartThread(clockThread, 0, nullptr) < 0) VitaProbe::LogClock("boot");
    }
    VitaProbe::StartClockMonitor();
    VitaProbe::RunCore4Probe();   // MSC_VITA_PROBE_CORE4=1|only
}

// ux0:data/mscharged/env.txt: one NAME=VALUE per line ('#' starts a comment),
// put into the process environment before anything reads it. The runtime and
// the renderer take their diagnostic switches from environment variables
// (MSC_*, MSC_VGX_*), and a Vita application has no environment of its own;
// this file is how a tester sets them on the device.
// The names the file set, so that a second reading (lab builds: a client uploads a new
// env.txt before it starts a run) first takes back what the first one did.
// Keep 96 file records, up to two picture and five core-plan defaults, and the conditional GPU timer for a later reread.
char g_envNames[104][48];
constexpr int kEnvNameCapacity = sizeof(g_envNames) / sizeof(g_envNames[0]);
constexpr int kEnvFileRecords = 96;
int g_envNameCount = 0;

// The player build (NP_PLAYER=1, platform/vita/vita.mk) is an ordinary application: it must not pick up the lab runs' leftovers, which live in the
// same folder (a test's env.txt with MSC_MAX_FRAMES or a nav script would end or steer a game that the maintainer starts from the home screen). Its
// settings file, when there is one, is np-player.txt (same NAME=VALUE format).
#if defined(NP_PLAYER)
#define NP_ENV_FILE "ux0:/data/mscharged/np-player.txt"
#else
#define NP_ENV_FILE "ux0:/data/mscharged/env.txt"
#endif
// The native game's save data does not go where the recompiled build keeps its own: the SDK layer's default data directory ux0:data/mscharged
// holds <dir>/NAND, which IS the recompiled build's NAND/ (NPS2 made the formats the same on purpose), and the native game has never read or written
// a populated save of the owner's. The default is set before the file is read (the file may still override it, one line, to share the save) and
// again after a re-read took the file's variables back.
void DefaultVariables() {
    setenv("NP_DATA_DIR", "ux0:data/mscharged/np-data", 0);
}

// MSC_PICTURE=performance|default|quality (np_picture_presets.h): the preset's switches, for each one the file did not set.
// Runs after the file is read; what it sets is recorded like the file's own variables, so a re-read takes it back.
void ApplyPicturePreset() {
    const char* preset = std::getenv("MSC_PICTURE");
    if (!preset || !*preset) return;
    const int n = np_picture::Expand(
        preset, [](const char* name) { return std::getenv(name) != nullptr; },
        [](const char* name, const char* value) {
            setenv(name, value, 0);
            if (g_envNameCount < kEnvNameCapacity && std::strlen(name) < sizeof(g_envNames[0])) std::strcpy(g_envNames[g_envNameCount++], name);
            std::fprintf(stderr, "[vita] MSC_PICTURE preset sets %s=%s\n", name, value);
        });
    if (n < 0) std::fprintf(stderr, "[vita] MSC_PICTURE=%s is not a preset (performance, default, quality): ignored\n", preset);
    else std::fprintf(stderr, "[vita] MSC_PICTURE=%s: %d switch(es) set\n", preset, n);
}

void LoadEnvFile() {
    NpEnvCache::Unsettle();   // (nxboot25 R2-12) the environment is written again: nothing kept from the last plan stays valid
    for (int i = 0; i < g_envNameCount; ++i) unsetenv(g_envNames[i]);
    g_envNameCount = 0;
    VitaPlan::ForgetDefaults();   // (1.2: the defaults of the last start-up; ApplyPlan gives them again after this reading)
    DefaultVariables();
#if defined(NP_PLAYER)
    const SceUID fd = sceIoOpen(NpSettingsScreen::SavedFilePath(), SCE_O_RDONLY, 0);
#else
    const SceUID fd = sceIoOpen(NP_ENV_FILE, SCE_O_RDONLY, 0);
#endif
    if (fd < 0) return;
    static char text[16384];   // (the Settings screen appends its lines at the end of np-player.txt: the whole file must be read)
    int got = sceIoRead(fd, text, sizeof(text) - 1);
    // 1.2: a file longer than the buffer used to be cut without a word (and a value cut in the middle of its line applied as it stood). Now the player's basic
    // record says so, and the line that was cut is not applied. (The Galaxy port found its env.txt lines cut at 512 bytes the same way.)
    char probe;
    const bool cut = got == static_cast<int>(sizeof(text) - 1) && sceIoRead(fd, &probe, 1) > 0;
    sceIoClose(fd);
    if (got <= 0) return;
    text[got] = '\0';
    if (cut) {
        char* lastEnd = std::strrchr(text, '\n');
        got = lastEnd ? static_cast<int>(lastEnd - text) : 0;
        text[got] = '\0';
        char note[120];
        std::snprintf(note, sizeof(note), "%s is longer than %u bytes: the lines after byte %d are ignored", NP_ENV_FILE + 20, static_cast<unsigned>(sizeof(text) - 1), got);
        VitaLog::Note(note);
    }
#if defined(NP_PLAYER)
    {   // set18: a value an earlier 1.3 Settings screen wrote into its own block and that is only worse for the player (np_settings.h WithoutObsolete) is not applied; the next save drops it
        std::vector<std::string> notes;
        const std::string kept = NpSettings::WithoutObsolete(std::string(text, static_cast<size_t>(got)), &notes);
        if (!notes.empty() && kept.size() < sizeof(text)) {
            std::memcpy(text, kept.data(), kept.size());
            got = static_cast<int>(kept.size());
            text[got] = '\0';
            for (const std::string& n : notes) std::fprintf(stderr, "[vita] %s\n", n.c_str());
            VitaLog::Note(notes[0].c_str());
        }
    }
#endif
    int count = 0;
    bool refusedNoted = false;
    for (char* line = text; line && *line;) {
        char* end = std::strchr(line, '\n');
        if (end) *end = '\0';
        while (*line == ' ' || *line == '\t') ++line;
        size_t length = std::strlen(line);
        while (length != 0 && (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t')) line[--length] = '\0';
        char* equals = std::strchr(line, '=');
        if (*line != '#' && equals && equals != line) {
            *equals = '\0';
            // (From Codex.) A name that cannot be recorded could not be unset when env.txt is read again:
            // it is refused, not set.
            if (count >= kEnvFileRecords || std::strlen(line) >= sizeof(g_envNames[0])) {
                std::fprintf(stderr, "[vita] env.txt: too many or too long a variable, ignored: %s\n", line);
                if (!refusedNoted) {   // (the quiet player build has no stderr: the basic record keeps the first, its notes are few)
                    refusedNoted = true;
                    char note[160];
                    std::snprintf(note, sizeof(note), "%s: too many variables or a name over %u characters, ignored: %.60s", NP_ENV_FILE + 20,
                                  static_cast<unsigned>(sizeof(g_envNames[0]) - 1), line);
                    VitaLog::Note(note);
                }
                line = end ? end + 1 : nullptr;
                continue;
            }
#if defined(NP_PLAYER)
            if (NpChangeGuardVita::Skips(line)) {   // startcheck19: the standard line is used this start (the file keeps the player's)
                std::fprintf(stderr, "[vita] %s: %s=%s not applied (two failed starts: the standard one is used)\n", NP_ENV_FILE + 20, line, equals + 1);
                line = end ? end + 1 : nullptr;
                continue;
            }
#endif
            setenv(line, equals + 1, 1);
            std::strcpy(g_envNames[g_envNameCount++], line);
            std::fprintf(stderr, "[vita] %s: %s=%s\n", NP_ENV_FILE + 20, line, equals + 1);
            ++count;
        }
        line = end ? end + 1 : nullptr;
    }
    std::fprintf(stderr, "[vita] %s: %d variable(s) set\n", NP_ENV_FILE + 20, count);
    ApplyPicturePreset();
}

// The player's core plan (vita_plan.cpp), after the file: what the file set wins.
void ApplyPlan() {
    // MSC_VITA_STOCK_SIM=1 (test only): no fourth core, and that before the plan places the mixer and the file workers (clockfix17, F4; the same three keys again in SetUpCpu).
    if (const char* sim = std::getenv("MSC_VITA_STOCK_SIM"); sim && std::strcmp(sim, "1") == 0) {
        setenv("MSC_VITA_CORE3", "0", 1);
        setenv("MSC_VITA_CORE3_SEND", "0", 1);
        setenv("MSC_VITA_CORE3_SHADERS", "0", 1);
    }
    const char* names[8];
    const int n = VitaPlan::Apply(names, 8);
    for (int i = 0; i < n && g_envNameCount < kEnvNameCapacity; ++i) std::strcpy(g_envNames[g_envNameCount++], names[i]);
    // This default depends on MSC_DYNRES, so a reread must remove it before the plan checks that switch again.
    const char* timer = "MSC_VITA_RT_TIMERS";
    bool recorded = false;
    for (int i = 0; i < g_envNameCount; ++i)
        if (std::strcmp(g_envNames[i], timer) == 0) recorded = true;
    if (!recorded && std::getenv(timer) && g_envNameCount < kEnvNameCapacity) std::strcpy(g_envNames[g_envNameCount++], timer);
    // (nxboot25 R2-12) The plan is applied and nothing writes the environment until the next LoadEnvFile: the fourth-core readers keep their answers (np_env_cache.h).
    NpEnvCache::Settle(std::getenv("MSC_ENV_CACHE"));
    std::fprintf(stderr, "[env] kept answers for the fourth-core readers: %s (MSC_ENV_CACHE=0 = every call reads; read the 300-picture [env] lines: lock takes per picture were 5.7-14.3, expect about 1)\n", NpEnvCache::Active() ? "on" : "off");
}

// Runs before every other C++ static constructor of the program: opens the log and the text
// console, so that a failure during static initialisation, or anywhere before main(), still leaves
// a last line saying how far start-up got.
struct EarlyBoot {
    EarlyBoot() {
        sceIoMkdir("ux0:/data", 0777);
        sceIoMkdir(VitaPlatform::kDataDevicePath, 0777);
        // (A lab build that is in lab mode starts in lab.txt and moves to log.txt when a run
        // begins: vita_lab.cpp.)
        VitaLog::Open(VitaPlatform::kDataDevicePath, VitaLab::EarlyLogName());
        VitaLog::SetSynchronous(true);
        const bool console = VitaConsole::Init();
        VitaLog::InstallStdioHooks();
        std::fprintf(stderr, "\n[vita] ===== Mario Strikers Charged, PS Vita native port, version %s, build %s (compiled %s %s) =====\n", NpBuildIdentity::Version(),
                     NpBuildIdentity::BuildId()[0] ? NpBuildIdentity::BuildId() : "unnamed", __DATE__, __TIME__);
        {   // 1.3 (SC10): the version and build id in the basic record too, which quiet builds write and the log header above does not reach
            char identity[160];
            std::snprintf(identity, sizeof(identity), "version %s, build %s", NpBuildIdentity::Version(), NpBuildIdentity::BuildId()[0] ? NpBuildIdentity::BuildId() : "unnamed");
            VitaLog::Note(identity);
        }
        if (!console) std::fprintf(stderr, "[vita] WARNING: no display framebuffer; continuing with the log file only\n");
        {   // 1.3 (review 12 Nov): the atomic save code (np_safe_file.h: write .tmp, fsync, rename) assumes fsync works on the memory card. Said once, at start-up.
            const char* probe = "ux0:data/mscharged/.fsync-probe";
            const int fd = ::open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                std::fprintf(stderr, "[vita] fsync check: could not open %s (errno %d): not checked\n", probe, errno);
            } else {
                const int w = static_cast<int>(::write(fd, "fsync", 5));
                errno = 0;
                const int r = ::fsync(fd);
                const int e = errno;
                ::close(fd);
                ::unlink(probe);
                std::fprintf(stderr, "[vita] fsync check on the memory card: fsync returned %d%s (errno %d, write %d): %s\n", r, r == 0 ? " (works)" : "", e, w,
                             r == 0 ? "saves and settings are flushed before they are renamed into place" : "WARNING: the safe writer's flush does not work here, a power cut can leave a renamed file without its data");
            }
        }
#if defined(NP_PLAYER)
        NpSettingsGuardVita::Run();   // 1.3: an older version's np-player.txt is stamped, backed up and its stale lines turned off before it is read
        NpChangeGuardVita::Begin();   // startcheck19: after two failed starts of a changed Picture or Language line, this start reads the standard ones (LoadEnvFile skips them)
#endif
        LoadEnvFile();
        ApplyPlan();
        VitaLog::ApplyStdioMode();   // crash26b: MSC_STDIO_LOCK (the plan sets 1: the log printf family bypasses the FILE); says which in a [stdio] line
        VitaBootScreen::Begin();   // the player build's loading screen, or the text console (MSC_BOOT_SCREEN)
        {   // 1.3: one line on the loading screen: the older settings file that was updated, else a language the disc lacks
            const char* language = VitaPlan::LanguageNotice();
            if (language) VitaLog::Note("language: the chosen language is not on the disc: English is used");
#if defined(NP_PLAYER)
            const char* guard = NpSettingsGuardVita::LoadingLine();
            const char* change = NpChangeGuardVita::LoadingLine();   // two failed starts after a Picture or Language change: the plainest news first
#else
            const char* guard = nullptr;
            const char* change = nullptr;
#endif
            VitaBootScreen::Notice(change ? change : language ? language : guard);
        }
        VitaPlatform::BootStage("C++ static initialisers");
    }
};
EarlyBoot g_earlyBoot __attribute__((init_priority(101)));

// ---- the freeze button (galaxyport18, np_freeze_button.h) ----------------------------------------------------------------------------
// L + R + Select held 3 s (after the first 20 s, never while the settings screen is open): a FATAL line with every thread's state, then a deliberate trap that the system turns into a
// core dump (np_vita_crash.cpp FreezeTrap). The thread is the lowest priority of the process, reads the pad itself (sceCtrlPeekBufferPositive: not VitaInput, so the game's pad reads and
// tap history are untouched) every 100 ms and keeps no game state. MSC_FREEZE_BUTTON=0 turns it off.
void FreezeReport(unsigned long long sinceStartMs) {
    char line[300];
    std::snprintf(line, sizeof(line), "FATAL: freeze button (L+R+Select held 3 s) at +%llu ms, frame %llu, task state 0x%x, task frame %u", sinceStartMs,
                  static_cast<unsigned long long>(VitaPlatform::FrameCount()), np::stall::State(), np::stall::Frame());
    VitaLog::EventUncapped(line);
    // every registered thread: status (SCE_KERNEL_THREADSTATUS_*: 1 running, 2 ready, 4 waiting, 8 suspended, 16 dormant...), what it waits on, priority, CPU time
    SceKernelThreadInfo info;
    int missesInRow = 0, seen = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        ++seen;
        std::snprintf(line, sizeof(line), "freeze thread %-24s status 0x%X wait type 0x%X id 0x%X priority %d cores 0x%X cpu %llu ms", info.name, static_cast<unsigned>(info.status),
                      static_cast<unsigned>(info.waitType), static_cast<unsigned>(info.waitId), static_cast<int>(info.currentPriority),
                      static_cast<unsigned>(info.currentCpuAffinityMask), static_cast<unsigned long long>(info.runClocks / 1000u));
        VitaLog::EventUncapped(line);
    }
    std::snprintf(line, sizeof(line), "freeze: %d threads listed (a logging build's log.txt has the last [hitch] / [late] lines just above this one)", seen);
    VitaLog::EventUncapped(line);
}

void StartFreezeButton() {
#if defined(NP_PLAYER)
    const bool playerBuild = true;
#else
    const bool playerBuild = false;
#endif
    if (!NpFreeze::Enabled(std::getenv("MSC_FREEZE_BUTTON"), playerBuild)) return;
    const SceUID thread = sceKernelCreateThread("msc_freezebutton", [](SceSize, void*) -> int {
        NpFreeze::Chord chord;
        for (;;) {
            sceKernelDelayThread(NpFreeze::kPollMs * 1000);
            SceCtrlData d;
            if (sceCtrlPeekBufferPositive(0, &d, 1) <= 0) { chord.Step(0, false, false); continue; }
            const unsigned all = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
            const unsigned long long ms = sceKernelGetProcessTimeWide() / 1000u;
            if (chord.Step(static_cast<uint32_t>(ms), (d.buttons & all) == all, NpSettingsScreen::IsOpen())) {
                FreezeReport(ms);
                VitaPlatform::FreezeTrap();
            }
        }
        return 0;
    }, 0x10000100 + 31, 16 * 1024, 0, 0, nullptr);   // (191, the lowest user priority, as the clock watch; 16 KiB because the log's Event takes a 700-byte line, a path and printf)
    if (thread < 0 || sceKernelStartThread(thread, 0, nullptr) < 0) VitaLog::Note("freeze button: no thread");
}

} // namespace

extern "C" const char* NpDataDevicePath(void) { return VitaPlatform::kDataDevicePath; }
extern "C" const char* NpGameRootPath(void) {
    static char path[160];
    if (!path[0]) {
        const char* over = std::getenv("MSC_GAME_ROOT");
        std::snprintf(path, sizeof(path), "%s", over && *over ? over : "ux0:/data/mscharged/game");
    }
    return path;
}

int main(int argc, char** argv) {
    if (!NP_LOGGING) {
        std::cout.setstate(std::ios_base::badbit);
        std::cerr.setstate(std::ios_base::badbit);
        std::clog.setstate(std::ios_base::badbit);
    }
    VitaPlatform::BootStage("platform start-up");
    VitaPlatform::InstallFatalHooks();
    std::atexit([] { VitaLog::Flush(); });
#if defined(NP_PLAYER)
    // SAFE START (L + R held at the start; set18: the old LiveArea tile's "-settings" no longer opens it): before anything else of the game starts.
    if (NpSettingsScreen::Requested(argc, argv)) {
        const NpSettingsScreen::Result shown = NpSettingsScreen::Run();
        if (shown == NpSettingsScreen::Result::kExit) {
            VitaPlan::CancelPendingStart();
            VitaPlan::LaunchClean("the settings screen's exit");   // stab13 MSC_LAUNCH_GUARD
            std::fprintf(stderr, "[vita] settings screen done: quitting\n");
            VitaLog::Flush();
            sceKernelExitProcess(0);
            for (;;) sceKernelDelayThread(1000000);
        }
        // "Save and start" and the system would not start the application again: read the file again and go on in this process.
        // (startcheck19: the changed lines are judged again, as a start of their own: Begin takes the first start's count back first)
        if (NpChangeGuardVita::Begin()) VitaBootScreen::Notice(NpChangeGuardVita::LoadingLine());
        LoadEnvFile();
        ApplyPlan();
        VitaLog::ApplyStdioMode();   // crash26b: MSC_STDIO_LOCK (the plan sets 1: the log printf family bypasses the FILE); says which in a [stdio] line
    }
#else
    (void)argc;
    (void)argv;
#endif
    // Lab builds: the command server, here and nowhere later, so that everything below runs
    // after RUN with the env.txt the client uploaded. Returns when the game is to boot.
    VitaLab::ServeBeforeRun([] {
        std::fprintf(stderr, "[vita] env.txt is read again\n");
        LoadEnvFile();
        ApplyPlan();
        VitaLog::ApplyStdioMode();   // crash26b: MSC_STDIO_LOCK (the plan sets 1: the log printf family bypasses the FILE); says which in a [stdio] line
    });
    SetUpCpu();
    if (NP_LOGGING) {
        LogSystemState();
        if (VitaMem::Checkpoint) VitaMem::Checkpoint("start of main");
        VitaPlatform::SelfTest();
    }

    // std::filesystem and relative paths need POSIX-looking paths; see np_vita_internal.h.
    VitaPlatform::BootStage("checking the data directory ux0:data/mscharged");
    if (chdir(VitaPlatform::kDataDevicePath) != 0)
        Die("the data directory is missing",
            "ux0:data/mscharged could not be created or entered.\n"
            "The memory card may be full or not writable. Free some space and start again.");

    // The smoke application has no game data and overrides this weak hook.
    const bool needData = !NpVitaGameDataOptional || !NpVitaGameDataOptional();
    if (needData) {
        NpStartupCheck::Message problem;
        if (!NpStartupCheck::CheckGameData(NpGameRootPath(), Exists, problem)) Die(problem.category, problem.details);
    }

    // First-run files shipped inside the package (app0:/seed/...), copied to ux0:data/mscharged
    // when missing: the SDK layer's NAND seed and anything else the game wants to find on disk.
    VitaPlatform::BootStage("first-run files");
    if (Exists("app0:/seed") && !SeedTree("app0:/seed", "ux0:/data/mscharged"))
    {
        std::fprintf(stderr, "[vita] WARNING: could not copy every first-run file from app0:/seed\n");
        VitaBootScreen::Notice(NpText::Tr(NpStartupCheck::SeedFailedNotice()));
    }

    VitaPlatform::BootStage("the native save");
    ImportRecompiledSave();

    VitaPlatform::BootStage("installing the crash handler");
    VitaPlatform::InstallCrashHandler();
    StartFreezeButton();

    // The system set-up of the multiplayer's ad hoc dialog, on this (the main) thread, before the renderer starts (W5 BOOT).
    if (np_vita_radio_system_init) np_vita_radio_system_init();

    VitaPlatform::BootStage("starting the helper-core job pool");
    NpJobsInit(0);

    VitaPlatform::BootStage("wiring the SDK layer and the renderer");
    VitaPlatform::WireSdkAndRenderer();

    np_tls_fast_init();   // MSC_TLS_FAST (1.3 cpuexact13): the emulated thread-local lookup without its call chain, after a self-check on this thread and a second one
    ApplyFpscr();   // MSC_VITA_FPSCR (1.3 armcpu9): what the main thread's floating-point control register holds, and (when asked) flush-to-zero

    VitaPlatform::BootStage("the game's start-up");
    const int result = np_game_main();
    std::fprintf(stderr, "[vita] the game's main returned %d\n", result);
    NpNetCensus::Log("game exit");   // MSC_NET_CENSUS=1: "[net] sockets open N ..." (np_net_census.h): a leak, or the plug-in's FTP wedge?
    if (VgxQuiesceForExit) VgxQuiesceForExit(200);   // (the back end finishes its picture before the process goes)
    VitaLab::EndReason("the game's main returned %d", result);
    if (result == 0) VitaPlan::LaunchClean("the game's main returned 0");   // stab13 MSC_LAUNCH_GUARD
    VitaLog::FlushBlocking();   // (exit17: not Flush(), which skips the write while the writer thread is inside its own)
    sceKernelExitProcess(result);
    return result;
}

#endif // __vita__
