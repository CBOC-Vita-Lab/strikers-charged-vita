// The runner includes production loader/restart/fallback blocks without the rest of the Vita executable.
#include "np_settings.h"
#include "np_picture_presets.h"
#include "np_vita_internal.h"
#include "np_env_cache.h"   // (nxboot25: the production ApplyPlan / LoadEnvFile blocks settle and unsettle the kept answers)
#include <psp2/io/fcntl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>

static std::string settingsFile, savedSettings, guardFile = "0\n";
static const char* guardPath;
static int guardWrites, restarts, failures;
static bool execSucceeds, exitSettings;
static int execResolution;
static std::mutex guardIoMutex, pauseMutex;
static std::condition_variable pauseChanged;
static std::string pauseValue;
static bool pauseWrite, writerPaused, releaseWriter;
#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); ++failures; } } while (0)

// (vita_plan.cpp asks whether this is a PS TV: never here; MSC_PSTV=1 is the host's way to say yes, tested in tests/test_pstv.cpp)
int sceKernelIsPSVitaTV(void) { return 0; }
namespace VitaThreads { bool Core3Active() { const char* v = std::getenv("MSC_VITA_CORE3"); return v && std::strcmp(v, "1") == 0; } }   // (a proven fourth core, for "stock-sim-core3")
unsigned long long sceKernelGetProcessTimeWide(void) { return 0; }
int sceIoOpen(const char* path, int flags, int) {
    if (std::strstr(path, "np-launch.txt")) return -1;   // stab13 launch guard: a card error, so the guard stays off in these tests (tests/test_launch_guard.cpp has its own)
    if (std::strstr(path, "np-plan.txt")) {
        std::lock_guard<std::mutex> lock(guardIoMutex);
        if (flags & SCE_O_TRUNC) guardFile.clear();
        return 2;
    }
    return 1;
}
int sceIoRead(int fd, void* dst, unsigned int size) {
    std::lock_guard<std::mutex> lock(guardIoMutex);
    const std::string& file = fd == 2 ? guardFile : settingsFile;
    const size_t n = file.size() < size ? file.size() : size;
    std::memcpy(dst, file.data(), n);
    return static_cast<int>(n);
}
int sceIoWrite(int fd, const void* src, unsigned int size) {
    if (fd != 2) std::abort();
    {
        std::unique_lock<std::mutex> lock(pauseMutex);
        if (pauseWrite && pauseValue == std::string(static_cast<const char*>(src), size)) {
            pauseWrite = false;
            writerPaused = true;
            pauseChanged.notify_all();
            pauseChanged.wait(lock, [] { return releaseWriter; });
        }
    }
    std::lock_guard<std::mutex> lock(guardIoMutex);
    guardFile.assign(static_cast<const char*>(src), size);
    if (guardPath) {
        FILE* file = std::fopen(guardPath, "wb");
        CHECK(file);
        if (file) {
            CHECK(std::fwrite(src, 1, size, file) == size);
            CHECK(std::fclose(file) == 0);
        }
    }
    ++guardWrites;
    return static_cast<int>(size);
}
int sceIoClose(int) { return 0; }
static int VgxQuiesceForExit(unsigned) { return 1; }
static void VgxQuiesceCancel(void) {}
static int sceAppMgrLoadExec(const char*, char* const*, void*) {
    ++restarts;
    if (execResolution == 1) VitaPlan::Heartbeat(300);
    else if (execResolution == 2) VitaPlan::Failed("failure while restarting");
    if (execSucceeds) std::_Exit(0);
    return -1;
}
static void sceKernelExitProcess(int) { if (exitSettings) std::_Exit(0); std::abort(); }
static void sceKernelDelayThread(int) {}
namespace VitaLog { void Flush() noexcept {} void Note(const char*) noexcept {} void ApplyStdioMode() {} }
// startcheck19: the start check for Picture and Language changes has its own test (test_change_guard.cpp); here it never applies, so the loader runs as before.
namespace NpChangeGuardVita { static bool Begin() { return false; } static bool Skips(const char*) { return false; } static const char* LoadingLine() { return nullptr; } }
namespace VitaBootScreen { void Notice(const char*) {} }

namespace NpSettingsScreen {
enum class Result { kExit, kStart, kRestart };
static const char* SavedFilePath() { return "np-player.txt"; }
static bool Requested(int, char**) { return true; }
static Result Run() {
    if (exitSettings) return Result::kExit;
    settingsFile = savedSettings;
    Result result = Result::kStart;
#include "settings_runtime_restart.inc"
}
}

#include "settings_runtime_loader.inc"

