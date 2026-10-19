// PS Vita entry point of the native port: process parameters (np_vita_process.cpp), system setup,
// first-run checks, then the game's own main (src/Game/main.cpp compiled with -Dmain=np_game_main).
// Ported from the recompiled build's vita_main.cpp (one/vita/src): no kubridge requirement, no
// guest memory layout, no runtime start-up; the log, the text console, env.txt, lab mode, the core
// plan and the clocks are unchanged.
#if defined(__vita__)

#include "np_vita.h"
#include "np_vita_internal.h"
#include "np_vita_settings.h"
#include "vita_lab.h"
#include "../../sdk/save/np_save_import.h"
#include "np_picture_presets.h"

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <unistd.h>

extern "C" __attribute__((weak)) int NpVitaGameDataOptional();  // smoke application: returns 1
extern "C" int np_vita_radio_system_init(void) __attribute__((weak));   // platform/sdk/net/vlink_vita_radio.cpp (W5 BOOT): AppUtil + CommonDialog set-up
int np_game_main();                                // the decomp's `int main()`, renamed by -Dmain=np_game_main (NPB's Makefile)
extern "C" unsigned int _newlib_heap_size_user;    // np_vita_process.cpp
extern "C" char __executable_start;                // start of the image (linker script)

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
// from the owner's PSVshellPlus profile (np/docs/vita.md, "444 or 500 MHz").
void SetUpCpu() {
    // Which thread runs where: vita_threads.cpp (the plan, and MSC_VITA_CORE_* in env.txt).
    // The game thread is this one, core 0 by default.
    const int affinity = VitaThreads::PinCurrentThread(VitaThreads::kGame, false);
    if (NP_LOGGING) VitaThreads::LogPlan();
    int want = 444;
    if (const char* t = std::getenv("MSC_VITA_CPU_MHZ")) {
        const int v = std::atoi(t);
        if (v == 111 || v == 166 || v == 222 || v == 333 || v == 444) want = v;
    }
    // The maximum an application may request without a kernel plugin.
    const int arm = scePowerSetArmClockFrequency(want);
    const int bus = scePowerSetBusClockFrequency(222);
    const int gpu = scePowerSetGpuClockFrequency(222);
    const int xbar = scePowerSetGpuXbarClockFrequency(166);
    if (!NP_LOGGING) return; // Keep clock/affinity settings; omit diagnostic probe threads.
    std::fprintf(stderr, "[vita] clocks: CPU %d MHz, bus %d MHz, GPU %d MHz, GPU xbar %d MHz (set calls returned 0x%X 0x%X 0x%X 0x%X; affinity 0x%X)\n",
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                 scePowerGetGpuXbarClockFrequency(), static_cast<unsigned>(arm), static_cast<unsigned>(bus),
                 static_cast<unsigned>(gpu), static_cast<unsigned>(xbar), static_cast<unsigned>(affinity));
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
char g_envNames[96][48];
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
            if (g_envNameCount < 96 && std::strlen(name) < sizeof(g_envNames[0])) std::strcpy(g_envNames[g_envNameCount++], name);
            std::fprintf(stderr, "[vita] MSC_PICTURE preset sets %s=%s\n", name, value);
        });
    if (n < 0) std::fprintf(stderr, "[vita] MSC_PICTURE=%s is not a preset (performance, default, quality): ignored\n", preset);
    else std::fprintf(stderr, "[vita] MSC_PICTURE=%s: %d switch(es) set\n", preset, n);
}

void LoadEnvFile() {
    for (int i = 0; i < g_envNameCount; ++i) unsetenv(g_envNames[i]);
    g_envNameCount = 0;
    DefaultVariables();
#if defined(NP_PLAYER)
    const SceUID fd = sceIoOpen(NpSettingsScreen::SavedFilePath(), SCE_O_RDONLY, 0);
#else
    const SceUID fd = sceIoOpen(NP_ENV_FILE, SCE_O_RDONLY, 0);
#endif
    if (fd < 0) return;
    static char text[16384];   // (the Settings screen appends its lines at the end of np-player.txt: the whole file must be read)
    const int got = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (got <= 0) return;
    text[got] = '\0';
    int count = 0;
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
            if (g_envNameCount >= 96 || std::strlen(line) >= sizeof(g_envNames[0])) {
                std::fprintf(stderr, "[vita] env.txt: too many or too long a variable, ignored: %s\n", line);
                line = end ? end + 1 : nullptr;
                continue;
            }
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
    const char* names[8];
    const int n = VitaPlan::Apply(names, 8);
    for (int i = 0; i < n && g_envNameCount < 96; ++i) std::strcpy(g_envNames[g_envNameCount++], names[i]);
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
        std::fprintf(stderr, "\n[vita] ===== Mario Strikers Charged, PS Vita native port, build %s %s =====\n", __DATE__, __TIME__);
        if (!console) std::fprintf(stderr, "[vita] WARNING: no display framebuffer; continuing with the log file only\n");
        LoadEnvFile();
        ApplyPlan();
        VitaBootScreen::Begin();   // the player build's loading screen, or the text console (MSC_BOOT_SCREEN)
        VitaPlatform::BootStage("C++ static initialisers");
    }
};
EarlyBoot g_earlyBoot __attribute__((init_priority(101)));

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
    std::atexit([] { VitaLog::Flush(); });
#if defined(NP_PLAYER)
    // The LiveArea's Settings button (or L + R at the start): the settings screen, before anything else of the game starts.
    if (NpSettingsScreen::Requested(argc, argv)) {
        const NpSettingsScreen::Result shown = NpSettingsScreen::Run();
        if (shown == NpSettingsScreen::Result::kExit) {
            std::fprintf(stderr, "[vita] settings screen done: quitting\n");
            VitaLog::Flush();
            sceKernelExitProcess(0);
            for (;;) sceKernelDelayThread(1000000);
        }
        // "Save and start" and the system would not start the application again: read the file again and go on in this process.
        LoadEnvFile();
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
        Die("the data directory is missing", "ux0:data/mscharged could not be created or entered.");

    char sysFst[200], filesDir[200];
    std::snprintf(sysFst, sizeof(sysFst), "%s/sys/fst.bin", NpGameRootPath());
    std::snprintf(filesDir, sizeof(filesDir), "%s/files", NpGameRootPath());
    // The smoke application has no game data and overrides this weak hook.
    const bool needData = !NpVitaGameDataOptional || !NpVitaGameDataOptional();
    if (needData && (!Exists(sysFst) || !Exists(filesDir)))
        Die("the game data is missing",
            "Copy the extracted disc of Mario Strikers Charged (USA, R4QE01) to\n"
            "    ux0:data/mscharged/game/sys/    (boot.bin, bi2.bin, fst.bin, main.dol, ...)\n"
            "    ux0:data/mscharged/game/files/  (the disc's files)\n"
            "The path can be changed with MSC_GAME_ROOT in ux0:data/mscharged/env.txt.");

    // First-run files shipped inside the package (app0:/seed/...), copied to ux0:data/mscharged
    // when missing: the SDK layer's NAND seed and anything else the game wants to find on disk.
    VitaPlatform::BootStage("first-run files");
    if (Exists("app0:/seed") && !SeedTree("app0:/seed", "ux0:/data/mscharged"))
        std::fprintf(stderr, "[vita] WARNING: could not copy every first-run file from app0:/seed\n");

    VitaPlatform::BootStage("the native save");
    ImportRecompiledSave();

    VitaPlatform::BootStage("installing the crash handler");
    VitaPlatform::InstallCrashHandler();

    // The system set-up of the multiplayer's ad hoc dialog, on this (the main) thread, before the renderer starts (W5 BOOT).
    if (np_vita_radio_system_init) np_vita_radio_system_init();

    VitaPlatform::BootStage("starting the helper-core job pool");
    NpJobsInit(0);

    VitaPlatform::BootStage("wiring the SDK layer and the renderer");
    VitaPlatform::WireSdkAndRenderer();

    VitaPlatform::BootStage("the game's start-up");
    const int result = np_game_main();
    std::fprintf(stderr, "[vita] the game's main returned %d\n", result);
    VitaLab::EndReason("the game's main returned %d", result);
    VitaLog::Flush();
    sceKernelExitProcess(result);
    return result;
}

#endif // __vita__