static void StartupSettingsFallback() {
    int argc = 0;
    char** argv = nullptr;
#include "settings_runtime_fallback.inc"
}
namespace NpSettingsScreen {
namespace { int g_restartRefusedCalls = 0; void RestartRefused() { ++g_restartRefusedCalls; } }
#include "settings_runtime_relaunch.inc"
}
static void RunningSettingsRestart() {
    // 1.3: the technical screen no longer restarts a match; the relaunch is the native screen's (NpNativeSettingsBackendRestart: the same function, a refusal says so)
    using NpSettingsScreen::RestartRefused;
    using NpSettingsScreen::RelaunchRunningGame;
    if (!RelaunchRunningGame()) RestartRefused();
}
static bool Is(const char* key, const char* value) {
    const char* actual = std::getenv(key);
    return value ? actual && !std::strcmp(actual, value) : !actual;
}
static std::string Owners(int count) {
    std::string text;
    for (int i = 0; i < count; ++i) text += "OWNER_" + std::to_string(i) + "=manual\n";
    return text;
}
static void Start(const std::string& text) {
    settingsFile = text;
    LoadEnvFile();
    ApplyPlan();
}
static std::string SelectPlan(const char* plan, bool removePointer = false) {
    NpSettings::State state;
    NpSettings::Load(state, settingsFile.c_str());
    NpSettings::SetChoice(state, NpSettings::OptionIndex("CORE PLAN"), !std::strcmp(plan, "S") ? 3 : !std::strcmp(plan, "Cn") ? 1 : 0);
    if (removePointer) NpSettings::SetChoice(state, NpSettings::OptionIndex("POINTER SPEED"), NpSettings::GetOption(NpSettings::OptionIndex("POINTER SPEED")).defaultChoice);   // (NORMAL: 5 of the 1-10 scale)
    std::string out;
    CHECK(NpSettings::Serialize(state, out));
    return out;
}
static void PauseGuardWrite(const char* value) {
    std::lock_guard<std::mutex> lock(pauseMutex);
    pauseValue = value;
    pauseWrite = true;
    writerPaused = releaseWriter = false;
}
static void WaitForPausedWrite() {
    std::unique_lock<std::mutex> lock(pauseMutex);
    CHECK(pauseChanged.wait_for(lock, std::chrono::seconds(2), [] { return writerPaused; }));
}
static void ReleasePausedWrite() {
    std::lock_guard<std::mutex> lock(pauseMutex);
    releaseWriter = true;
    pauseChanged.notify_all();
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) return 2;
    if (argc >= 3) {
        guardPath = argv[2];
        FILE* file = std::fopen(guardPath, "rb");
        if (!file) return 2;
        char buffer[32];
        const size_t n = std::fread(buffer, 1, sizeof(buffer), file);
        guardFile.assign(buffer, n);
        std::fclose(file);
    }
    const int initialFailures = std::atoi(guardFile.c_str());
    const std::string test = argv[1];
    if (test == "fallback-named" || test == "fallback-auto") {
        Start("# original\n");
        savedSettings = SelectPlan(test == "fallback-named" ? "S" : "auto");
        StartupSettingsFallback();
        CHECK(restarts == 1);
        CHECK(Is("MSC_FRAMESEND", test == "fallback-named" ? "off" : "thread"));
        CHECK(VitaPlan::Level() == (test == "fallback-named" ? 3 : 0));
        CHECK(guardWrites == (test == "fallback-named" ? 4 : 3));
        if (test == "fallback-named") {
            VitaPlan::Failed("named plan test");
            CHECK(guardWrites == 4 && guardFile == "0\n");
        }
    } else if (test == "full-named" || test == "full-cn") {
        Start(Owners(95) + "NP_POINTER_SPEED=1.5\n");
        settingsFile = SelectPlan(test == "full-named" ? "S" : "Cn", true);
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_FRAMESEND", test == "full-named" ? "off" : "thread"));
        CHECK(Is("MSC_FS_PIPE", nullptr));
        CHECK(VitaPlan::Level() == (test == "full-named" ? 3 : 1));
        CHECK(Is("OWNER_94", "manual"));
        CHECK(!std::getenv("NP_POINTER_SPEED"));
    } else if (test == "full-picture") {
        Start(Owners(95) + "MSC_PICTURE=performance\n");
        CHECK(Is("MSC_VITA_RENDER", "864x488") && Is("MSC_PERFORMANCE_MODE", "1"));   // (18 Nov floors: the 1.1 PERFORMANCE file's 640x362 is drawn at 864x488, MSC_PICTURE_FLOORS in the plan)
        {   // the floors on a fixed size: 480x272 (the old SMALLEST row) and a held native picture are raised, a size at or over the floor is kept, MSC_PICTURE_FLOORS=0 leaves a hand-set size alone
            settingsFile = "MSC_VITA_RENDER=480x272\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VITA_RENDER", "864x488"));
            settingsFile = "MSC_VITA_RENDER=864x488\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VITA_RENDER", "864x488"));
            settingsFile = "MSC_VITA_RENDER=912x520\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VITA_RENDER", "912x520"));
            settingsFile = "MSC_PICTURE_HOLD=native\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VITA_RENDER", "864x488"));   // (the held native size 640x448 is under the floor)
            settingsFile = "MSC_PICTURE_FLOORS=0\nMSC_VITA_RENDER=480x272\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VITA_RENDER", "480x272"));   // (the developer's switch)
            settingsFile = "MSC_PICTURE_FLOORS=0\nMSC_PICTURE_HOLD=native\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VITA_RENDER", "640x448"));
        }
        NpSettings::State state;
        NpSettings::Load(state, settingsFile.c_str());
        NpSettings::ResetAll(state);   // (1.3 prune: no picture modes; every row at its default is what QUALITY (AUTO) was)
        CHECK(NpSettings::Serialize(state, settingsFile));
        LoadEnvFile();
        ApplyPlan();
        // (plan24 FIX-01: the defaults are SMOOTH 60: LOCKED 60 and its dynamic resolution, no held picture and no fixed size)
        CHECK(Is("MSC_LOCK60", "1") && Is("MSC_PICTURE_HOLD", nullptr) && Is("MSC_VITA_RENDER", nullptr) && Is("MSC_PERFORMANCE_MODE", nullptr));
        CHECK(Is("MSC_FRAMESEND", "thread") && VitaPlan::Level() == 0);
    } else if (test == "performance-group") {
        // 1.3 prune: the rows over the picture, saved and re-read by the real loader: the keys appear, the next reread removes them (derived values do not outlive the file), and the
        // 96-assignment records are not exhausted. The keys of the rows that went are plain lines now: a file that names one still reaches the plan (no reader was removed).
        NpSettings::State state;
        NpSettings::Load(state, "MSC_WATCH=1\n");
        NpSettings::SetChoice(state, NpSettings::OptionIndex("STADIUM CROWD"), 1);
        NpSettings::SetChoice(state, NpSettings::OptionIndex("DEPTH OF FIELD"), 1);   // (ship26: ON, which writes lazy; the build default is 0)   // (set18: ANTI-ALIASING went)
        NpSettings::SetChoice(state, NpSettings::OptionIndex("SHARPNESS"), 1);
        CHECK(NpSettings::Serialize(state, settingsFile));
        Start(settingsFile);
        CHECK(Is("MSC_PERFORMANCE_MODE", "1") && Is("MSC_DOF", "lazy") && Is("MSC_VITA_UPSCALE", "off") && Is("MSC_WATCH", "1"));
        CHECK(Is("MSC_FPS", nullptr) && Is("MSC_VITA_RENDER", nullptr) && Is("MSC_LOCK60", "1") && Is("MSC_FPS_REPLAY", nullptr) && Is("MSC_DYNRES_MAX", nullptr));   // (the plan's full picture: no fixed size from the file)
        CHECK(Is("MSC_VITA_RT_TIMERS", "2"));   // (dynamic resolution stays on: the GPU timers are on)
        NpSettings::Load(state, settingsFile.c_str());
        NpSettings::SetChoice(state, NpSettings::OptionIndex("FRAME RATE"), NpSettings::ChoiceCount(NpSettings::OptionIndex("FRAME RATE")) - 1);   // 30 LOCKED
        CHECK(NpSettings::Serialize(state, settingsFile));
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_FPS", "30") && Is("MSC_PERFORMANCE_MODE", "1") && Is("MSC_FPS_REPLAY", nullptr) && Is("MSC_DYNRES_MAX", nullptr) && Is("MSC_WATCH", "1"));
        CHECK(Is("MSC_DYNRES_FG", "1") && Is("MSC_VITA_ANISO", "2"));   // (the build's own values for the rows that went)
        // a hand-written line of a row that went (it passes through the screen's save) still reaches the plan: dynamic resolution OFF takes the GPU timers with it
        settingsFile += "MSC_DYNRES=off\n";
        NpSettings::Load(state, settingsFile.c_str());
        NpSettings::SetChoice(state, NpSettings::OptionIndex("HD CHARACTERS"), 1);   // OFF
        CHECK(NpSettings::Serialize(state, settingsFile) && settingsFile.find("MSC_DYNRES=off") != std::string::npos);
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_DYNRES", "off") && Is("MSC_HDTEX", "0") && Is("MSC_VITA_RT_TIMERS", nullptr) && Is("MSC_FPS", "30"));
        // RESET TO DEFAULTS: everything the screen owns goes, and the hand line of the removed row with it
        NpSettings::Load(state, settingsFile.c_str());
        NpSettings::ResetAll(state);
        CHECK(NpSettings::Serialize(state, settingsFile));
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_DYNRES", nullptr) && Is("MSC_FPS", nullptr) && Is("MSC_VITA_RT_TIMERS", "2") && Is("MSC_WATCH", "1"));
        CHECK(Is("MSC_DYNRES_FG", "1") && Is("MSC_VGX_MSAA", "4") && Is("MSC_VITA_ANISO", "2") && Is("MSC_PICTURE_HOLD", nullptr) && Is("MSC_LOCK60", "1"));   // (the build's own values come back when the file no longer names them)
        // a 1.1 file with the old anti-aliasing line (set18: no row owns it): kept as written through a save
        settingsFile = "MSC_VITA_MSAA=2\nMSC_FPS=30\n";
        LoadEnvFile();
        CHECK(Is("MSC_VITA_MSAA", "2") && Is("MSC_FPS", "30"));
        NpSettings::Load(state, settingsFile.c_str());
        CHECK(NpSettings::Serialize(state, settingsFile) && settingsFile.find("MSC_VITA_MSAA=2") != std::string::npos);
        NpSettings::SetChoice(state, NpSettings::OptionIndex("HD CHARACTERS"), 1);
        CHECK(NpSettings::Serialize(state, settingsFile));
        LoadEnvFile();
        CHECK(Is("MSC_VITA_MSAA", "2") && Is("MSC_HDTEX", "0") && Is("MSC_FPS", "30"));
    } else if (test == "stock-sim-core3") {
        // clockfix17 F4: MSC_VITA_CORE3=1 puts the mixer and the file workers on the fourth core (a proven core: the stub below); under MSC_VITA_STOCK_SIM=1 they stay off it
        Start("MSC_VITA_CORE3=1\n");
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("NP_VITA_CORE_AUDIO", "3") && Is("NP_VITA_CORE_IO", nullptr));   // (the control: without the sim the mixer moves; T8: the disc and save workers stay on their plan core, the core 3 rules)
        Start("MSC_VITA_CORE3=1\nMSC_VITA_CORE3_DISC=1\n");
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("NP_VITA_CORE_AUDIO", "3") && Is("NP_VITA_CORE_IO", "3"));   // (T8: MSC_VITA_CORE3_DISC=1 gives them the core)
        Start("MSC_VITA_CORE3=1\nMSC_VITA_CORE3_RULES=0\n");
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("NP_VITA_CORE_AUDIO", "3") && Is("NP_VITA_CORE_IO", "3"));   // (T8: the rules off, the 1.3 placement)
        Start("MSC_VITA_CORE3=1\nMSC_VITA_STOCK_SIM=1\n");
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("NP_VITA_CORE_AUDIO", nullptr) && Is("NP_VITA_CORE_IO", nullptr) && Is("MSC_VITA_CORE3", "0"));
    } else if (test == "quality-defaults") {
        // 1.2, the owner's decision: the picture and timing items are on in the build (vita_plan.cpp), a key named in the file wins (=0 turns one off), and a reread decides
        // the conditional one (the multisampled rungs need the ladder) anew
        struct Item { const char* key; const char* on; const char* off; };
        const Item items[] = {{"MSC_DYNRES_FG", "1", "0"}, {"MSC_VGX_SHQUAD", "1", "0"}, {"MSC_DOF", "0", "lazy"}, {"MSC_VITA_QUEUE_FIXED30", "1", "0"},
                              {"MSC_VITA_PRIO_VBLANK", "0x70", "0"}, {"MSC_VITA_CORE0_ALONE", "1", "0"}, {"MSC_PACE_VBLANK", "1", "0"}, {"MSC_VITA_ANISO", "2", "0"},
                              {"MSC_SLIP", "20", "0"}, {"MSC_VGX_SOFTXFORM", "256", "128"}};
        Start("# nothing named\n");
        for (const Item& i : items) CHECK(Is(i.key, i.on));
        // plan24 FIX-01: an empty file is SMOOTH 60 (what the console runs measured with MSC_LOCK60=1 by hand): LOCK60 with its supporting defaults, the dynamic resolution's ladder (the multisampled
        // rungs), and NO held picture and no fixed size (the hold's 960x544 used to be set first, which made LOCKED 60 "do nothing here" and left all its supporting defaults unset)
        CHECK(Is("MSC_LOCK60", "1") && Is("MSC_PICTURE_HOLD", nullptr) && Is("MSC_VITA_RENDER", nullptr) && Is("MSC_PICTURE_HOLD_MSAA", nullptr) && Is("MSC_VGX_MSAA", "4"));
        CHECK(Is("MSC_AMORT_60", "1") && Is("MSC_FX_GOV", "1") && Is("MSC_FX_GOV_MEGA", "1") && Is("MSC_MEGA_WARM_KIB", "28672") && Is("MSC_VGX_HUDNATIVE", "1") && Is("MSC_LOCK60_HOLD", "1"));
        CHECK(Is("MSC_VGX_UNIDUP", "2") && Is("MSC_VITA_RT_TIMERS", "2"));   // (the speed list of 17 Oct is still there)
        CHECK(Is("MSC_VGX_GXMCACHE", nullptr) && Is("MSC_VGX_FBO_SAMPLES", nullptr) && Is("MSC_REDUCED_MOTION", nullptr) && Is("MSC_LAN_INPUT_DELAY", nullptr));   // (kept off)
        std::string off;
        for (const Item& i : items) off += std::string(i.key) + "=" + i.off + "\n";
        settingsFile = off;
        LoadEnvFile();
        ApplyPlan();
        for (const Item& i : items) CHECK(Is(i.key, i.off));
        // the file stops naming them: the defaults are back
        settingsFile = "MSC_WATCH=1\n";
        LoadEnvFile();
        ApplyPlan();
        for (const Item& i : items) CHECK(Is(i.key, i.on));
        // a fixed picture size, the dynamic resolution off or the old start-up multisampling: no multisampled rungs by default, everything else as before; a reread restores the defaults.
        // 1.3: none of them is a full picture (the player chose something it would fight), LOCKED 60 and a 30 FPS rate keep the ladder, so for those two the rungs are on
        // plan24 FIX-01: SMOOTH 60 is the default mode, so MSC_LOCK60 is set unless the file names a frame rate or a hold: its effects governor (FX_GOV) comes with it where LOCKED 60 is not blocked by a fixed size or DYNRES off
        struct Blocked { const char* line; const char* msaa; const char* fxGov; };
        for (const Blocked& b : {Blocked{"MSC_VITA_RENDER=640x362\n", nullptr, nullptr}, Blocked{"MSC_DYNRES=off\n", nullptr, nullptr}, Blocked{"MSC_DYNRES=0\n", nullptr, nullptr}, Blocked{"MSC_VITA_MSAA=2\n", nullptr, "1"},
                                 Blocked{"MSC_LOCK60=1\n", "4", "1"}, Blocked{"MSC_FPS=30\n", "4", nullptr}, Blocked{"MSC_PICTURE_HOLD=0\n", "4", "1"}}) {
            settingsFile = b.line;
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VGX_MSAA", b.msaa) && Is("MSC_DYNRES_FG", "1") && Is("MSC_VGX_SOFTXFORM", "256"));
            CHECK(Is("MSC_SHADOW_LIVE", "flat"));   // wave 9: the live-play shadow trade is on in every picture mode (hold off, fixed size, LOCKED 60, 30 FPS)
            CHECK(std::strcmp(b.line, "MSC_PICTURE_HOLD=0\n") == 0 ? Is("MSC_PICTURE_HOLD", "0") : Is("MSC_PICTURE_HOLD", nullptr));
            CHECK(Is("MSC_FX_GOV", b.fxGov));   // (LOCKED 60 brings its own effects governor)
            settingsFile = "MSC_WATCH=1\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_VGX_MSAA", "4") && Is("MSC_PICTURE_HOLD", nullptr) && Is("MSC_LOCK60", "1") && Is("MSC_LOCK60_HOLD", "1"));   // (the empty file's defaults, SMOOTH 60)
        }
        // plan24 FIX-01, the other modes: a named picture hold, a named frame rate (30 LOCKED; 60) or the 45 cadence by hand get no MSC_LOCK60 and none of its supporting defaults; a named hold is held
        // at 960x544 with the effects governor and its own anti-aliasing; a 30 or 45 rate keeps the ladder; SMOOTH 60 written by the Settings row (MSC_LOCK60=1) is the empty file's result
        struct Mode { const char* line; const char* lock60; const char* hold; const char* render; const char* fxGov; const char* lockHold; };
        for (const Mode& m : {Mode{"MSC_WATCH=1\n", "1", nullptr, nullptr, "1", "1"}, Mode{"MSC_LOCK60=1\n", "1", nullptr, nullptr, "1", "1"},
                              Mode{"MSC_FPS=30\n", nullptr, nullptr, nullptr, nullptr, nullptr}, Mode{"MSC_FPS=45\n", nullptr, nullptr, nullptr, nullptr, nullptr},
                              Mode{"MSC_FPS=60\n", nullptr, "full", "960x544", "1", nullptr}, Mode{"MSC_PICTURE_HOLD=full\n", nullptr, "full", "960x544", nullptr, nullptr},
                              Mode{"MSC_PICTURE_HOLD=0\n", "1", "0", nullptr, "1", "1"}, Mode{"MSC_VITA_RENDER=864x488\n", "1", nullptr, "864x488", nullptr, nullptr},
                              Mode{"MSC_DYNRES=off\n", "1", nullptr, nullptr, nullptr, nullptr}, Mode{"MSC_CADENCE45=1\n", nullptr, nullptr, nullptr, nullptr, nullptr}}) {
            settingsFile = m.line;
            LoadEnvFile();
            ApplyPlan();
            const bool ok = Is("MSC_PICTURE_HOLD", m.hold) && Is("MSC_VITA_RENDER", m.render) && Is("MSC_FX_GOV", m.fxGov) && Is("MSC_LOCK60_HOLD", m.lockHold) && Is("MSC_LOCK60", m.lock60);
            if (!ok) std::fprintf(stderr, "mode '%s' wrong\n", m.line);
            CHECK(ok);
        }
        settingsFile = "MSC_WATCH=1\n";
        LoadEnvFile();
        ApplyPlan();
        // wave 9: a file that names LIVE SHADOWS FULL keeps the game's shadow pictures, with the hold on and off
        for (const char* line : {"MSC_SHADOW_LIVE=full\n", "MSC_SHADOW_LIVE=full\nMSC_PICTURE_HOLD=0\n"}) {
            settingsFile = line;
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_SHADOW_LIVE", "full"));
        }
        settingsFile = "MSC_WATCH=1\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_SHADOW_LIVE", "flat"));
        // 1.3 the held picture's anti-aliasing: 2 samples by default; the ANTI-ALIASING row maps onto MSC_PICTURE_HOLD_MSAA while the hold is on (OFF 0, a 1.3-dev file's MSC_VGX_MSAA=2 still 2, the ladder's default value 4 reads as ON, 4X its own key);
        // with the hold off (the older ladder) the key is not set and the rungs are the ladder's
        {
            struct Aa { const char* line; const char* want; };
            // (plan24: the hold is no longer the empty file's picture, SMOOTH 60 is: these name the hold)
            for (const Aa& c : {Aa{"MSC_PICTURE_HOLD=full\n", "2"}, Aa{"MSC_PICTURE_HOLD=full\nMSC_VGX_MSAA=0\n", "0"}, Aa{"MSC_PICTURE_HOLD=full\nMSC_VGX_MSAA=2\n", "2"}, Aa{"MSC_PICTURE_HOLD=full\nMSC_VGX_MSAA=4\n", "2"}, Aa{"MSC_PICTURE_HOLD=full\nMSC_PICTURE_HOLD_MSAA=4\n", "4"},
                                Aa{"MSC_PICTURE_HOLD=full\nMSC_PICTURE_HOLD_MSAA=0\nMSC_VGX_MSAA=2\n", "0"}, Aa{"MSC_PICTURE_HOLD=0\n", nullptr}, Aa{"MSC_PICTURE_HOLD=0\nMSC_PICTURE_HOLD_MSAA=4\n", "4"}, Aa{"MSC_VITA_MSAA=2\n", nullptr}}) {
                settingsFile = c.line;
                LoadEnvFile();
                ApplyPlan();
                CHECK(Is("MSC_PICTURE_HOLD_MSAA", c.want));
            }
            settingsFile = "MSC_WATCH=1\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(Is("MSC_PICTURE_HOLD_MSAA", nullptr) && Is("MSC_VGX_MSAA", "4") && Is("MSC_PICTURE_HOLD", nullptr));
        }
        // asked for by hand with a fixed size: honoured
        settingsFile = "MSC_VITA_RENDER=640x362\nMSC_VGX_MSAA=2\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_VGX_MSAA", "2"));
    } else if (test == "file-cap") {
        Start(Owners(97));
        CHECK(Is("OWNER_95", "manual") && Is("OWNER_96", nullptr));
        settingsFile = "AFTER_REREAD=kept\n";
        LoadEnvFile();
        CHECK(Is("OWNER_95", nullptr) && Is("AFTER_REREAD", "kept"));
        CHECK(Is("MSC_FRAMESEND", nullptr) && Is("MSC_FS_PIPE", nullptr));
    } else if (test == "duplicate-cap") {
        std::string text;
        for (int i = 0; i < 96; ++i) text += "OWNER=same\n";
        Start(text + "IGNORED=overflow\n");
        CHECK(Is("OWNER", "same") && Is("IGNORED", nullptr));
    } else if (test == "timers-reread") {
        Start("# dynamic resolution on\n");
        CHECK(Is("MSC_VITA_RT_TIMERS", "2"));
        settingsFile = "MSC_DYNRES=off\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_VITA_RT_TIMERS", nullptr));
        settingsFile = "MSC_DYNRES=1\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_VITA_RT_TIMERS", "2"));
    } else if (test == "timers-explicit-0" || test == "timers-explicit-3") {
        const char* value = test == "timers-explicit-0" ? "0" : "3";
        const std::string timer = std::string("MSC_VITA_RT_TIMERS=") + value + "\n";
        Start("MSC_DYNRES=off\n" + timer);
        CHECK(Is("MSC_VITA_RT_TIMERS", value));
        settingsFile = "MSC_DYNRES=1\n" + timer;
        LoadEnvFile();
        ApplyPlan();
        CHECK(Is("MSC_VITA_RT_TIMERS", value));
        int records = 0;
        for (int i = 0; i < g_envNameCount; ++i)
            if (!std::strcmp(g_envNames[i], "MSC_VITA_RT_TIMERS")) ++records;
        CHECK(records == 1);
    } else if (test == "full-tracking") {
        Start(Owners(95) + "MSC_PICTURE=performance\n");
        CHECK(g_envNameCount == 104);
        settingsFile = "MSC_DYNRES=off\n";
        LoadEnvFile();
        CHECK(Is("MSC_VITA_RT_TIMERS", nullptr) && Is("MSC_VITA_RENDER", nullptr) && Is("MSC_PERFORMANCE_MODE", nullptr));
        CHECK(Is("MSC_FRAMESEND", nullptr) && Is("MSC_VGX_BACKEND_THREAD", nullptr) && Is("MSC_VITA_CORE_FRAMESEND", nullptr));
        CHECK(Is("MSC_FS_PIPE", nullptr) && Is("NP_VITA_CORE_THP", nullptr));
        ApplyPlan();
        CHECK(Is("MSC_VITA_RT_TIMERS", nullptr));
    } else if (test == "guard-auto") {
        Start("# first start\n");
        LoadEnvFile();
        ApplyPlan();
        CHECK(VitaPlan::Level() == 0 && guardWrites == 1 && guardFile == "1\n");
        VitaPlan::Heartbeat(300);
        CHECK(guardWrites == 2 && guardFile == "0\n");
    } else if (test == "guard-manual") {
        Start("# first start\n");
        settingsFile = "MSC_FRAMESEND=off\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(VitaPlan::Level() == -1);
        VitaPlan::Failed("manual thread test");
        VitaPlan::Heartbeat(300);
        CHECK(guardWrites == 2 && guardFile == "0\n");
    } else if (test == "guard-return-auto") {
        Start("# first start\n");
        settingsFile = "MSC_PLAN=S\n";
        LoadEnvFile();
        ApplyPlan();
        VitaPlan::Failed("named plan test");
        CHECK(guardWrites == 2 && guardFile == "0\n");
        settingsFile = "# back to auto\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(VitaPlan::Level() == 0 && guardWrites == 3 && guardFile == "1\n");
    } else if (test == "guard-leave-named" || test == "guard-leave-manual" || test == "guard-return-named" || test == "guard-return-manual") {
        Start("# initial automatic plan\n");
        const bool manual = test.find("manual") != std::string::npos;
        settingsFile = manual ? "MSC_FRAMESEND=off\n" : "MSC_PLAN=S\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(guardFile == std::to_string(initialFailures) + "\n" && guardWrites == 2);
        CHECK(VitaPlan::Level() == (manual ? -1 : 3));
        VitaPlan::Heartbeat(300);
        VitaPlan::Failed("explicit plan is unguarded");
        CHECK(guardFile == std::to_string(initialFailures) + "\n" && guardWrites == 2);
        if (test.find("return") != std::string::npos) {
            settingsFile = "# return to automatic\n";
            LoadEnvFile();
            ApplyPlan();
            CHECK(guardFile == std::to_string(initialFailures + 1) + "\n" && guardWrites == 3);
            CHECK(VitaPlan::Level() == (initialFailures < 3 ? initialFailures : 3));
            LoadEnvFile();
            ApplyPlan();
            CHECK(guardWrites == 3);
        }
    } else if (test == "guard-failed-leave" || test == "guard-cleared-leave") {
        Start("# initial automatic plan\n");
        const bool failed = test == "guard-failed-leave";
        const int resolved = failed ? VitaPlan::Level() + 1 : 0;
        if (failed) VitaPlan::Failed("genuine startup failure");
        else VitaPlan::Heartbeat(300);
        CHECK(guardFile == std::to_string(resolved) + "\n" && guardWrites == 2);
        settingsFile = "MSC_PLAN=S\n";
        LoadEnvFile();
        ApplyPlan();
        CHECK(guardFile == std::to_string(resolved) + "\n" && guardWrites == 2);
        settingsFile = "# return to automatic after resolved result\n";
        LoadEnvFile();
        ApplyPlan();
        VitaPlan::Heartbeat(300);
        CHECK(guardFile == std::to_string(resolved) + "\n" && guardWrites == 2);
    } else if (test == "concurrent-cancel-heartbeat") {
        guardFile = "1\n";
        Start("# initial automatic plan\n");
        PauseGuardWrite("1\n");
        auto cancel = std::async(std::launch::async, [] { return VitaPlan::CancelPendingStart(); });
        WaitForPausedWrite();
        auto heartbeat = std::async(std::launch::async, [] { VitaPlan::Heartbeat(300); });
        CHECK(heartbeat.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
        ReleasePausedWrite();
        CHECK(cancel.get());
        heartbeat.get();
        CHECK(guardFile == "0\n");
    } else if (test == "concurrent-restore-failure" || test == "concurrent-cancel-failure") {
        guardFile = "1\n";
        Start("# initial automatic plan\n");
        const bool restore = test == "concurrent-restore-failure";
        if (restore) CHECK(VitaPlan::CancelPendingStart());
        PauseGuardWrite(restore ? "2\n" : "1\n");
        auto writer = std::async(std::launch::async, [restore] {
            if (restore) VitaPlan::RestorePendingStart(true);
            else VitaPlan::CancelPendingStart();
        });
        WaitForPausedWrite();
        auto failed = std::async(std::launch::async, [] { VitaPlan::Failed("concurrent real failure"); });
        CHECK(failed.wait_for(std::chrono::milliseconds(5000)) == std::future_status::ready);
        ReleasePausedWrite();
        writer.get();
        failed.get();
        if (restore) {
            settingsFile = "MSC_PLAN=S\n";
            LoadEnvFile();
            ApplyPlan();
        } else VitaPlan::RestorePendingStart(true);
        CHECK(guardFile == "2\n");
    } else if (test == "restart-success-startup" || test == "restart-success-running" || test == "settings-exit") {
        Start("# automatic startup before settings\n");
        if (test == "settings-exit") exitSettings = true;
        else {
            savedSettings = SelectPlan("S");
            execSucceeds = true;
        }
        if (test == "restart-success-running") RunningSettingsRestart();
        else StartupSettingsFallback();
        CHECK(false);   // successful exec and process exit cannot return
    } else if (test == "restart-failed-running" || test == "restart-failed-startup") {
        Start("# automatic startup before settings\n");
        if (test == "restart-failed-startup") {
            savedSettings = SelectPlan("auto");
            StartupSettingsFallback();
        } else RunningSettingsRestart();
        CHECK(restarts == 1);
        CHECK(guardFile == std::to_string(initialFailures + 1) + "\n" && guardWrites == 3);
        CHECK(VitaPlan::Level() == (initialFailures < 3 ? initialFailures : 3));
    } else if (test == "restart-return-heartbeat" || test == "restart-return-failure") {
        Start("# automatic startup before settings\n");
        execResolution = test == "restart-return-heartbeat" ? 1 : 2;
        RunningSettingsRestart();
        const int expected = execResolution == 1 ? 0 : VitaPlan::Level() + 1;
        CHECK(guardFile == std::to_string(expected) + "\n" && guardWrites == 3);
        VitaPlan::Heartbeat(300);
        CHECK(guardFile == std::to_string(expected) + "\n" && guardWrites == 3);
    } else if (test == "guard-fresh-named") {
        if (argc != 4) return 2;
        const int expected = std::atoi(argv[3]);
        Start("MSC_PLAN=S\n");
        VitaPlan::Heartbeat(300);
        CHECK(VitaPlan::Level() == 3 && guardWrites == 0);
        CHECK(guardFile == std::to_string(expected) + "\n");
    } else if (test == "guard-fresh") {
        if (argc != 4) return 2;
        const int expected = std::atoi(argv[3]);
        CHECK(initialFailures == expected);
        Start("# fresh executable process\n");
        CHECK(VitaPlan::Level() == (expected < 3 ? expected : 3));
        CHECK(guardFile == std::to_string(initialFailures + 1) + "\n" && guardWrites == 1);
    } else if (test.compare(0, 11, "guard-boot-") == 0) {
        const int failed = std::atoi(test.c_str() + 11);
        guardFile = std::to_string(failed) + "\n";
        Start("# new process\n");
        CHECK(VitaPlan::Level() == (failed < 3 ? failed : 3));
        CHECK(guardFile == std::to_string(failed + 1) + "\n" && guardWrites == 1);
        VitaPlan::Failed("real startup failure");
        CHECK(guardFile == std::to_string(VitaPlan::Level() + 1) + "\n");
    } else return 2;
    std::printf("%s: %d failures\n", test.c_str(), failures);
    return failures ? 1 : 0;
}
