// vita_plan.cpp (Vita only): the core plan of the player build and its safety net (PIPE, wave 5).
//
// The plans (np/docs/wave4-VRS.md section 4, wave5-PIPE.md):
//   C  frame send on a worker (core 1) + the record player on its own thread (core 2) + GL back end (core 1) + the two-stage pipeline   the best plan, the player's default
//   Cn the same without the pipeline (MSC_FS_PIPE not set: the join of the send is at the task boundary)                                  what C was before the pipeline
//   A  frame send on a worker (core 2), the player and the GL back end on one thread (core 1)                                              what the model calls the default plan
//   S  no worker at all: the frame is sent on the game thread (the build's old default)                                                    the way out
// The player build starts in C. C has the most threads and the newest code (the pipeline, the back end hand-off), and it has never run on a console, so a
// guard file (ux0:data/mscharged/np-plan.txt, one number) steps down when a start does not get anywhere:
//   at start       the number is read and written back plus one;     0 -> C, 1 -> Cn, 2 -> A, 3 or more -> S
//   at 300 frames  presented (about 10 s of running; 900 before 17 Oct): the number is set to 0 again (the start was fine)
//   on a stall     (no frame for 30 s) or a crash: the number is set to the level that was running plus one (the next start uses the next plan)
// Nothing here applies when the owner's np-player.txt (or a lab build's env.txt) sets one of MSC_FRAMESEND, MSC_VGX_BACKEND_THREAD,
// MSC_VITA_CORE_FRAMESEND or MSC_PLAN: explicit settings win, no guard (MSC_FS_PIPE alone is a switch on top of whatever plan runs). MSC_PLAN=C|Cn|A|S|auto picks a plan by hand (auto = the guard's choice);
// a lab build does nothing unless MSC_PLAN is set.
#include "np_vita_internal.h"
#include "np_vita_core3.h"
#include "np_launch_guard.h"
#include "../../sdk/include/np/np_text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <pthread.h>

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#endif

// 1.3 (SC06): the Vita's own language, as "es", "fr" or "en" (np_vita_main.cpp), and the disc folder (NpGameRootPath). Both are weak: a host test, or a build without them, simply has neither.
extern "C" const char* NpVitaSystemLanguageCode(void) __attribute__((weak));
extern "C" const char* NpGameRootPath(void) __attribute__((weak));

// startcheck19: the start check for Picture and Language changes (np_change_guard_vita.cpp) follows this guard's Cancel / Restore / Failed / Heartbeat. Weak: a host test of this
// file has no glue; nothing happens there.
namespace NpChangeGuardVita {
bool Cancel() __attribute__((weak));
void Restore() __attribute__((weak));
void Failed() __attribute__((weak));
void Stable() __attribute__((weak));
}

namespace VitaPlan {
namespace {

const char* g_languageNotice = nullptr;   // an English key of np_text_strings.tsv for the loading screen, or null
bool DiscHas(const char* root, const char* relative) {
    char path[400];
    std::snprintf(path, sizeof(path), "%s/files/Art/fe/%s", root, relative);
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

constexpr const char* kGuardFile = "ux0:data/mscharged/np-plan.txt";
// A start that presented this many frames worked (17 Oct: 300, about 10 s; it was 900: a look at the loading screen and a quit counted as a failure).
constexpr int kClearFrames = 300;
int g_level = -1;          // plan that is running (0 C, 1 Cn, 2 A, 3 S), -1 = no plan applied
bool g_guarded = false;    // the guard file is in use
bool g_cleared = false;
bool g_guardStarted = false;
bool g_guardReserved = false;
int g_startFailures = 0;
pthread_mutex_t g_guardMutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<bool> g_failurePending{false};

#if defined(__vita__)
int ReadGuard() {
    const SceUID fd = sceIoOpen(kGuardFile, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    char text[16] = {};
    const int n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    return n > 0 ? std::atoi(text) : 0;
}
void WriteGuard(int value) {
    const SceUID fd = sceIoOpen(kGuardFile, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    char text[16];
    const int n = std::snprintf(text, sizeof(text), "%d\n", value);
    sceIoWrite(fd, text, static_cast<SceSize>(n));
    sceIoClose(fd);
}
#else
int ReadGuard() { return 0; }
void WriteGuard(int) {}
#endif

// ---- the launch guard (stab13, np_launch_guard.h): ux0:data/mscharged/np-launch.txt, one number -----------------------------------------------------------------------
constexpr const char* kLaunchFile = "ux0:data/mscharged/np-launch.txt";
NpLaunchGuard::State g_launchState = NpLaunchGuard::State::kOff;
bool g_launchDecided = false;     // decided once per process (a settings reread applies the same decision again)
bool g_launchMarked = false;      // the file holds this launch's "running" number
int g_launchPrevious = 0;
bool g_launchSafeLogged = false;
#if defined(__vita__)
// 0 read, 1 no file, 2 error
int ReadLaunchFile(int* value) {
    const SceUID fd = sceIoOpen(kLaunchFile, SCE_O_RDONLY, 0);
    if (fd < 0) return static_cast<unsigned>(fd) == 0x80010002u ? 1 : 2;   // SCE_ERROR_ERRNO_ENOENT
    char text[16] = {};
    const int n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n < 0) return 2;
    *value = NpLaunchGuard::ParseCount(text, static_cast<size_t>(n));
    return 0;
}
bool WriteLaunchFile(int value) {
    const SceUID fd = sceIoOpen(kLaunchFile, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return false;
    char text[16];
    const int n = std::snprintf(text, sizeof(text), "%d\n", value);
    const int w = sceIoWrite(fd, text, static_cast<SceSize>(n));
    sceIoClose(fd);
    return w == n;
}
unsigned long long ProcessMs() { return sceKernelGetProcessTimeWide() / 1000ull; }
#else
int ReadLaunchFile(int*) { return 2; }
bool WriteLaunchFile(int) { return false; }
unsigned long long ProcessMs() { return 0; }
#endif

bool Has(const char* name) {
    const char* e = std::getenv(name);
    return e && *e;
}

bool LaunchGuardSwitchedOff() {
    // Coordinator review (22 Nov): default OFF. A close from the LiveArea (how most players quit) cannot be told from a freeze, so a short session ended
    // that way would make the next launch a slower "safe" one. MSC_LAUNCH_GUARD=1 turns it on (console test s32-launch decides).
    const char* e = std::getenv("MSC_LAUNCH_GUARD");
    if (!e || *e != '1') return true;
    return Has("MSC_BOOT_MATCH") || Has("MSC_INPUT_SCRIPT");   // the console test kit kills its runs: a kill is not a freeze there
}

// Reads the file and writes this launch's number, once per process; the state it decides is kept for a settings reread.
void LaunchGuardBegin() {
    if (g_launchDecided) return;
    g_launchDecided = true;
    g_launchState = NpLaunchGuard::State::kOff;
    if (LaunchGuardSwitchedOff()) return;
    int previous = 0;
    if (ReadLaunchFile(&previous) == 2) {   // a card error: the guard does nothing this start
        std::fprintf(stderr, "[launch] %s cannot be read: no launch guard this start\n", kLaunchFile);
        return;
    }
    g_launchPrevious = previous;
    g_launchState = NpLaunchGuard::Decide(previous);
    g_launchMarked = WriteLaunchFile(NpLaunchGuard::NextCount(previous));
    std::fprintf(stderr, "[launch] marker %s: %d earlier launch(es) in a row did not end cleanly -> %s%s\n", kLaunchFile, previous,
                 g_launchState == NpLaunchGuard::State::kSafe ? "this launch starts with the optional thread / core / pacing switches off" :
                 g_launchState == NpLaunchGuard::State::kGaveUp ? "an ordinary start (the safe start was tried twice already)" : "an ordinary start",
                 g_launchMarked ? "" : " (the marker could not be written)");
}

// A clean end of this launch (or stable play): the number goes back to 0. Once.
void LaunchGuardClear(const char* why) {
    if (!g_launchMarked) return;
    g_launchMarked = false;
    WriteLaunchFile(0);
    std::fprintf(stderr, "[launch] %s: the launch marker is cleared\n", why);
}


const char* LevelName(int level) { return level == 0 ? "C" : level == 1 ? "Cn" : level == 2 ? "A" : "S"; }

void FailedUnlocked(const char* why) {
    if (NpChangeGuardVita::Failed) NpChangeGuardVita::Failed();
    if (!g_guarded) return;
    g_cleared = true;
    g_guardReserved = false;
    WriteGuard(g_level + 1);
    std::fprintf(stderr, "[plan] %s: the next start uses plan %s\n", why, LevelName(g_level + 1 > 3 ? 3 : g_level + 1));
}

void DrainPendingFailure() {
    if (g_failurePending.exchange(false)) FailedUnlocked("failure during guard update");
}

// pthread's static initializer also works before VitaSDK's C++ mutex constructors (EarlyBoot uses this code).
class GuardLock {
public:
    explicit GuardLock(bool tryOnly = false)
        : locked_((tryOnly ? pthread_mutex_trylock(&g_guardMutex) : pthread_mutex_lock(&g_guardMutex)) == 0) {
        if (locked_ && !tryOnly) DrainPendingFailure();
    }
    ~GuardLock() {
        while (locked_) {
            DrainPendingFailure();
            pthread_mutex_unlock(&g_guardMutex);
            locked_ = false;
            // Failed may have arrived between the last drain and unlock. Repair under the same lock, or let its new owner drain it.
            if (g_failurePending.load()) locked_ = pthread_mutex_trylock(&g_guardMutex) == 0;
        }
    }
    explicit operator bool() const { return locked_; }
    GuardLock(const GuardLock&) = delete;
    GuardLock& operator=(const GuardLock&) = delete;
private:
    bool locked_;
};

// The names Apply gave a default to (kSpeed and kQuality below), so that a reread of the settings file (ForgetDefaults) can take them back before the
// file is read again: a default that depends on another setting (the multisampled rungs need the dynamic resolution and no fixed size) is decided anew.
const char* g_defaulted[320];   // (18 Nov: 160 was full with ai27 + phys27; wave 9: 80 was full at the 72nd default; 1.3: 40 was nearly full; a name past the end would not be taken back on a reread)
int g_defaultedCount = 0;
void RememberDefault(const char* name) {
    if (g_defaultedCount < static_cast<int>(sizeof(g_defaulted) / sizeof(g_defaulted[0]))) g_defaulted[g_defaultedCount++] = name;
}

struct LaunchEnv {
    bool IsPlanDefault(const char* name) const {
        for (int i = 0; i < g_defaultedCount; ++i)
            if (std::strcmp(g_defaulted[i], name) == 0) return true;
        return std::strcmp(name, "MSC_FS_PIPE") == 0 && !Has(name);   // the thread plan below sets it (plan C): turned off before it does
    }
    void Set(const char* name, const char* value) const {
        bool known = false;
        for (int i = 0; i < g_defaultedCount; ++i)
            if (std::strcmp(g_defaulted[i], name) == 0) known = true;
        setenv(name, value, 1);
        if (!known) RememberDefault(name);
        std::fprintf(stderr, "[launch] safe start: %s=%s\n", name, value);
    }
};

bool CancelPendingStartUnlocked() {
    if (NpChangeGuardVita::Cancel) NpChangeGuardVita::Cancel();   // (its own count, whatever this guard does)
    if (!g_guardReserved) return false;
    WriteGuard(g_startFailures);
    g_guardReserved = false;
    return true;
}

// (1.3) Where the mixer and the file workers go, once the plan is known. Their roles are the SDK layer's (np_plat_vita.cpp), read when their threads start, later than this.
//   MSC_VITA_CORE3=1 with a proven core: mixer and disc and save workers on it; np_plat_vita.cpp goes back to the plain mask if the system refuses a thread.
//   MSC_VITA_CORE_GC: the collector's place (read by vita_threads.cpp), and the mixer's where that is off the player and the game (np_vita_core3.h).
// A line of the owner's for NP_VITA_CORE_AUDIO or NP_VITA_CORE_IO wins. Nothing here is on without a key: the Strikers A/B has not been run. Thread placement only: no game tick,
// random number or sync record depends on a core.
void PlaceAudioAndFiles(int level) {
    // MSC_VITA_STOCK_SIM=1 (test only, clockfix17): a stock console has no fourth core; nothing is placed, whatever MSC_VITA_CORE3 said before the sim forced it off.
    if (const char* sim = std::getenv("MSC_VITA_STOCK_SIM"); sim && std::strcmp(sim, "1") == 0) {
        std::fprintf(stderr, "[plan] MSC_VITA_STOCK_SIM=1: mixer and file workers stay off the fourth core\n");
        return;
    }
    if (VitaThreads::Core3Active && VitaThreads::Core3Active()) {
        // (T8) The rules (np_vita_core3.h): the disc and save workers (NP_VITA_CORE_IO) stay on their plan core unless MSC_VITA_CORE3_DISC=1; MSC_VITA_CORE3_RULES=0 gives back the 1.3 placement.
        const bool rules = NpVitaCore3::RulesOn(std::getenv("MSC_VITA_CORE3"), std::getenv("MSC_VITA_CORE3_RULES"));
        const bool disc = !rules || NpVitaCore3::DiscWanted(std::getenv("MSC_VITA_CORE3_DISC"));
        static const char* const kMoved[] = {"NP_VITA_CORE_AUDIO", "NP_VITA_CORE_IO"};
        for (const char* name : kMoved) {
            if (Has(name)) continue;
            if (!disc && !std::strcmp(name, "NP_VITA_CORE_IO")) continue;
            setenv(name, "3", 0);
            RememberDefault(name);
        }
    }
    const char* gc = std::getenv("MSC_VITA_CORE_GC");
    if (const char* mixer = NpVitaCore3::MixerFollows(gc, level); mixer && !Has("NP_VITA_CORE_AUDIO")) {
        setenv("NP_VITA_CORE_AUDIO", mixer, 0);
        RememberDefault("NP_VITA_CORE_AUDIO");
    }
    if (gc && *gc) std::fprintf(stderr, "[plan] MSC_VITA_CORE_GC=%s: mixer on %s\n", gc, Has("NP_VITA_CORE_AUDIO") ? std::getenv("NP_VITA_CORE_AUDIO") : "its default core");
}

} // namespace

const char* LanguageNotice() { return g_languageNotice; }

void ForgetDefaults() {
    GuardLock lock;
    if (!lock) return;
    for (int i = 0; i < g_defaultedCount; ++i) unsetenv(g_defaulted[i]);
    g_defaultedCount = 0;
}

bool CancelPendingStart() {
    GuardLock lock;
    return lock && CancelPendingStartUnlocked();
}

void RestorePendingStart(bool cancelled) {
    GuardLock lock;
    if (!lock) return;
    if (NpChangeGuardVita::Restore) NpChangeGuardVita::Restore();
    if (!cancelled || !g_guarded || g_guardReserved || g_cleared) return;
    WriteGuard(g_startFailures + 1);
    g_guardReserved = true;
}

// 1.2 PS TV: the console without a touch screen and without a rear touchpad (the PlayStation TV / Vita TV). MSC_PSTV=1|0 forces the answer (tests, and a console that
// cannot say); unset, the system is asked (sceKernelIsPSVitaTV).
bool IsPsTv() {
    if (const char* e = std::getenv("MSC_PSTV"); e && *e) return *e != '0';
#if defined(__vita__)
    return sceKernelIsPSVitaTV() == 1;
#else
    return false;
#endif
}

// Called after the settings file was read. Returns the names of the variables it set in `names` (so that a second reading can take them back).
int Apply(const char** names, int maxNames) {
    GuardLock lock;
    if (!lock) return 0;
    g_level = -1;
    g_guarded = false;
#if !defined(MSC_LAB)
    const bool player = true;   // (the player build and the release build: not a lab build)
#else
    const bool player = false;
#endif
    // The AI script time budget (docs/wave5-PIPE.md section 5): the retail rule skips scripts when 1 ms of REAL time is used, which makes the AI weaker on a CPU slower than the
    // Wii's and the match depend on the clock. The player build starts with MSC_AI_BUDGET=off (no script is skipped, nothing is drawn from nlDefaultSeed = what the network matches
    // and the oracle runs do); the owner's line MSC_AI_BUDGET=retail|slow:K|ops:R in np-player.txt wins. Lab builds keep the retail rule unless env.txt says otherwise.
    if (player && !Has("MSC_AI_BUDGET")) {
        setenv("MSC_AI_BUDGET", "off", 0);
        std::fprintf(stderr, "[plan] AI script budget: off (the player build's default; MSC_AI_BUDGET=retail in np-player.txt brings back the original real-time rule)\n");
    }
    // 1.3 (SC06, "how do I play in Spanish"): the game's language. One setting does both halves: MSC_LANGUAGE (the Settings screen's LANGUAGE row, or by hand) is read by
    // the SDK as the game's own system language (os_core.cpp: the disc's North American Spanish or French tables, fonts and menu pictures, NP_LANG 4 or 3) and by the port's
    // texts (np_text.h: this screen, the loading screen, the online messages). A hand-written NP_LANG=4 still works for the game and for the port (np_text.h reads it too).
    //   - Neither is set: the Vita's own language (System Settings), when it is Spanish or French; anything else stays English. The first start is then in the player's language
    //     without a visit to Settings. The row shows it (it reads the file, so it says ENGLISH until the player saves a choice; the saved choice then wins).
    //   - The language is Spanish or French but the disc has no tables for it (the folder ux0:data/mscharged/game/files/Art/fe has no naspanish.loc / nafrench.loc: another
    //     region's disc, a partial copy): the game would load nothing for it, so English is used, the log says so and the loading screen shows one line.
    g_languageNotice = nullptr;
    if (player) {
        if (!Has("MSC_LANGUAGE") && !Has("NP_LANG") && NpVitaSystemLanguageCode) {
            const char* code = NpVitaSystemLanguageCode();
            if (code && (std::strcmp(code, "es") == 0 || std::strcmp(code, "fr") == 0)) {
                setenv("MSC_LANGUAGE", code, 0);
                RememberDefault("MSC_LANGUAGE");
                std::fprintf(stderr, "[plan] language: the Vita's system language is %s, so MSC_LANGUAGE=%s (the LANGUAGE row of Settings or a line in np-player.txt wins)\n", code, code);
            }
        }
        const int wanted = NpText::LanguageFromEnv();
        if ((wanted == NpText::kSpanish || wanted == NpText::kFrench) && NpGameRootPath) {
            const char* root = NpGameRootPath();
            const char* table = wanted == NpText::kSpanish ? "naspanish.loc" : "nafrench.loc";
            if (root && *root && !DiscHas(root, table)) {
                setenv("MSC_LANGUAGE", "en", 1);
                setenv("NP_LANG", "1", 1);
                RememberDefault("MSC_LANGUAGE");
                RememberDefault("NP_LANG");
                g_languageNotice = wanted == NpText::kSpanish ? "NO SPANISH ON THIS DISC: ENGLISH IS USED." : "NO FRENCH ON THIS DISC: ENGLISH IS USED.";
                std::fprintf(stderr, "[plan] language: %s is not on this disc (%s/files/Art/fe/%s is missing): English is used\n", wanted == NpText::kSpanish ? "Spanish" : "French", root, table);
            }
        }
    }
    // Speed switches measured on the console (17 Oct 2026, hw/w5/RESULTS.md run c6): open play went from 29.2-29.6 to 29.9 fps (p95 35 ms) with no picture
    // change. Every Vita build (lab and player) gets them unless env.txt / np-player.txt sets the name (=0 turns one off):
    //   MSC_VITA_VGL_SKIP=1      vitaGL skips sceGxm sets that repeat the state already set (GLB)
    //   MSC_VGX_UNIDUP=2         uniform blocks equal to the bound ones are not uploaded again (GLB)
    //   MSC_VGX_PROMOSMALL=3     small replayed display lists get static GPU buffers after 3 calm replays (GXD: half the open-play stream)
    {
        //   MSC_SKIN_GPU=1           CPU-skinned characters (cut-scenes, replays, celebrations, choose sides, stadium select) skinned in the vertex
        //                            shader; their lists become static buffers (W5 SKIN: stream 3.2-4.4 -> 0.45 MB in replays, decode -55 %) [host-proven,
        //                            pictures and simulation equal; console A/B: env-np-skin-{off,on,gpu,inst}.txt set both explicitly]
        //   MSC_VGX_SOFTINST=16      the 3D menus' ~880 small CPU-transformed meshes drawn as instanced batches from persistent buffers (W5 SKIN:
        //                            captains stream 1.65 -> 0.09 MB) [host-proven, as above]
        //   MSC_VGX_BLENDNODISCARD=1 a blended, depth-write-off draw whose alpha test rejects only alpha 0 is made without the discard (W12 CROWD: host-proven,
        //                            md5 of the dumped pictures equal at five open-play frames; a fragment below half an alpha step is the only difference)
        //   MSC_VGX_CELLCLEAR=1      the clears of the crowd-impostor atlas cells (about 27 scissored glClear calls a frame in open play) made as one quad draw
        //                            per run of cells; colour 0 and depth 1 written to the bit, rectangles identical (host: md5 of the dumped pictures equal at the
        //                            open-play and menu frames tested) [console A/B: =0 is the plain glClear]
        //   MSC_VGX_CROWDTRIM=1      each crowd sprite quad is cut to the part of its picture a figure was drawn in: about nine tenths of the crowd's
        //                            fragments are the sprites' transparent margins (W12 CROWD: host-proven, the pictures are the same; the console A/B
        //                            is tools/console-1.2 runs lookctrim0 and ctrim0)
        static const char* const kSpeed[][2] = {{"MSC_VITA_VGL_SKIP", "1"}, {"MSC_VGX_UNIDUP", "2"}, {"MSC_VGX_PROMOSMALL", "3"},
                                                {"MSC_SKIN_GPU", "1"}, {"MSC_VGX_SOFTINST", "16"}, {"MSC_VGX_BLENDNODISCARD", "1"}, {"MSC_VGX_CELLCLEAR", "1"},
                                                // int28: MSC_VGX_CROWDTRIM left at code default 0 (real-Vita 6413/6415/6423: turning it off saves 8+ ms Core 1 CPU in walk-outs & ~5 ms in live play with negligible GPU delta and avoids PROJ artifacts).
                                                // wave 12 (be25): the uniform-block equality test with NEON instead of newlib memcmp, same answer for any contents (host: mode 2 compares both, 0 disagreed over 1800 pictures, mutant caught; qemu-arm NEON test). Kill switch =0 (kit run fastcmp0).
                                                {"MSC_VGX_FASTCMP", "1"}, {"MSC_VGX_UNICMP", "1"},
                                                // wave 13 (render13 R5): RtScan sizes rt.live / rt.nextBreak without the fill their backward loops overwrite (host proof: BEHASH and det equal, verify mode 2 poisons and finds 0 unwritten, mutant caught).
                                                {"MSC_VGX_RTSCAN_NOFILL", "1"},
                                                // wave 9 (physpar): the ten characters' poses of each simulation step on the job lanes, same bits (host proof: det trace and every frame hash equal over a whole match60 and The Dump, verify mode and mutants in docs/TECHNICAL.md). Kill switch MSC_PHYS_POSE=0 (kit run physpose0).
                                                {"MSC_PHYS_POSE", "1"},
                                                // wave 13 (cpuexact13), all exact (same answers, fewer instructions); kill switches =0 (kit run cpuexact0):
                                                // MSC_TLS_FAST: the Vita's emulated thread-local lookup read from the TLS slot (np_fasttls.cpp; self-checked at start, forwards to the library when the check fails);
                                                // MSC_TICKS_FAST: OSGetTick/OSGetTime without two 64-bit divides (np_ticks_fast.h; every ns below 2^50 proved equal);
                                                // MSC_FPBITS: the hottest pose-code float compares as integer tests on the bits (np/fpbits.h; all 2^32 patterns proved equal).
                                                {"MSC_TLS_FAST", "1"}, {"MSC_TICKS_FAST", "1"}, {"MSC_FPBITS", "1"},
                                                // gxp17: MSC_TASK_EXEC_SKIP: the task runner no longer reads the clock for mExecutionTime (written, never read; about 18 clock reads a picture). =0: as before.
                                                {"MSC_TASK_EXEC_SKIP", "1"},
                                                // gxp17: the three cpu22 exact cuts (np/cpu22.h; written in wave 12 with a verify mode and left out of the plan until a console verify run): the shadow view's inverse made at its first
                                                // read, the 150 power-up drawable lookups kept per stadium state, one ring walk in GetReplayExcitement. Host proof (det trace, back-end hashes, verify mode 2 with 0 mismatches,
                                                // mutants caught) on The Dump and Vice goal routes in the gxp17 note. =0 each: as before.
                                                {"MSC_CPU22_INVLAZY", "1"}, {"MSC_CPU22_POWERUP", "1"}, {"MSC_CPU22_REPLAY", "1"},
                                                // cpu24 (wave 24, np/cpu24.h; w8/cpu24.md): three exact game-thread cuts. PART: EvaluateParticleColour without the double floor() call, the second divide and the scale step at 1.0f
                                                // (unit-tested bit for bit on x86 and the VitaSDK compiler under qemu, 714 k colours verified on a host route, det trace and frame hashes equal). TIGHTSTAT: the diagnostic sums of
                                                // NpFxTightParticle only while MSC_FX_TIGHT_STAT is on. NOTIFY: a mesh writer's three or four vertex streams are reported to the renderer as one written range, 38 % fewer
                                                // np_gx_notify_write rounds a picture (host); a notification only invalidates, so the span is safe (the host glnull cannot show a missing notification: its mutant is invisible there).
                                                // =0 each: as before.
                                                {"MSC_CPU24_PART", "1"}, {"MSC_CPU24_TIGHTSTAT", "1"}, {"MSC_CPU24_NOTIFY", "1"},
                                                // 1.3 defaults, emulator-checked; none timed on a console yet (tools/console-1.2 d1 and the fstrim1, vstrim1, upcombined, floorbake0, floorsheen0, pipeheavy0, gxmdirect runs).
                                                // MSC_VGX_FS_TRIM / VS_TRIM need the 1.3 shader pack: without it each form is a cold compile that keeps heap (docs/TECHNICAL.md).
                                                {"MSC_VGX_FS_TRIM", "1"}, {"MSC_VGX_VS_TRIM", "1"},
                                                // (shader pack 16 holds these forms and the plain ones: =0 on either costs no compile)
                                                {"MSC_VGX_TEV_CANON", "2"}, {"MSC_VGX_FINCLAMP", "3"},
                                                // int32 (real-Vita 6474d: Vice 55.7 -> 58.2 eff_fps, 92.4% -> 97.0% @ 60 Hz, -1.03 ms GPU busy, 0 drops, zero visual change; pre-baked in pack31+):
                                                // non-projective vec2 texcoords, omit unused v_col0 varyings, and hardware texture2DProj with safe z==0 guard.
                                                {"MSC_VGX_TEX2D", "1"}, {"MSC_VGX_LEANVARY", "1"}, {"MSC_VGX_FS_PROJ", "1"},
                                                // wave 13 shtext13 (G5): vertex text without data-dependent `if` (normalise guards as n / (l + step(l, 0.0)), palette row clamps as min() before the cast): bit-identical on Mesa
                                                // (renderer/tests/run_shader_text_test.sh: 124 changed texts of the pack list, 20.7 M output words by bit pattern, mutants caught). Changes 124 of the list's 137 vertex texts: each is a pack
                                                // miss until the pack is re-recorded (env_player.sh has it for the recording). Bit-exact on Mesa; on the SGX "imperceptible" (Sony's compiler may round a normal by a last bit; positions unaffected). Kill switch =0 (the old texts, which the old pack holds).
                                                {"MSC_VGX_VS_NOBRANCH", "1"},
                                                {"MSC_VGX_LODBIAS_RUNG", "60"}, {"MSC_VITA_SHARPEN_SCALE", "1.0"}, {"MSC_VITA_ANISO_MIN", "128"},   // render-size aware picture (docs/TECHNICAL.md); Galaxy's console runs: bias at 60 %, small textures out of anisotropic filtering, both free
                                                {"MSC_VITA_UPSCALE", "combined"},   // SMOOTH's look from 3 reads instead of 6 (Galaxy measured SMOOTH's kind at 8 ms)
                                                // look (11 Nov, host pictures, w6/look.md): k = 1.0 stays (0.6 / 1.0 / 1.4 differ by less than 0.005 SSIM; the edge term already sits on the sharpness-against-ringing frontier),
                                                // MSC_VITA_SHAPE stays off (at equal pixels the game's shape beats the 16:9 shape in SSIM at 0.97 / 0.84 / 0.70 of the ladder: +0.011 / +0.006 / +0.006),
                                                // and MSAA 2x below the game's own size is not added (at equal cost the next size up wins on fidelity; only MSAA cleans stair edges).
                                                {"MSC_SCORE_NODEPTH", "1"},
                                                {"MSC_VGX_GXM_DIRECT", "1"},
                                                // int28: MSC_SHADOW_EVERY left at code default 1 (every-frame shadows in walk-outs/replays; 6423 proved 1 + RES=144 runs 29.5 fps walk-out & 30.0 fps replays while eliminating DK fly-in shadow lag).
                                                {"MSC_CROWD_REPLAY", "3"},
                                                // (cf18, 23 Nov console, s37 at 500 MHz) MSC_CROWD_REPLAY's floor only works with the cluster flag (the 1.3 constructor-order bug): walk-cf0/cf1 and cf0d/cf1d, cut-scene GPU p95
                                                // weighted 50.7 -> 45.5 ms (Vice) and 50.6 -> 47.2 ms (The Dump), late cut-scene pictures 51.9 -> 36.3 % and 37.8 -> 35.8 %. Far crowd rows refresh every 2nd picture. Kill switch =0.
                                                {"MSC_CROWD_CLUSTERFLAG", "1"},
                                                // (mv18, 23 Nov console) THP planes as one byte a texel (bytes equal the RGBA decode: run_i8churn_test.sh, 7 mutants; needs vitaGL 0053, in the build): Vice stadium preview
                                                // new pictures 323 -> 371, repeated 82 -> 38, late gaps 79 -> 36 of ~400 calls. MSC_THP_DELAY=2 made it worse (repeated 106): not used. Kill switch =0.
                                                {"MSC_VGX_I8CHURN", "1"},
                                                // (c3def18, 23 Nov console, s41 at 500 MHz, CapUnlocker) The fourth core where the probe proves it: audio output (rules: not the mixer, never the log writer or disc
                                                // workers, nothing while online), a parked job lane (WORK bit 1) and the front end's decode worker (bit 2). Vice tour against c3w-off: late gameplay pictures 13.0 -> 2.2 %
                                                // (lane) / 4.1 % (worker); 60 fps pictures 4.0 -> 13.4 % (worker); game work a 1-step picture 14.0 -> 7.2 ms in steady windows (worker). The watch never moved a thread,
                                                // every run ended clean. Without the plug-in the probe answers "not available" and nothing changes. Launch marker (c3g18) and watch (pace18) stay on. Kill switch MSC_VITA_CORE3=0.
                                                {"MSC_VITA_CORE3", "1"}, {"MSC_VITA_CORE3_WORK", "3"},
                                                {"MSC_FLOOR_BAKE", "7,8"}, {"MSC_FLOOR_SHEEN", "vertex"},   // Lava Pit, Pipeline Central. Not Wastelands: its cracks lose their glow
                                                {"MSC_STADIUM_HEAVY", "8"}, {"MSC_STADIUM_HEAVY_CROWD", "0"},
                                                // (dumpgpu25, 25 Nov; owner: invisible whole-game wins default ON) The Dump's rain drawn as hull quads: host pictures identical (max diff 1/255,
                                                // PSNR 94-99), about -2.5 ms GPU ESTIMATE (1.43 of 6.17 fetch-screens a play picture), det equal. Kill switch MSC_STADIUM_CUT=0; console A/B dg25-hull.
                                                // (ship26 + int28: bare mask 21336 = 8 FARDECOR + 16 WORLDNPC + 64 DUMPFX + 256 MAT1 + 512 MAT2 + 4096 MATBILIN + 16384 RAINHULL; 256+512 Dump sheen shader pre-baked in pack28; MSC_STADIUM_CUT_FAR=32)
                                                {"MSC_STADIUM_CUT", "21336"},
                                                // wave 9 simthread: snapshot grab/blend and crowd sprite views on the job pool's lanes (bit-identical on the host: frame hashes and det
                                                // trace over 30000 pictures at Vice and The Dump's route); =0 is the kill switch, kit A/B snappar0
                                                {"MSC_SNAP_PAR", "1"}, {"MSC_CROWD_POS_PAR", "1"},
                                                // wave 11 gt16a: the snapshot slot takes the pose without the previous node matrices nothing reads (render and replay data only; host: det trace, replay
                                                // byte hash and frame hashes identical, poison and nudge mutants caught); =0 is the kill switch. MSC_RENDER_NEON is OFF (code default 0): see TECHNICAL.md
                                                {"MSC_SNAP_ONECOPY", "1"},   // (proofs16: exact on match60 and The Dump, nudge mutant m3/d3 caught on both, seed break caught)
                                                // sweep24 (24 Nov): software prefetch hints, bits 1 2 4 8 16 (BuildNodeMatrices, DrawableCharacter::Blend, the recorder's write pointer, ShaderSkinMesh::Pose,
                                                // NpSim26Apply). A pld changes no value and never faults: det trace 4687 lines md5 7f8ee01a5edb and frame-hash csv equal to the plain run on the Vice tour (host glnull, seed
                                                // 1234abcd; the seed break differs); each address is a bounded read of data the code reads next. No memory. Not timed on a console yet: kit line sweep24 pf0 is the OFF side. =0 turns it off.
                                                {"MSC_PREFETCH", "31"}};
                                                // wave 10 (simthread10) MSC_RENDER_RNG / MSC_FRAMELOCK_STEP / MSC_BLEND_STEP: OFF in r15 (Fable batch 2: BLEND_STEP changes 3180 of 7004 frame hashes,
                                                // FRAMELOCK fired once in all runs, the perturbed-render proof diverges at Vice step 3560). Nothing in r15 needs them while bg30 is off. Kit run renderrng1.
                                                // (MSC_VIS_STEP stays off by default: it changes which players the AI counts as on screen, by a few flags in a thousand; bg30 / a simulation thread turn it on)
        char line[2048];
        size_t n = 0;
        for (const auto& s : kSpeed) {
            if (Has(s[0])) continue;
            setenv(s[0], s[1], 0);
            RememberDefault(s[0]);
            const int w = std::snprintf(line + n, sizeof(line) - n, "%s%s=%s", n ? " " : "", s[0], s[1]);
            if (w > 0 && n + static_cast<size_t>(w) < sizeof(line)) n += static_cast<size_t>(w);
        }
        if (n) std::fprintf(stderr, "[plan] speed switches on by default (=0 in env.txt / np-player.txt turns one off): %s\n", line);
    }
    // 1.2: whatever helps performance and graphics is on by default, the best experience for someone who does not dive into the
    // settings; if testing finds a major issue, a default is changed back afterwards. The picture and timing items of 1.2 that were switches (off until the console A/B) are on in every
    // Vita build unless env.txt / np-player.txt names the key. The console A/B turns each one OFF (tools/console-1.2). The host harness does not run this file: its
    // defaults stay as they were, so its picture comparisons keep their meaning.
    //   MSC_DYNRES_FG=1            Prioritize Players: the ladder softens the stadium and crowd (a crowd refresh step) before the whole scene shrinks; only acts when the GPU is short
    //   MSC_VGX_SHQUAD=1           the shadow-volume blend quad is cut to the pixels it can change (exact: pictures equal on the host); up to 6 MiB of CPU copies of small static buffers
    //   MSC_VGX_SHQUAD_LIVE=1 + MSC_VGX_LEDGERPAL=1   the same quad is cut in open play too (flat shadows: the ellipses go through the matrix palette, which the ledger now measures); exact, ~86% of the quad's pixels at The Dump (host)
    //   MSC_FX_GLOW_DEAD=1         (gpu25) the glow chain of walk-outs and cut-scenes does not draw its last level when the composite does not read it (the game's tweak index is 5 there): one scene, one copy and a half-screen pass less per picture; exact (host: 172 Dump pictures identical); estimate 1.1 to 2 ms of graphics chip and ~1.5 ms of back end per glow picture
    //   MSC_HELD_SIZE_DOWN=1       (held30, console 18 Nov: the 30 path drew walk-outs and cut-scenes at 784x544 1x and ran 35 ms where the hold drew 960x544 and ran 47) with the full picture policy (Quality), replays, cut-scenes and walk-outs are drawn at the hold ladder's bottom kept level, 864x488 with one sample (the Quality floor, 81 % of the pixels); live play and Mega Strikes unchanged
    //   MSC_HELD_GLOW_HALF=1       (held30, owner 17 Nov 23:23 "small sacrifices for a big improvement") in walk-outs, cut-scenes and replays the glow chain's six blur passes (six scenes of the graphics chip) are redrawn every other picture; the composite reads the previous picture's chain; players, ball, HUD untouched; the first 6 pictures of a held scene are drawn in full; host: Vice cut-scene / replay pictures identical, walk-out haze edge differs by up to 77/255 on 4-8 % of the pixels on the skipped pictures; estimate -3 ms graphics chip, -4 ms back end, -1 ms front end a picture on average (a skipped picture saves 6.6 / 9 / 2)
    //   MSC_FS_HELD=1              (rep27, console 18 Nov: in replays the game thread waited at the send join, p50 37-40 ms a picture, while core 1 ran the send worker AND the front end, 86 % of a 47 ms picture) a replay's frame is sent on the game thread (no simulation step in a replay: core 0 has room), not on the send worker beside the front end; the same GX stream in the same order (the plain send the loading frames take); 2 = also cut-scenes and the walk-out
    //   MSC_HELD_GOV=1             (scenes30, console 23 Nov s37/s41: walk-outs, celebrations, Mega Strike cut-scenes and replays 26-57 % of pictures shown 3 refreshes, GPU 40-50 ms of 33.3) a held scene whose
    //                              GPU time stays over 33.3 ms takes, one at a time: the crowd refresh floor 2 -> 3, shadows every 3rd picture, the glow chain every 3rd picture; given back when there is room;
    //                              nothing changes while a scene makes 30 (host: pictures and frame stats identical to =0 at level 0; det trace equal at every level); VISIBLE when engaged; kill switch =0
    //   MSC_FX_GOV_HOLD=60         (scenes30) the fill governor keeps its sprite thinning through 60 frames without large sprites instead of letting go at the first one (the console's 0 -> 256 -> 512 -> 0
    //                              flap in every walk-out, celebration and replay, GPU unchanged); a kept level thins nothing while no large sprites are there; kill switch =0
    //   MSC_DOF=lazy               the depth-of-field capture is made only on frames that draw the blur quad (same picture); MSC_DOF=1 is the game's own, MSC_DOF=0 no blur at all
    //   MSC_VITA_QUEUE_FIXED30=1   GPU queue depth stays 2 while the frame rate is held at 30 (the Galaxy port's fixed-depth fix)
    //   MSC_VITA_PRIO_VBLANK=0x70         the vi-retrace thread runs ahead of the audio threads (abs 112; 0 = the timer role's priority as before)
    //   MSC_VITA_CORE0_ALONE=1     library threads that may run on core 0 are moved to cores 1-2: the game thread has core 0 alone
    //   MSC_PACE_VBLANK=1          animation pacing counts the vblanks the display really showed
    //   MSC_VITA_ANISO=3           anisotropic minification for trilinear textures: the far half of the pitch stays sharp
    //   MSC_SLIP=20                bounded slip: a late frame advances the game by at most 20 % less than it took; single player only (off in a LAN or online session by itself)
    //   MSC_SLIP_BUDGET_MS=30      (wave 9) ... but only from a bucket of 30 ms that fills at 2 % of real time: a single hitch is cut as before, a sustained overload runs the game at 98 % or better instead of 83 % (np_slip.h)
    //   MSC_SLIP_REFILL_PCT=0.5    (pacing12) the bucket fills at 0.5 % (was the default 2 %) of real time: with MSC_CLOCK=slip a sustained overload loses at most that share; MSC_CLOCK=debt (the default) does not use the bucket
    //   MSC_VGX_SOFTXFORM=256      small rigid meshes up to 256 vertices (the 1.1 limit was 128) are transformed on the CPU and merged: 4.4 % fewer draw calls, rounding-class
    //                              pixel changes. (MSC_VGX_SOFTXFORM=0 is no CPU transform at all, not the old value: 128 is.)
    //   MSC_DYNRES_UNDO=1          dynamic resolution: a step down in any scene is judged on the GPU's mean busy time over 30 frames and undone when it saved less than half of what the model expects
    //   MSC_DYNRES_SEVERE=1        dynamic resolution: the GPU at 1.8 times the budget for 3 s in play opens a deeper floor (0.70 per side) until the scene fits at the ordinary one (Galaxy port's measured drop-in fix)
    //   MSC_VGX_CULL=1             a resident triangle draw wholly outside the scissor of its own state is not recorded (exact: renderer/src/vgx_cull.h; on the Pipeline Central route 27-31 of 172
    //                              such draws a frame in the emulator's census, 12 % of their triangles; the Galaxy port's measured GXV_STATICCULL)
    //   MSC_VGX_VGL_FRAGMEMO=1     vitaGL 0018: a program keeps what the shader patcher answered for a blend function (the same pointers)
    //   MSC_VGX_VGL_UNIREUSE=1     vitaGL 0018: a program whose uniforms did not change since its last copy in this scene names that copy again (the same bytes)
    //   MSC_VGX_UNICMP=1           be26: a uniform block is compared with the place of its uniforms in vitaGL's buffer, not with a second copy of the values (one copy to read per draw; emulator verify run =2: 0 disagreements; console A/B: unicmp0)
    //   MSC_VGX_VGL_UNIHIST=1      vitaGL 0046 (be26): also any of the scene's last four copies of a program's uniform buffer with the same bytes is named again (host model test, emulator verify run; console A/B: unihist0)
    //   MSC_VGX_COPYINPLACE=1      the shadow volumes' full-screen alpha copy is not made: the EFB's scene still ends there, and the quad that reads the copy reads the EFB's colour
    //                              buffer (whose alpha nothing writes until that quad is drawn) in the next one. One 960x544 copy scene fewer in every 3D frame; exact by construction
    //                              (renderer/src/vgx_backend_gl.cpp, InplaceScan), not timed on a console [console A/B: copyinplace0]
    //   MSC_FPS_LADDERHOLD=1       AUTO frame rate: a GPU-limited miss at 60 waits (at most 8 s) while the dynamic resolution has picture steps left; a CPU-limited one still goes to 30 at once
    //   MSC_VGX_MSAA=4             anti-aliasing as a rung of the dynamic-resolution ladder (4x, then 2x, at the game's own size, in place of the supersampled sizes above it): the ladder
    //                              only climbs there after a full second of GPU room, so nothing is paid when there is none. Not set with a fixed picture size (MSC_VITA_RENDER: the
    //                              samples would be on all the time), with the dynamic resolution off, or with the older start-up multisampling (MSC_VITA_MSAA).
    //   MSC_FX_LOWRES_MAXH=224     the effects pass (Mega glow, heavy-stadium haze) keeps its game-size pixel count when the picture is larger: host pictures differ only in
    //                              Mega frames, at most ~800 pixels at the glow edge, indistinguishable side by side (fx report, 6 Nov). Estimate -0.6..-1.7 ms in Mega at 960.
    //   MSC_DYNRES_LATEHOLD=2000   at 60 only (MSC_DYNRES_LATE30 off): one frame over the GPU budget steps the picture down 2 rungs at once and holds it 2 s; host model: late
    //                              frames at 60 0.51 % -> 0.21 % for ~7 % fewer pixels; no effect at 30 (borrow report, 6 Nov). Console A/B: latehold60.
    // (13 Nov 2026: MSC_VGX_ATLASJOIN and MSC_VGX_SHADOWDOWN are on: open play 6 to 4 scenes, pictures in w7/scenes2.md)
    // Kept off on purpose: MSC_VGX_GXMCACHE (emulator aborts under investigation), MSC_VGX_FBO_SAMPLES, MSC_REDUCED_MOTION (a comfort choice), MSC_LAN_INPUT_DELAY (changes
    // multiplayer feel), the crowd, clear-cut and skin-lane options still being built.
    {
        static const char* const kQuality[][2] = {{"MSC_DYNRES_FG", "1"}, {"MSC_VGX_SHQUAD", "1"}, {"MSC_VGX_SHQUAD_LIVE", "2"}, {"MSC_VGX_LEDGERPAL", "1"}, {"MSC_FX_GLOW_DEAD", "1"}, {"MSC_HELD_GLOW_HALF", "1"}, {"MSC_HELD_SIZE_DOWN", "1"}, {"MSC_HELD_GOV", "1"}, {"MSC_FX_GOV_HOLD", "60"}, {"MSC_HELD_GOV_SIZE", "0"}, {"MSC_DOF", "0"}, {"MSC_VITA_QUEUE_FIXED30", "1"},
                                                  {"MSC_VITA_PRIO_VBLANK", "0x70"}, {"MSC_VITA_CORE0_ALONE", "1"}, {"MSC_PACE_VBLANK", "1"},
                                                  {"MSC_VITA_ANISO", "2"}, {"MSC_SLIP", "20"}, {"MSC_VGX_SOFTXFORM", "256"},
                                                  {"MSC_DYNRES_UNDO", "1"}, {"MSC_DYNRES_SEVERE", "1"},
                                                  {"MSC_VGX_CULL", "1"}, {"MSC_VGX_VGL_FRAGMEMO", "1"}, {"MSC_VGX_VGL_UNIREUSE", "1"}, {"MSC_VGX_VGL_UNIHIST", "1"},
                                                  {"MSC_FPS_LADDERHOLD", "1"}, {"MSC_FX_LOWRES_MAXH", "224"}, {"MSC_FX_COMPOSITE", "1"}, {"MSC_FX_MENU", "0"}, {"MSC_FX_CELEB_MINPX", "900000"}, {"MSC_MEGA_SHARP", "1"}, {"MSC_MEGA_BALLS", "1"}, {"MSC_MEGA_FX_MAXDIV", "4"}, {"MSC_BALL_FINS", "2"}, {"MSC_PICTURE_HOLD_MSAA_HELD", "0"}, {"MSC_HELD_AA_AHEAD", "1"}, {"MSC_AA_POLICY", "auto"}, {"MSC_VGX_COPYINPLACE", "1"},   // owner 14 Nov 16:55: replays and stadium entrances at 30 without anti-aliasing beat them slow with it: one sample in every stadium's held scenes (r15: auto = 3, 4, 15); int34/int34b: crisp menu giant ball (FX_MENU=0), soft anti-aliased ball spikes (BALL_FINS=2), Mega Save locked 960x544 & Mega cut-scenes under MSC_HELD_GOV (MEGA_SHARP=1, real-Vita A/B 6512 vs 6513, MEGA_FX_MAXDIV=4)
                                                  {"MSC_DYNRES_LATEHOLD", "2000"}, {"MSC_VGX_PASSDOWN", "1"}, {"MSC_VGX_PASSDOWN_TWOQUADS", "1"}, {"MSC_VGX_ATLASJOIN", "1"}, {"MSC_VGX_SHADOWDOWN", "1"}, {"MSC_VGX_ATLATE", "1"}, {"MSC_VGX_DISCARD_BLEND", "1"},   // (13 Nov, w8 crowd2b, host only: alpha-tested draws behind opaque ones, pictures identical; celebration crowd near to far, rim pixels only; not timed on a console: runs atlate0, crowdftb0)
                                                  // smooth (11 Nov, w6/smooth.md): nothing is built or let go in play that could have been done at start-up or kept
                                                  {"MSC_PIC_PREWARM", "1"}, {"MSC_POOL_KEEP", "18000"}, {"MSC_POOL_TRIM_ONE", "1"},
                                                  {"MSC_VGX_HOUSEKEEP_BACKOFF", "1"},   // timing only: a program or texture trim pass that deleted nothing waits before it scans again (Galaxy: 1.5 ms a picture)
                                                  {"MSC_HITCH_MS", "25"},
                                                  // stall10: the frames the game throws away on purpose (end of loading, walk-out wait) are not paced at one retrace each; not in a LAN / online / recorded game
                                                  {"MSC_DISCARD_FAST", "1"},
                                                  // front end (fe60): identical pictures and det trace on the host (docs/TECHNICAL.md); console A/B is tools/console-1.2 run fe60off
                                                  {"MSC_VGX_SKINDIRECT", "1"}, {"MSC_VGX_SKINMEMO", "1"}, {"MSC_VGX_BUILDFAST", "1"}, {"MSC_VGX_FASTIDX", "1"},
                                                  // front end (fe8, w8/fe8.md): the key extras (light facts, wide TEV registers) kept while nothing they read was written; display-list hits validated by a watch epoch of the pages they use. Frame hashes and det trace identical on the host; console A/B is tools/console-1.2 run fe8off
                                                  {"MSC_VGX_EXTMEMO", "1"}, {"MSC_VGX_DLWATCH", "1"}, {"MSC_VGX_LEDGERFAST", "1"}, {"MSC_VGX_CROWDTRIM_PLAY", "0"},
                                                  // febe24 (w8/febe24.md): a crowd-trim cell set by its rectangle (exact by disjointness: BEHASH, det trace and frame hashes identical over 2800 pictures, verify 35,359 sets 0 differ, mutant caught); console A/B is tools/console-1.2 run ctrect0
                                                  {"MSC_VGX_CROWDTRIM_RECT", "1"},
                                                  // gpueff18 (w8/gpueff18.md, host 23 Nov): the crowd trim never cut a quad in a held scene: welded figure lists were refused ("range"), every held-scene crowd quad is a little bent ("parallelogram"), and Vice's held crowd uses q = 1 projective texgen. WELD measures
                                                  // a welded draw over the vertices its indices name (boxes checked: 0 of 2986 pictures with a texel outside), SKEW cuts the bent quads on the triangle each cut corner lies in, PROJ (wave 7) takes q = 1. Celebration crowd fragments -36 to -45 %
                                                  // (Dump, Vice); pictures differ in single isolated pixels only (PSNR 51-58 dB worst picture, 0.01 mean level). Console pairs ge-trim0-d / ge-trim0-v against scn-on-d / scn-on-v; kill switches =0.
                                                  {"MSC_VGX_CROWDTRIM_WELD", "1"}, {"MSC_VGX_CROWDTRIM_SKEW", "1"}, {"MSC_VGX_CROWDTRIM_PROJ", "1"},
                                                  // gpu24 (w8/gpu24.md, FIX-11): the display's scene (the present, one full-target mask quad every picture) begins with the mask set too (vitaGL 0029 bit 1; the framebuffer-object scenes, bit 0,
                                                  // are on by default since r11 and ran on the console). Exact by the host model test (run_vitagl_maskkeep_test.sh, all modes); armed only for a scene that loads nothing with the scissor off.
                                                  // Console A/B: kit run maskbg1 (the old value 1) against any default run; the [rt] scissor mask background line must show scenes begun with the mask set one more per picture.
                                                  {"MSC_VGX_VGL_MASKBG", "3"},
                                                  // wave 24 (Gemini review 2B; be24/be26): copies into vitaGL's uncached pools (stream buffers and the uniform ring) by the fastest of the
                                                  // byte-exact variants a start-up benchmark picks (every variant is read back and compared before use; 1.2 x the plain copy or it stays plain).
                                                  // Console: issue 19.0 -> 14.6 us a draw (17 Nov, be26); wave 24 job 6038 Vice: 43.8 fps, no fall, no fault. =0 is the plain copy; kit wrcopy0.
                                                  {"MSC_VITA_WRCOPY", "auto"},
                                                  // recorder (recorder9, w8/recorder.md): register writes and fog set-up made cheaper; the recorded stream is the same bytes (MSC_REC_HASH identical over 5000 pictures at two stadiums); console A/B is tools/console-1.2 run recoff
                                                  {"MSC_REC_FAST", "1"},
                                                  // cadence (w7, w7/cadence.md): policy and draw side only, det trace identical on the host; console A/B is tools/console-1.2 run cadenceoff
                                                  {"MSC_CADENCE", "1"}, {"MSC_PRESENT_SYNC_START", "1"}, {"MSC_FX_ATTACH", "1"},
                                                  // wave 7 follow-ups (w7/cadence.md): a held scene is drawn at the game's own size (its target must be kept already); display-list entries unused for 600 frames stay while they add up to 24 MiB (measured 19 MiB: 11 heap + 8 static buffers)
                                                  {"MSC_CADENCE_FULL", "1"}, {"MSC_DL_KEEP", "24"},
                                                  // polish (w7, w7/polish.md): the sharpening eases over 20 pictures at a size step; an HD texture that arrives late is sampled one level coarser and eases in over 8
                                                  {"MSC_SHARPEN_EASE", "20"}, {"MSC_HDTEX_FADE", "8"}, {"MSC_STILL_TOL", "1"},
                                                  // crowd (w7, w7/crowd.md): the same figure lists, det trace and 4005 frame hashes identical on the host; console A/B is tools/console-1.2 run crowdvisoff
                                                  {"MSC_CROWD_VISFAST", "1"},
                                                  // game-thread cost of the crowd (wave 10, w8/gt10.md; host proof: det trace and all 5004 frame hashes of match60 identical to MSC_CROWD_GT=0, the break mutant changes 957; counts are host estimates):
                                                  // a crowd sprite's view matrices are made where the sprite is captured (bit 1), none for a captured sprite with no figures (bit 4), and in int26 (console 6241/6273, render-only, no RNG/sim change) one pose per texture (bit 2, ~1 ms Core 0)
                                                  {"MSC_CROWD_GT", "7"},
                                                  // crowd and decorative NPC poses (wave 12, w8/crowd24.md; host proof on Vice, The Dump and both goal routes: det trace and every frame hash identical to both switches off, with the job lanes and with MSC_JOBS=1,
                                                  // the break mutants MSC_POSE_JOBS_BREAK=1 change 965-1617 hashes): the crowd sprites' poses (animation evaluate, node and skin matrices, and the sprite views) are made on the job lanes before the capture loop (bit 1);
                                                  // bit 2 (the drawn NPCs' node and skin pose on the lanes) is exact too but 8 models are ~20 k instructions, less than a fork costs on the console (77 us wake): left off. An NPC the render filter hides is not evaluated
                                                  {"MSC_POSE_JOBS", "1"}, {"MSC_NPC_EVAL_LAZY", "1"},
                                                  // crowd25 (w8/crowd25.md; owner 17 Nov 22:19 / 22:23: cheaper crowd animation is fine when needed, down to the bare minimum if it is subtle and seamless): the adaptive crowd-cost governor (np_crowdgov.h). In live play, while the game thread is the limiting lane and over 87 % of the period, the crowd sprites refresh less often by size class (far first, near last, staggered by sprite, six levels) and come back when there is room. Render side only; host: det trace identical, crops in the note. Kit run crowdgov0 turns it off.
                                                  {"MSC_CROWD_GOV", "1"},
                                                  // game thread, exact (wave 11, w8/gt16b.md; host proof on match60, The Dump and Lava Pit with items: det trace and all 5004 frame hashes identical to both switches off, the mutants (MSC_ODE_BPFULL_BREAK=1 and 2,
                                                  // MSC_AI_AVOID_BREAK=1 and 2) change the trace, the verify modes count wrong = 0): the broad phase skips the pairs collideAABBs returns from first (bit-exact integer tests) and steps over a character's bone geoms
                                                  // in runs by one test of the run's union box (2); AvoidController::Update runs the pure tests before the AVL lookup (1) and rejects a pair surely out of range before the emulated square root (2). Same results, fewer cycles
                                                  {"MSC_ODE_BPFULL", "2"}, {"MSC_AI_AVOID", "3"},
                                                  // sim26 (wave 12, w8/sim26.md; host proof on the Vice tour, The Dump, both goal routes and the Mega Strike route with the job lanes
                                                  // (NP_PARALLEL host build, MSC_JOBS=3): det trace (seed 1234abcd) and every frame hash identical to all five off, the verify modes
                                                  // count wrong 0, the mutants change the trace / the pictures, seed 1234abce differs): the broad phase's bone-run rows walk a candidate
                                                  // list (BP), UpdatePose's arithmetic runs on the pose lanes and pass 3 only checks and writes (BONES), the lanes claim the goalies'
                                                  // poses first (POSEORDER), the nets' load-time indices / texcoords are not recopied every step (NETGRAB), the islands of a step run
                                                  // on the lanes with the geom notifications made after the join in the old order (ISLANDS). Kit runs sim26off-v / sim26off-d, sim26ver.
                                                  // gt27 MSC_BLEND_ASYNC (console 18 Nov, int8 A/B 2 pairs, Smooth 60 Vice, [gsplit-pic] 1-step pictures: p50 15.1-16.1 vs 16.0-17.2 ms, p95 17-18 vs 19-24 ms): the live-play blend on the helper lane. MSC_FS_HELD left OFF (replays not faster on the console).
                                                  {"MSC_BLEND_ASYNC", "1"}, {"MSC_SIM26_BP", "1"}, {"MSC_SIM26_BONES", "1"}, {"MSC_SIM26_POSEORDER", "1"}, {"MSC_SIM26_NETGRAB", "1"}, {"MSC_SIM26_ISLANDS", "1"},
                                                  // gt27 (w8/gt27.md): the fork's packet refresh prefetches the next records' cold sources (hints only; host proof in the pipelined send run serially:
                                                  // frame hashes identical, repeatable base; kit run refreshpf0 is the OFF side)
                                                  {"MSC_FS_REFRESH_PREFETCH", "1"},
                                                  // wave 12 ai27 (w8/ai27.md; host32 proofs: det trace seed 1234abcd and every frame hash identical to all four off on the tour, The Dump, the Vice and Dump goal routes and a Mega route, verify modes wrong 0,
                                                  // a mutant per switch changes the trace, seed 1234abce differs): the closest-player lists that are already in order skip qsort (SORT), FuzzyVariantCollection::operator= reads the slot pointers
                                                  // itself (VCOPY), AvoidController::Update asks IsAvoiding once per kind (AVOID), UserControlledT stops at the first pad of the side (CTRL). Kit runs ai27off-v / ai27off-d.
                                                  {"MSC_AI27_SORT", "1"}, {"MSC_AI27_VCOPY", "1"}, {"MSC_AI27_AVOID", "1"}, {"MSC_AI27_CTRL", "1"},
                                                  // phys27 (wave 12, w8/phys27.md; host proof as sim26: det trace and frame hashes identical to both off on five routes, verify modes wrong 0, mutants and seed 1234abce
                                                  // change the trace / pictures): the narrow-phase callback is not run for a pair dCollide surely answers with no contact (no collider for the classes, e.g. plane-plane; a plane
                                                  // and a transformed sphere / box / capsule whose AABB is clear of the plane) (PLANE); the blend weight w / acc is not divided when nothing was accumulated before, it is exactly 1 (DIV).
                                                  {"MSC_PHYS27_PLANE", "1"}, {"MSC_PHYS27_DIV", "1"},
                                                  // gt18 (w8/gt18.md; exact: det trace seed 1234abcd and every frame hash identical to off on the Vice and The Dump fast-boot routes to their end, verify 25 M calls 0 mismatches, mutant
                                                  // caught, unit test 15 M quaternions 0 differences; host callgrind -5.4 % instructions): nlQuatNLerp / nlQuatRenormalize without the calls into the SDK maths, and no shared counters in
                                                  // NpWeightT. Kit runs gt18off-v / gt18off-d are the OFF side, ab-gt18-v the in-run A/B.
                                                  {"MSC_GT18_QUAT", "1"},
                                                  // no loading in play (w8/noload.md): the cut-scene, animation and defence-HUD files a match reads after kick-off are kept in up to 8 MiB of the C heap (a line-up needs about 8.1; given back when the heap is short), read ahead while the match loads
                                                  {"MSC_DVD_KEEP_KIB", "8192"},
                                                  // the textures a match uses are shown to the renderer while it loads and kept against the texture budget until the match is over, so none is uploaded in play (w8/noload.md)
                                                  {"MSC_TEX_PRETOUCH", "2"},
                                                  // (OFF in the plan until fixed, w10 cuts, w8/cuts10.md; kit runs warm1, cutsshapes1, scenepin1 are the ON side) no first uses at a cut: the other sample mode's program variants are drawn at load (MSC_FXAA_WARM, the r9 boot crash is fixed: the small target is kept) and the pass objects an earlier session made after kick-off (cut-scene, replay and celebration chains) are made again before kick-off (MSC_CUTS_SHAPES; the card's <list>.shapes, the package's vgx_passshapes.txt). Kill switches =0; console A/B: kit runs warm0, cutsshapes0
                                                  
                                                  // lessons from the Galaxy console runs (w8/lessons.md): nothing is trimmed and made again in a loop, the warm-up queue puts a match's own programs first, and a late frame
                                                  // advances the game by at most two 20 ms steps (fewer long picture holds; single player only, with MSC_SLIP)
                                                  {"MSC_POOL_THRASH_GUARD", "1"}, {"MSC_VGX_WARM_ORDER", "1"}, {"MSC_SLIP_STEPS", "2"},
                                                  // the shader warm-up (wave 10, w8/warm10.md; int34): the old guard stopped it at picture 0 on every build since r7 (44 MiB in use passed at start-up of a 96 MiB heap), so a match started with about 31
                                                  // programs and the rest linked one a picture from the draw path, the draws that needed them skipped. Now the guard is a floor on the FREE heap (MSC_VGX_WARM_FLOOR_MIB, code default 24), picture 1
                                                  // warms at most MSC_WARM_CAP (600 in the plan) programs plus every marked match program via MSC_WARM_CAP_MARKED=1 (clamped to ProgramLimit()-64 = 836 so >= 64 slots stay free before EvictPrograms),
                                                  // and a draw that feeds a copy or an off-screen target is never left out for a program that is not ready (it compiles and links at once)
                                                  {"MSC_WARM_MATCH", "1"}, {"MSC_WARM_BLOCK", "1"}, {"MSC_WARM_CAP", "600"},
                                                  // pacing (wave 9, w8/pacing.md): the game clock never runs more than 2 % behind real time over a sustained overload (a late frame is cut only from a 30 ms bucket that fills at 2 % of real time:
                                                  // single player, np_slip.h), the two 600-frame thread sweeps run on the watch thread instead of the game and render threads (timing only), and the console log gets two [pace] lines per 5 s
                                                  // (display-queue call, GPU, flip, holds, frame ids shown, long holds by picture % 600). No picture or simulation result changes; console A/B: slipbudget0, sweepbg0
                                                  {"MSC_SLIP_BUDGET_MS", "30"}, {"MSC_SLIP_REFILL_PCT", "0.5"}, {"MSC_VITA_SWEEP_BG", "1"}, {"MSC_PACE_LOG", "5"},
                                                  // (wave 9, coordinator B and C, host model only, nothing timed on a console: kit runs latdepthp1, betweenold, padlate1)
                                                  // MSC_LAT_DEPTH_PACED=2: an aligned free run keeps 4 frames in flight, not 3: at a GPU of 1.0 refresh of work the model gives 59.2 pictures a second instead of 52.3, 1.05: 56.4 instead of
                                                  // 49.0, 1.1: 53.9 instead of 48.8, for 4.3 ms more button delay at 1.0 and none above it (tools/fps/pace_mid_model.cpp depth)
                                                  // The free run's gate, sticky: in under 20 ms of the busiest stage (1.2 refreshes: 7 pictures in 10 one refresh long), out over 25 ms for 60 frames, 45 s before the next try, doubled for a
                                                  // try that failed within 90 s (4 min at most); with the dynamic-resolution ladder judged by 24 ms while the run is free (it spent the headroom of the 30 budget on 2x anti-aliasing and
                                                  // a larger picture until the run gave 38-45 pictures a second in a 1/2 mix). A scene that needs 20-30 ms keeps the clean 30 at full quality
                                                  {"MSC_LAT_DEPTH_PACED", "2"}, {"MSC_FPS_BETWEEN_ENTER", "20"}, {"MSC_FPS_BETWEEN_LEAVE", "25"}, {"MSC_FPS_BETWEEN_COOL", "45"}, {"MSC_FPS_BETWEEN_COOLMAX", "240"},
                                                  {"MSC_FPS_BETWEEN_FAIL", "90"}, {"MSC_FPS_BETWEEN_OUT", "60"}, {"MSC_DYNRES_PACED_UP_MS", "24"},
                                                  // (wave 9) a tap latched by a frame that ran no simulation step (one in six at 60) reaches the next step; single player only (np_controls.h TapCarry)
                                                  {"MSC_TAP_CARRY", "1"},
                                                  // (wave 9 helper smooth) MSC_SMOOTH_CLOCK=1: under a held cadence a frame's game time is its picture's predicted SHOW-to-show time (whole refreshes, after 8 evenly held
                                                  // pictures; np_show_clock.h), not its start-to-start time: a picture shown 2 refreshes no longer carries 1 or 3 refreshes of motion when only its frame START moved
                                                  // (r10: 8-41 % of evenly held pictures). MSC_SMOOTH_LOG=1: the [smooth] line (display thread, one per 300 pictures of live play)
                                                  {"MSC_SMOOTH_CLOCK", "1"}, {"MSC_SMOOTH_LOG", "1"},
                                                  // (wave 10, host model only) MSC_LAT_DRAIN=1: a held 30 that stood aside after a long frame still drains a backlog of more than depth + 1 pictures (np_fps_fif.h);
                                                  // MSC_PAD_LATE=1: under a held 30 (a clean 30, the 30 fallback, a Mega Strike / Mega Save) the pad and the touch screen are read up to 12 ms later, in the slack the
                                                  // picture would have waited for its refresh anyway (single player only; never in a free run; np_feel.h PadLate gives it back at the first late picture)
                                                  {"MSC_LAT_DRAIN", "1"}, {"MSC_PAD_LATE", "1"}
                                                  // r16 warble19 (owner 10:11 / 10:23 / 10:30: every invisible GPU cut, and a cheaper shimmer of the same kind):
                                                  // the heat shimmer at Lava Pit and Sand Tomb with 2 scenes fewer, exact (host: 0 pixels differ at both, mutant caught);
                                                  // MSC_WARBLE_LITE=1 updates the shimmer map every second picture in the 60 mode only (30 Hz pattern). =0 turns one off; kit warboff
                                                  ,{"MSC_VGX_CLEARVEH", "1"}, {"MSC_VGX_EFBSWAP_NEXT", "1"}, {"MSC_VGX_PASSISO_COVER", "1"}, {"MSC_WARBLE_LITE", "1"}
                                                  // r16 probe18 / price17: the AI script profiling clock reads 0 (its only reader is the statistics while MSC_AI_BUDGET=off, the
                                                  // player default): about 80 clock reads a picture fewer on the game thread; det identical at The Dump, Lava Pit and match60, BREAK
                                                  // and the clock-counts mutant caught (w8/probe18.md). =0 turns it off; kit ainoclockoff
                                                  ,{"MSC_AI_NOCLOCK", "1"}
                                                  // r16 weld17 (Galaxy GXV_DLWELD port): each distinct vertex of a display list written once at its promotion (the [dlweld]
                                                  // line reports the ms spent): 45 % fewer static vertices, 11.7-13.3 MiB less static vertex memory (host counters), pictures identical
                                                  // on the host (match60, The Dump; mutant caught); Galaxy runs it on the console (drawn turn 23.9 -> 23.2 ms). =0 turns it off; kit dlweld0
                                                  ,{"MSC_VGX_DLWELD", "1"}
                                                  // dlrefresh24: skinned display lists the game rewrites before every draw (the walk-out's characters, 8-11 lists a picture) were decoded
                                                  // from scratch at every call (the bypass); now kept and only the arrays that changed are decoded again over the kept vertices, the
                                                  // result the full decode gives (host test with mutant, BEHASH and det identical on the walk-out and open play). =0 is the bypass; kit dlrefresh0
                                                  ,{"MSC_VGX_DLREFRESH", "1"}
                                                  // wave 14 (Galaxy asyncsh31): the shader worker's vglMalloc family takes newlib's heap only, never vitaGL's unlocked pools (a compile that fell
                                                  // back to them while the render thread allocated from the same pool corrupted it; the 8 MiB room check is a snapshot). Same memory as vglMalloc's
                                                  // first attempt while the heap has room, so nothing changes then; with no room the compile fails and the render thread compiles. Host: fake-allocator test + mutant
                                                  // (renderer/tests/run_worker_alloc_test.sh); never linked on the Vita by the helper. =0 restores the originals. console check: the [vgx] async-shaders-heap line (heap blocks, refused, foreign reallocs)
                                                  ,{"MSC_VGX_ASYNC_HEAPONLY", "1"}
                                                  // fepar25: the front end's vertex decode of plain stream draws (particles, HUD, flat crowd quads, replay overlays; 8 vertices or more, no matrix index) and the CPU
                                                  // transform of the soft ones run on a worker thread on core 1; the player joins it at the end of the picture (before the game is told its memory may change), before a record chunk is
                                                  // handed back and before the arena may move. Exact on the host (per-picture back-end hashes, frame hashes, det trace: Vice goal + replay + walk-out, The Dump, the tour; verify =2 and
                                                  // the join-skipped mutant). Estimate 1.7-2.3 ms off the player thread in live play (decode scope 1.9-2.7 ms + soft 0.3 ms of the s28i-fe lanes; x86 callgrind: 13 % of the front end's
                                                  // instructions leave the player, 0.4 M of staging stay). A worker silent for 200 ms is given up and the player decodes (a line says so). =0 turns it off; kit fepar0 / fepar0d
                                                  ,{"MSC_VGX_FEPAR", "1"}
                                                  // fe26: two exact cuts of the front end's per-draw state resolution. TEXSAME: a texture bind of the object the slot already holds, with no guest write
                                                  // notification since (or none over the texture's own pages), leaves the bindings clean, so the draw takes ResolveState's early exit; MTXSPLIT: the matrix blocks
                                                  // (position / texture / palette) are rebuilt only from the rows the writers touched. Host: per-picture back-end hashes, frame hashes and det identical off / on / verify
                                                  // at the tour, The Dump and Vice goal + replay + walk-out; mutants caught. Estimate 0.2-0.4 ms of the console's 3.0 ms ResolveState (x86 callgrind -8.5 %); not measured on a console. =0 turns each off; kit fe26off
                                                  ,{"MSC_VGX_TEXSAME", "1"}
                                                  ,{"MSC_VGX_MTXSPLIT", "1"}
                                                  // holdgpu24 (owner, 17 Nov 19:13: "448x320 is unacceptable"): the picture floors, renderer/src/vgx_floors.h. Quality never below 864x488, Smooth 60 never below 800x456 in
                                                  // pixels, and a 60 that cannot hold there falls back to a steady 30 at the full size. Render only. =0 is the old ladders (kit floors0)
                                                  ,{"MSC_PICTURE_FLOORS", "1"}
                                                  // crash26b (two console core dumps, 18 Nov): the SDK's newlib FILE has no lock; two threads printing to the shared stdout/stderr made __sfvwrite_r copy
                                                  // (size_t)-1 bytes over the heap. The wrapped printf / fputs / fwrite / puts / putc family of logging builds bypasses the FILE (np_log_stdio.h: one bounded
                                                  // format lock, whole texts to VitaLog::Write; the 1.2 stack depth, crash26's unbuffered streams stalled the console). Host proof: w8/crash26b.md. No picture
                                                  // or simulation change. =0 is the 1.2 path through the FILE; kit stdio0
                                                  // stab13 (Galaxy 2.3 crash: graphics memory handed back while the GPU still drew into it; vitaGL frees a texture that a scene drew into at once, only a
                                                  // texture that was sampled lately is deferred): vitaGL 0047 holds a delete of a block a scene drew into in the last four presents (or of the bound framebuffer's
                                                  // colour) 3 presents; every other delete is immediate, so a run that never makes such a delete is unchanged (the 0034 tour found none; host model tests/test_free_guard.py
                                                  // with mutants). A held delete shows as a [free-guard] line. =0 turns it off
                                                    /* MSC_VGX_FREE_GUARD_REF: off until fixed + console A/B (Fable review 22 Nov: held deletes are outside the 0036 recovery while video memory is full) */
                                                  ,{"MSC_STDIO_LOCK", "1"}   // a diagnostic line, no picture change: the first console night lists the frames over 25 ms and what made them
                                                  // (26 Nov, owner 08:28: "make sure any exact cuts made for performance apply to quality mode as well") two exact timing-only defaults that were
                                                  // LOCKED 60 only (kLock60) now apply in every mode, the 30 FPS / Quality choice included: the front end's decode worker kept on core 3, and the
                                                  // per-core probe read off the game thread (~0.6 ms of it a picture). No picture or simulation change (det exact on the 25 Nov int8 A/B).
                                                  ,{"MSC_VGX_FEPAR_KEEP", "7"}, {"MSC_FPS_PROBE_ASYNC", "1"}
                                                  // int26/int26b + ship26 + int28 (real-Vita verified):
                                                  // - RT_POOL=16 + VITA_FBO_KEEP=1 (6250/6251/6416: cuts RT destroys & saves 6 MiB peak CDRAM); VITA_ANISO_SCOPE=1; FX_OFFVIEW=1; SHQUAD_HELD=0 (-6.5 ms/pic Core 1).
                                                  // - ship26 held-scene pack: STADIUM_CUT_FAR=32, HELD_SCENES=7, FX_GLOW_COARSE=1, FX_GLOW=half, HELD_GOV_SEED=1, REPLAY_FARLOD=18 (raised from 10 to prevent DK close-up fly-in body LOD pop; HELD_GOV_SPAN left at code default 0 so walkouts at 26 ms GPU busy stay at crisp every-frame shadows/glow).
                                                  // - int28 visual fixes: SHADOW_RES left at code default 144 (96 missed JointSize 72x72, wasted 4.4 MiB RT + 67.7 ms hitch + 9x lower-res flicker; 6423 proved 144 is faster AND crisp);
                                                  //   HELD_GLOW_STAGGER / HELD_GOV_EFFECTS / VGL_MASKCLIP left OFF (eliminated EFB alpha / 7.5 Hz glow & shadow flashing and saved 1.5 scenes/f + 1.1 MB/f depth traffic);
                                                  //   FX_GOV_MAX=1 caps particle thinning at 1-in-2 (2x alpha) instead of harsh 4x strobing during DK fly-in.
                                                  // - int28 Core 0/3 scheduling (6420): LANE_CORE3=1 + VITA_PRIO_LANE=0x100000EC cut blend takeovers 73.1% -> 2.1% and halved SendJob join wait (100% Core 3 online-guard registered).
                                                  ,{"MSC_VGX_RT_POOL", "16"}, {"MSC_VITA_FBO_KEEP", "1"}, {"MSC_FX_OFFVIEW", "1"}
                                                  ,{"MSC_VGX_SHQUAD_HELD", "0"}, {"MSC_HELD_GOV_SEED", "1"}
                                                  ,{"MSC_STADIUM_CUT_FAR", "32"}, {"MSC_HELD_SCENES", "7"}, {"MSC_FX_GLOW_COARSE", "1"}, {"MSC_FX_GLOW", "half"}
                                                  ,{"MSC_REPLAY_FARLOD", "18"}, {"MSC_FX_GOV_MAX", "1"}
                                                  ,{"MSC_LANE_CORE3", "1"}, {"MSC_VITA_PRIO_LANE", "0x100000EC"}
                                                  // int29/int30 (real-Vita verified in 6442-6457):
                                                  // - VITA_QUEUE_FIXED60=2 stops LimitGpuQueue from collapsing 2->1 at 13.0 ms (still drops to 1 above 105% = 17.5 ms).
                                                  // - HOLD60_WALLSIM=1 (wallsim25_unit + 6454): prices 2-step simulation wall time at 30 fps in the frame clock so Vita runClocks lag (~5.4 ms) does not trap Vice at 30 fps.
                                                  // - VITA_STREAM_CACHED=1 (vitaGL 0060: 1.0 write-back/pic at scene submit, 0 spills, -2 ms Core 1 BE) + VGX_BEPREFETCH=2 (pure pld hint).
                                                  // - SKIN_POSE_PAR=1 + SKIN_POSE_PAR_AVOID=12 (6446/6447: exact Core 0+3 parallel skin-mesh posing avoiding Core 1/2, 0 mismatches, -1.6 to -2.1 ms Core 0).
                                                  // - VGX_LA88=2 (6447: exact 1-byte I8 / 2-byte IA8 self-test passed, saves 1.4-5 MiB CDRAM) + VGX_HSR_PROVEN=1 + VGX_ZERODRAW=1 (exact alpha=1 HSR & no-op draw skip).
                                                  ,{"MSC_VITA_QUEUE_FIXED60", "2"}, {"MSC_HOLD60_WALLSIM", "1"}, {"MSC_VITA_STREAM_CACHED", "1"}, {"MSC_VGX_BEPREFETCH", "2"}
                                                  ,{"MSC_SKIN_POSE_PAR", "1"}, {"MSC_SKIN_POSE_PAR_AVOID", "12"}
                                                  ,{"MSC_VGX_LA88", "2"}, {"MSC_VGX_HSR_PROVEN", "1"}, {"MSC_VGX_ZERODRAW", "1"}};
        char line[4096];   // r16: was 1024, which cut the list after ~MSC_HDTEX_FADE (the later defaults were set but never printed)
        size_t n = 0;
        auto add = [&](const char* name, const char* value) {
            setenv(name, value, 0);
            RememberDefault(name);
            const int w = std::snprintf(line + n, sizeof(line) - n, "%s%s=%s", n ? " " : "", name, value);
            if (w > 0 && n + static_cast<size_t>(w) < sizeof(line)) n += static_cast<size_t>(w);
        };
        for (const auto& s : kQuality)
            if (!Has(s[0])) add(s[0], s[1]);
        const char* dyn = std::getenv("MSC_DYNRES");
        const bool dynamicResolution = !(dyn && (std::strcmp(dyn, "0") == 0 || std::strcmp(dyn, "off") == 0));
        // 1.3 MSC_PICTURE_HOLD (the FULL picture policy for live play, renderer/src/vgx_hold.h) is the build's default picture: 960x544 held, edge smoothing on, and a late frame gives up the least
        // noticeable things first (effects target, far shadows, all shadows, crowd refresh) before the size takes up to three small steps (864x488, 776x440, 704x400, each a render target made
        // at match load: about 8 MiB of video memory); frozen in replays, cut-scenes, Mega Strikes and menus (the cadence holds those at 30). Nothing in 1.3 has run on a console: every
        // figure behind it is an estimate, and the console kit has a run that turns it off (the older ladder) and one that holds the size with no shedding. MSC_FX_GOV=1 goes with it (the
        // effects governor thins large sprites before any step, as the FULL preset always had it). It is NOT set when the player chose something it would fight: a fixed RESOLUTION or MSC_VITA_RENDER,
        // LOCKED 60 (its own governor), a 30 FPS rate (the hold's budget is the 60 one), DYNAMIC RESOLUTION off (no GPU timers: the policy would be blind) or the older MSC_VITA_MSAA. The
        // PERFORMANCE and LOCKED 30 modes write MSC_PICTURE_HOLD=0 (FULL PICTURE: OFF). MSC_PICTURE_HOLD=0 in env.txt / np-player.txt turns it off.
        // 18 Nov, owner's floors ("448x320 is unacceptable"): a fixed picture size under 864x488 (421,632 pixels: the 1.0-1.2 PERFORMANCE file's 640x362, the old SMALLER / SMALLEST rows' 640x362 and 480x272, a
        // hand-written MSC_VITA_RENDER) is raised to 864x488 here, with a log line. MSC_PICTURE_FLOORS=0 leaves it as written (a developer's switch; a host run that sets MSC_VITA_RENDER itself never passes
        // through this plan and is not clamped).
        if (Has("MSC_VITA_RENDER") && !(std::getenv("MSC_PICTURE_FLOORS") && !std::strcmp(std::getenv("MSC_PICTURE_FLOORS"), "0"))) {
            int rw = 0, rh = 0;
            if (std::sscanf(std::getenv("MSC_VITA_RENDER"), "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0 && static_cast<long>(rw) * rh < 864L * 488L) {
                std::fprintf(stderr, "[plan] MSC_PICTURE_FLOORS: MSC_VITA_RENDER=%dx%d is under the 864x488 floor: drawn at 864x488\n", rw, rh);
                const char* floorSize = "864x488";   // (a variable, not a plan default: the doc checker reads literal setenv calls as plan keys)
                setenv("MSC_VITA_RENDER", floorSize, 1);
            }
        }
        {   // plan24 FIX-01 (found on the host settings test: an empty np-player.txt gave MSC_PICTURE_HOLD=full + MSC_VITA_RENDER=960x544 and then "LOCKED 60 ... does nothing here", so none of
            // SMOOTH 60's supporting defaults were set; every console run named MSC_LOCK60=1 by hand and never saw it). SMOOTH 60 is the build's default frame-rate mode (l60def18: the 23 Nov console
            // A/B below), so the key is set here, BEFORE the full picture's default reads it ("LOCKED 60 is on": no held picture, no fixed size, the dynamic resolution's own governor), and not after.
            // Not when the file names a frame rate of its own (30 LOCKED = MSC_FPS=30, a fixed 45 / 60 / unlocked / game): the renderer reads MSC_LOCK60 by itself, and a 60-first picture governor has no
            // place under a locked 30. A player who named a frame rate, or the hold, gets the older (hold-on) defaults as before the flip.
            static const bool kLock60Default = true;
            const char* fpsNamed = std::getenv("MSC_FPS");
            const bool fpsFree = !fpsNamed || !*fpsNamed || !std::strcmp(fpsNamed, "auto");
            const char* holdNamed = std::getenv("MSC_PICTURE_HOLD");   // a hold the file names is the hold's own mode (960x544 fixed, its own governor): LOCKED 60 is not added on top of it
            const bool holdNamedOn = Has("MSC_PICTURE_HOLD") && holdNamed && *holdNamed && std::strcmp(holdNamed, "0") && std::strcmp(holdNamed, "off") && std::strcmp(holdNamed, "no");
            if (kLock60Default && !Has("MSC_LOCK60") && fpsFree && !holdNamedOn) {
                setenv("MSC_LOCK60", "1", 0);
                RememberDefault("MSC_LOCK60");
            }
        }
        {
            static const bool kHoldDefault = true;
            if (kHoldDefault && !Has("MSC_PICTURE_HOLD")) {
                const char* fps = std::getenv("MSC_FPS");
                const char* l60 = std::getenv("MSC_LOCK60");
                const char* c45 = std::getenv("MSC_CADENCE45");   // (helper f45: 45 FPS keeps the 30 FPS ladder, the Quality floors, with the 45 budget: no held picture)
                const bool f45 = (!fps || !*fps || !std::strcmp(fps, "auto")) && c45 && *c45 && std::atoi(c45) != 0;
                const char* why = Has("MSC_VITA_RENDER") ? "a fixed picture size is named" : (l60 && *l60 && std::atoi(l60) != 0 && !f45) ? "LOCKED 60 is on" : (fps && (!std::strcmp(fps, "30") || !std::strcmp(fps, "45"))) ? "the frame rate is 30"
                                : f45 ? "the 45 cadence is on by hand (MSC_CADENCE45=1: the 30 FPS ladder, inside the Quality floors, with the 45 budget)"
                                : !dynamicResolution ? "DYNAMIC RESOLUTION is off" : Has("MSC_VITA_MSAA") ? "MSC_VITA_MSAA is named" : nullptr;
                if (why) {
                    std::fprintf(stderr, "[plan] full picture (MSC_PICTURE_HOLD) not on by default: %s\n", why);
                } else {
                    add("MSC_PICTURE_HOLD", "full");
                    if (!Has("MSC_FX_GOV")) add("MSC_FX_GOV", "1");
                }
            }
        }
        // 1.3 MSC_LIVE_TRADES (np/livetrades.h, w8/trade.md): the owner's trades for LIVE PLAY ONLY (open play, TASK_GAMEPLAY) that pay for 960x544 at 60 with anti-aliasing: a player's shadow is one soft
        // ellipse in the main scene instead of a shadow picture (MSC_SHADOW_LIVE=flat: no pass, no copy, no model copy), and at 60 pictures a second the crowd sprites refresh at the rate in time they have at 30
        // (MSC_CROWD_LIVE: 8 sprite passes a frame in place of 16). Replays, cut-scenes, celebrations, Mega Strikes and menus keep the full look.
        // On with the FULL picture policy (MSC_PICTURE_HOLD on, named by the plan or the file); the trade has its own switch for the console kit (=full turns the shadow trade off alone). Render side only.
        if (!Has("MSC_LIVE_TRADES")) {
            const char* hold = std::getenv("MSC_PICTURE_HOLD");
            if (hold && *hold && std::strcmp(hold, "0") != 0 && std::strcmp(hold, "off") != 0) add("MSC_LIVE_TRADES", "1");
        }
        // The shadow trade is about live play, not about the hold: it is on in every picture mode (FULL PICTURE on or off, PERFORMANCE, LOCKED 30 / 60) unless the file names MSC_SHADOW_LIVE (the
        // Settings row LIVE SHADOWS FULL writes it). Flat shadows stay in live play whatever frame rate the game is running at (a change of shadow style in play would be a visible pop).
        if (!Has("MSC_SHADOW_LIVE")) add("MSC_SHADOW_LIVE", "flat");
        if (Has("MSC_PICTURE_HOLD") && !Has("MSC_VITA_RENDER")) {
            const char* h = std::getenv("MSC_PICTURE_HOLD");
            const char* size = nullptr;
            if (h && (!std::strcmp(h, "full") || !std::strcmp(h, "screen"))) size = "960x544";
            else if (h && (!std::strcmp(h, "1") || !std::strcmp(h, "native") || !std::strcmp(h, "on"))) size = "640x448";
            else if (h && std::strchr(h, 'x')) size = h;
            {   // (the floors, F3: a held size under 864x488 (=native's 640x448 or a hand-written WxH) is held at 864x488)
                int hw = 0, hh = 0;
                const char* ff = std::getenv("MSC_PICTURE_FLOORS");
                if (size && !(ff && !std::strcmp(ff, "0")) && std::sscanf(size, "%dx%d", &hw, &hh) == 2 && static_cast<long>(hw) * hh < 864L * 488L) {
                    std::fprintf(stderr, "[plan] MSC_PICTURE_FLOORS: the held size %s is under 864x488: held at 864x488\n", size);
                    size = "864x488";
                }
            }
            if (size) {
                setenv("MSC_VITA_RENDER", size, 0);
                RememberDefault("MSC_VITA_RENDER");   // (derived: a reread of the settings file drops it with the rest of the plan's values)
                std::fprintf(stderr, "[plan] MSC_PICTURE_HOLD=%s: fixed picture size %s (MSC_VITA_RENDER), no ladder\n", h, size);
            }
        }
        // 1.3 MSC_PICTURE_HOLD_MSAA=0|2|4 (renderer/src/vgx_hold.h, vgx_backend_gl.cpp "MSC_PICTURE_HOLD_MSAA"): the held picture is multisampled in live play, replays and menus (the main scene target only; a Mega Strike
        // or a heavy stadium draws with one sample because the effects pass needs the scene depth, which a multisampled target cannot give on the console). When frames run late the samples go first (4 -> 2 -> 1), before
        // any size step, and come back on proof of room. The default is 2 (the owner's 12 Nov decision: full 960x544 at 60 with anti-aliasing; 2x is the likely landing point, 4x stays selectable). The Settings row
        // ANTI-ALIASING maps onto it while the hold is on: ON (DEFAULT) 2 (MSC_VGX_MSAA=4, the ladder's default value, reads as ON), 2X ONLY 2 (MSC_VGX_MSAA=2), OFF 0 (MSC_VGX_MSAA=0), 4X (MSC_PICTURE_HOLD_MSAA=4). Costs 6.1 MiB (2x) / 16.3 MiB (4x with the 2x stop) of video memory
        // more than the hold's 12.2 MiB (ESTIMATES, colour + depth); a target that does not fit is left out of the order at run time. Nothing in 1.3 has run on a console.
        const bool holdOn = [&] { const char* h = std::getenv("MSC_PICTURE_HOLD"); return Has("MSC_PICTURE_HOLD") && h && *h && std::strcmp(h, "0") && std::strcmp(h, "off") && std::strcmp(h, "no"); }();
        if (holdOn && !Has("MSC_PICTURE_HOLD_MSAA")) {
            const char* m = std::getenv("MSC_VGX_MSAA");   // the ANTI-ALIASING row's key
            if (m && !std::strcmp(m, "0")) add("MSC_PICTURE_HOLD_MSAA", "0");
            else add("MSC_PICTURE_HOLD_MSAA", "2");
        }
        // 1.3 (hold9, from the first console runs, 13 Nov): MSC_PICTURE_HOLD_PACE=1 reads GPU time against what the pacing can use (the measured picture period when another stage, the game thread
        // or the 30 fps hold, sets it and the GPU is not the limit) and climbs back in seconds; MSC_PICTURE_HOLD_COMMIT=1 decides the ladder and samples at kick-off and never lets a hold target go in a match.
        if (holdOn && !Has("MSC_PICTURE_HOLD_PACE")) add("MSC_PICTURE_HOLD_PACE", "1");
        if (holdOn && !Has("MSC_PICTURE_HOLD_COMMIT")) add("MSC_PICTURE_HOLD_COMMIT", "1");
        // holdgpu24 (console s27d1b: "GPU mean 26.9 ms of 26.5" stepped 960x544 -> 912x520 -> 864x488 while [lane] gpu read 16.4 ms): with the pacing on, a held 30's late picture counts towards a size step only when the
        // GPU's busy time is the limit (vgx_hold.h Pacing::GpuGate), never for CPU-bound lateness. =0 is the old rule (kit holdgpubusy0)
        if (holdOn && !Has("MSC_PICTURE_HOLD_GPUBUSY")) add("MSC_PICTURE_HOLD_GPUBUSY", "1");
        // owner's decision (13 Nov, after side-by-side pictures: "full resolution without anti-aliasing looks worse than the resolution step down with it"): MSC_PICTURE_HOLD_AALAST=1 sheds picture SIZE first,
        // in small steps (960x544, 912x520, 864x488, 800x456), each with 2x; the samples go last, at the bottom. 0 is the earlier order (samples first).
        if (holdOn && !Has("MSC_PICTURE_HOLD_AALAST")) add("MSC_PICTURE_HOLD_AALAST", "1");
        // hold10 (r10 console run, 13 Nov): MSC_PICTURE_HOLD_START=1 starts a match at a level sure to hold the rate (the last match's level here, else a stadium table) with 8 s excused from proof;
        // MSC_PICTURE_HOLD_SCENES=1 keeps that level in replays and cut-scenes; MSC_PICTURE_HOLD_BUDGET_MARGIN=0.80 budgets a held 30 at 80 per cent of its period so pictures make their flip.
        if (holdOn && !Has("MSC_PICTURE_HOLD_START")) add("MSC_PICTURE_HOLD_START", "1");
        // dumpplay16 (r11 console logs: The Dump steps 9 -> 10 in its first seconds, 16-27 late pictures and 2-5 pass objects made in play): The Dump's first match starts on the bottom step (renderer/src/vgx_dumpplay.h); the same size it keeps from second 10 on; no other stadium; render only.
        if (holdOn && !Has("MSC_DUMP_START")) add("MSC_DUMP_START", "1");
        if (holdOn && !Has("MSC_PICTURE_HOLD_SCENES")) add("MSC_PICTURE_HOLD_SCENES", "1");
        // hold12 (r11/r12: goal cut-scenes and replays ran 14-20 pictures a second): held scenes get their own policy, started at a level known to fit on the cut picture itself.
        // r15 (14 Nov, Fable verification + emulator run r14-hold): OFF until the console decides. The policy acted on a cut-scene in Vita3K but did not engage on replays,
        // and its size change on the cut picture rebuilds pass objects at every goal until cuts10's pin and size sets are proven (w8/fable-stutter-1114.md F). Kit run scenepol1.
        if (holdOn && !Has("MSC_PICTURE_HOLD_BUDGET_MARGIN")) add("MSC_PICTURE_HOLD_BUDGET_MARGIN", "0.80");
        const bool ladder = dynamicResolution && !Has("MSC_VITA_RENDER") && !Has("MSC_VITA_MSAA");
        if (holdOn) {
            // (the older ladder's rungs are not used with the hold: MSC_PICTURE_HOLD_MSAA is the hold's own anti-aliasing)
        } else if (!Has("MSC_VGX_MSAA")) {
            if (ladder) add("MSC_VGX_MSAA", "4");
            else std::fprintf(stderr, "[plan] anti-aliasing rungs not on by default: no dynamic-resolution ladder (a fixed picture size, MSC_DYNRES off or MSC_VITA_MSAA); MSC_VGX_MSAA=4 asks for them\n");
        }
        if (n)
            std::fprintf(stderr, "[plan] quality and timing switches on by default (=0 in env.txt / np-player.txt turns one off; MSC_DOF=1 is the game's own, MSC_VGX_SOFTXFORM=128 the 1.1 limit): %s\n", line);
    }
    // 1.3 MSC_LOCK60 (LOCKED 60, docs/TECHNICAL.md "LOCKED 60"): 60 first in every scene, the picture size decided by renderer/src/vgx_lock60.h and the controller by np_fps_ctl.h Config::Lock60.
    // kLock60Default is the one switch that makes it the build's default (true since l60def18: the 23 Nov console A/B below); the Settings mode of the same
    // name (and MSC_PICTURE=lock60) writes MSC_LOCK60=1. With it on, the supporting switches below are defaults unless a line of the owner's names them:
    //   MSC_AMORT_60=1           the crowd's and the shadows' refresh keeps its rate in time at 60 (the work does not double with the frame rate)
    //   MSC_FX_GOV=1, MSC_FX_GOV_MEGA=1   the effects governor thins large sprites first, in Mega Strikes too (it runs before any picture rung)
    //   MSC_MEGA_WARM_KIB=28672  the render targets of the rungs, made ahead during a stadium's intro, are held (the Mega Strike's 16 MiB would drop half of them)
    //   MSC_VGX_HUDNATIVE=1      (look, 11 Nov) the 2D layers at the screen's 960x544 over the upscaled scene. Below about 0.86 of the screen per side the HUD is drawn at the 3D size and
    //                            blurred by the upscale: host pictures at 499x283 (w6/look: hud-499x283-*.png, an emulation with the native HUD pasted in) show the clock, score digits and the
    //                            Mega Strike prompts soft against crisp. LOCKED 60 is the mode that lives at those sizes, so it is where the switch belongs first. A picture within 3 % of the
    //                            screen's size is copied by the blit and has no split. NOT run on a console and not testable on the host harness (the present split is in the Vita host): the
    //                            console kit run `hudnative` (cost, HUD flipping between sharp and soft in frames where the run is cut: pause menu, wipes) decides whether it also becomes a default of the AUTO modes.
    // Never with a fixed picture size or the dynamic resolution off (the governor needs the rungs and the GPU timers): the line is logged and nothing is set.
    {
        // (l60def18, 23 Nov console s49, seed 1234abcd: SMOOTH 60 MODE with the hold against the UP TO 60 default: live-play fps 30.1 -> 37.8 Vice, 30.0 -> 36.9 The Dump; judder
        // 0.22-0.26 -> 0.19-0.20; walk-outs late 61 -> 41 % Vice, 37 -> 26 % The Dump; celebrations late 34 -> 15 % The Dump; no fault. Owner: "getting to 60 smooth should be the goal".)
        // (plan24 FIX-01: the MSC_LOCK60 default itself is set earlier, before the full picture's default, which asks for it: kLock60Default above)
        {   // helper f45: FRAME RATE 45 FPS wins over SMOOTH 60 MODE (np_fps.cpp ignores MSC_LOCK60 under MSC_FPS=45); the renderer reads MSC_LOCK60 itself, so the key is taken out here
            const char* fps = std::getenv("MSC_FPS");
            const char* c45 = std::getenv("MSC_CADENCE45");
            const char* lk = std::getenv("MSC_LOCK60");
            const bool f45 = (!fps || !*fps || !std::strcmp(fps, "auto")) && c45 && *c45 && std::atoi(c45) != 0;
            if (f45 && lk && *lk && std::atoi(lk) != 0) {
                unsetenv("MSC_LOCK60");
                std::fprintf(stderr, "[plan] MSC_CADENCE45=1 (the 45 cadence alone, a developer switch): SMOOTH 60 MODE (MSC_LOCK60) is not used with it\n");
            }
        }
        const char* l60 = std::getenv("MSC_LOCK60");
        if (l60 && *l60 && std::atoi(l60) != 0) {
            const char* dyn = std::getenv("MSC_DYNRES");
            if ((dyn && (std::strcmp(dyn, "0") == 0 || std::strcmp(dyn, "off") == 0)) || Has("MSC_VITA_RENDER")) {
                std::fprintf(stderr, "[plan] LOCKED 60 (MSC_LOCK60) needs the dynamic resolution and no fixed picture size: it does nothing here\n");
            } else {
                // (hold18, 23 Nov console s48, Vice x2 pairs: live-play fps 32.6 -> 37.1, The Dump 39.6; judder 0.18 both, players+ball motion p95 0.36-0.51; 45 instead of 30 for most of play) MSC_LOCK60_HOLD
                static const char* const kLock60[][2] = {{"MSC_AMORT_60", "1"}, {"MSC_FX_GOV", "1"}, {"MSC_FX_GOV_MEGA", "1"}, {"MSC_MEGA_WARM_KIB", "28672"}, {"MSC_VGX_HUDNATIVE", "1"}, {"MSC_LOCK60_HOLD", "1"},
                                                          // (wave 25, 24-25 Nov console int8, Vice player-like: FEPAR worker kept on core 3 (0 retirements, join wait 0.02-0.1 ms), the crowd one level
                                                          // lighter only in live play at 60, the probe off the game thread: 60 for 19-25 s against 13-19 s, and what lets the strict 50 hold; det exact (core60))
                                                          {"MSC_VGX_FEPAR_KEEP", "7"}, {"MSC_CROWD_FLOOR60", "4"}, {"MSC_FPS_PROBE_ASYNC", "1"},
                                                          // int26/int26b/int31 (26 Nov console 6250 + Carbon #2/S1): bigger sceGxm command rings (+640 KiB CDRAM) and scoped AF in Smooth 60 only (keeps Quality 30's tight CDRAM margin and full blended AF untouched; FBO_KEEP=1 is in kQuality for both).
                                                          {"MSC_VITA_VDM_KIB", "256"}, {"MSC_VITA_FRAGMENT_KIB", "1024"}, {"MSC_VITA_USSE_KIB", "64"}, {"MSC_VITA_ANISO_SCOPE", "1"},
                                                          // int32/int33 (real-Vita 6474e/6483/6483b/6483c): start normal play at 768x544 (The Dump starts at 728x512) and climb smoothly to native 960x544 1x without single-frame 16.3-16.8 ms blips aborting probation (PROBEAT=1.06, LATEAT=1.08), and keep Smooth-60 crowd floor/amortization active across temporary 30-fps live-play relief (CROWD_FLOOR60_LIVE=1) so The Dump never traps itself at 30 fps.
                                                          {"MSC_LOCK60_PLAY_START", "2"}, {"MSC_LOCK60_UPFIT", "0.88"}, {"MSC_LOCK60_UPHARD", "0.97"}, {"MSC_LOCK60_PROOF", "90"},
                                                          {"MSC_LOCK60_PROBEAT", "1.06"}, {"MSC_LOCK60_LATEAT", "1.08"}, {"MSC_CROWD_FLOOR60_LIVE", "1"}, {"MSC_LOCK60_HELD_FULL", "1"}};
                char line[1024];
                size_t n = 0;
                for (const auto& s : kLock60) {
                    if (Has(s[0])) continue;
                    setenv(s[0], s[1], 0);
                    RememberDefault(s[0]);
                    const int w = std::snprintf(line + n, sizeof(line) - n, "%s%s=%s", n ? " " : "", s[0], s[1]);
                    if (w > 0 && n + static_cast<size_t>(w) < sizeof(line)) n += static_cast<size_t>(w);
                }
                std::fprintf(stderr, "[plan] LOCKED 60 on (MSC_LOCK60=1): supporting defaults %s\n", n ? line : "all named by the file");
                // wave 25 (25 Nov console int8): with the strict 50 tier (MSC_LOCK60_USE50=1) a fall to 30 comes back to 50 by trial (P98 + TRIAL), not by the per-picture proof, which never
                // passed at Vice (the game thread's 16-18 ms against an 18 ms fit): Vice 81 % and 100 % of live play at 50 or 60 against 31-37 % without the trial. One line for the owner's A/B.
                if (const char* u50 = std::getenv("MSC_LOCK60_USE50")) {
                    if (*u50 && std::atoi(u50) != 0) {
                        if (!Has("MSC_LOCK60_P98")) { setenv("MSC_LOCK60_P98", "1", 0); RememberDefault("MSC_LOCK60_P98"); }
                        if (!Has("MSC_LOCK60_TRIAL")) { setenv("MSC_LOCK60_TRIAL", "1", 0); RememberDefault("MSC_LOCK60_TRIAL"); }
                        std::fprintf(stderr, "[plan] MSC_LOCK60_USE50=1: the 30 comes back to the 50 by trial (MSC_LOCK60_P98=%s MSC_LOCK60_TRIAL=%s)\n", std::getenv("MSC_LOCK60_P98"), std::getenv("MSC_LOCK60_TRIAL"));
                    }
                }
            }
        }
    }
    // 1.3 MSC_FPS_BETWEEN=pace as the AUTO default (docs/TECHNICAL.md "Frame rate: AUTO holds 30 while 37 to 60 is available"). AUTO alone goes back to 60 only when every core's
    // predicted 60 Hz cost is under 80 % of a refresh (13.3 ms) for 2 s and after a cool-down of 6 s to 4 min, so a scene that needs 14-26 ms a frame (38-70 pictures a second
    // available) stayed at 30: the shape of Galaxy's old pacing rule. With pace such a match runs free between 30 and 60 (the controller is unchanged and still goes to 60 when it
    // fits). kBetweenDefault is the switch; MSC_FPS_BETWEEN=30 in env.txt / np-player.txt, or the 30 FPS choice, turns it off. Not with LOCKED 60 (its controller does not use it).
    {
        static const bool kBetweenDefault = true;
        const char* l60 = std::getenv("MSC_LOCK60");
        const bool lock60 = l60 && *l60 && std::atoi(l60) != 0;
        const char* c45 = std::getenv("MSC_CADENCE45");   // (helper f45: MSC_CADENCE45=1 is MSC_FPS=45: no AUTO pacing)
        const bool f45 = c45 && *c45 && std::atoi(c45) != 0;
        if (kBetweenDefault && !lock60 && !f45 && !Has("MSC_FPS_BETWEEN") && !Has("MSC_FPS")) {
            setenv("MSC_FPS_BETWEEN", "pace", 0);
            RememberDefault("MSC_FPS_BETWEEN");
            std::fprintf(stderr, "[plan] Frame Rate AUTO rates between 30 and 60 by default (MSC_FPS_BETWEEN=pace; =30 holds AUTO's 30 until 60 fits)\n");
        }
    }
    // 1.2 PS TV (audit item: the game must be fully playable with the pad alone). Every action of the game has a button on the pad (Mega Strike meter, Mega Save aim
    // with the stick and Cross / R, the opponent's Mega Strike by mashing, the menus, the title's Start; CONTROLS.md), and touch is only ever an addition. A PS TV has
    // neither panel, but a controller's stick click can be mapped to a touch by the system, which here would be a stray rear tap (cycle items, switch player) or a stray
    // press of a menu button. So on a PS TV the player gets the pad-only defaults: no menu pointer, no touch Mega Save / Strike / Shake, no rear shortcuts (the
    // Triangle control guide then lists the buttons only). np-player.txt wins (a line of the owner's for any of them stays); MSC_PSTV=0 turns this off.
    if (IsPsTv()) {
        static const char* const kPadOnly[][2] = {{"NP_TOUCH_POINTER", "0"}, {"MSC_TOUCH_MINIGAMES", "0"}, {"MSC_REAR_TOUCH", "0"}};
        char line[160];
        size_t n = 0;
        for (const auto& s : kPadOnly) {
            if (Has(s[0])) continue;
            setenv(s[0], s[1], 0);
            const int w = std::snprintf(line + n, sizeof(line) - n, "%s%s=%s", n ? " " : "", s[0], s[1]);
            if (w > 0 && n + static_cast<size_t>(w) < sizeof(line)) n += static_cast<size_t>(w);
        }
        std::fprintf(stderr, "[plan] PS TV (no touch screen, no rear touchpad): pad-only defaults%s%s (a line in np-player.txt wins; MSC_PSTV=0 turns this off)\n", n ? ": " : " already set by the file", n ? line : "");
    }
    // W5 RT (docs/wave5-RT.md): the timers of the scene changes, of the GPU and of the first draw after a target change, and the input-delay record
    // (pad read -> the refresh that shows the frame) are on in every LAB build: their cost is a clock read per scene change and per timed draw, one
    // GPU notification and one wake-up of a watcher thread per scene, a few clock reads per frame; they print the [rt] and [lat] lines and the lat_*
    // fields of [frame-timing]. The player build has them off (MSC_VITA_RT_TIMERS / MSC_VGX_RTTIME / MSC_LAT in np-player.txt turn them on).
    {
        static const char* const kLab[][2] = {{"MSC_VITA_RT_TIMERS", "3"}, {"MSC_VGX_RTTIME", "1"}, {"MSC_LAT", "1"}, {"MSC_STALL_TIMERS", "1"}};
        static const char* const kPlayer[][2] = {{"MSC_LAT", "0"}};
        if (!player) {
            for (const auto& s : kLab) if (!Has(s[0])) setenv(s[0], s[1], 0);
        } else {
            for (const auto& s : kPlayer) if (!Has(s[0])) setenv(s[0], s[1], 0);
            // DYNRES (23 Oct): the renderer's dynamic resolution and the frame-rate controller read the GPU time of every frame, so the player build
            // has the GPU timers on (one notification and one watcher wake-up per scene); MSC_DYNRES=0 or MSC_VITA_RT_TIMERS=0 turns them off.
            const char* dyn = std::getenv("MSC_DYNRES");
            if (!Has("MSC_VITA_RT_TIMERS") && !(dyn && (std::strcmp(dyn, "0") == 0 || std::strcmp(dyn, "off") == 0))) setenv("MSC_VITA_RT_TIMERS", "2", 0);
        }
    }
    // W5 RT (docs/wave5-RT.md section 6): the button lag fix. Under the 30 fps cap the frame was held on the game thread until two retraces after
    // the previous hand-over and only then sent to the GPU and the display; with MSC_LATE_WAIT=1 it is sent as soon as it is built and the same
    // wait follows the send. Every game task keeps its place against the wait (host32: frame times and menus frame-identical, det traces
    // deterministic, LAN pairs late/late and late/off in sync), pacing is held by the display gate as before. Every Vita build gets it unless
    // env.txt / np-player.txt sets MSC_LATE_WAIT (=0 turns it off).
    // W5 ITEM (docs/wave5-ITEM.md): the drawing-only fixes of the original game (resting goal nets, the electrocution picture) on by default;
    // the settings row ORIGINAL GAME FIXES writes "none" for OFF and "all" for the Boo fix too (that one changes the match).
    if (!Has("MSC_GAME_FIXES")) {
        setenv("MSC_GAME_FIXES", "net,electro", 0);
        std::fprintf(stderr, "[plan] original-game drawing fixes on by default: MSC_GAME_FIXES=net,electro (none turns them off)\n");
    }
    if (!Has("MSC_LATE_WAIT")) {
        setenv("MSC_LATE_WAIT", "1", 0);
        std::fprintf(stderr, "[plan] button lag fix on by default: MSC_LATE_WAIT=1 (the frame is sent before the 30 fps wait; MSC_LATE_WAIT=0 turns it off)\n");
    }
    // W5 RT (docs/wave5-RT.md sections 3-4): the per-scene depth load/store hints (vgx MSC_VGX_RT=1 -> vitaGL vglRtSceneHint, the 0003 patch): pass targets,
    // copy destinations and the display skip their depth load and store, the EFB's when its depth is not read again. Pictures equal on host32 GL with the
    // hostile-depth simulation (75 of 75); not yet seen on a console. Every Vita build gets it unless env.txt / np-player.txt sets MSC_VGX_RT (=0 turns it off).
    if (!Has("MSC_VGX_RT")) {
        setenv("MSC_VGX_RT", "1", 0);
        std::fprintf(stderr, "[plan] render-target depth hints on by default: MSC_VGX_RT=1 (MSC_VGX_RT=0 turns them off)\n");
    }
    // stab13 (MSC_LAUNCH_GUARD, np_launch_guard.h): the player build only. The marker file is read and set once per process; a launch whose predecessor never cleared it
    // starts with the optional thread / core / pacing switches off (the plan's own defaults only: a line of the owner's wins), the log says which.
    if (player) {
        LaunchGuardBegin();
        LaunchEnv env;
        const int off = NpLaunchGuard::ApplySafe(g_launchState, env);
        if (off && !g_launchSafeLogged) {
            g_launchSafeLogged = true;
            std::fprintf(stderr, "[launch] %d optional switch(es) off for this launch; a clean exit or %u minutes of play clear the marker\n", off,
                         NpLaunchGuard::MinutesFromText(std::getenv("MSC_LAUNCH_GUARD_MIN")));
        }
    }
    const char* want = std::getenv("MSC_PLAN");
    const bool explicitThreads = Has("MSC_FRAMESEND") || Has("MSC_VGX_BACKEND_THREAD") || Has("MSC_VITA_CORE_FRAMESEND");
    const bool automatic = (player || (want && *want)) && !explicitThreads && (!want || !*want || std::strcmp(want, "auto") == 0);
    // Choosing an explicit plan is not a failed automatic start. Restore only an unresolved reservation, not a real result.
    if (!automatic) CancelPendingStartUnlocked();
    if (!player && !(want && *want)) {
        PlaceAudioAndFiles(-1);
        return 0;
    }
    if (explicitThreads) {
        std::fprintf(stderr, "[plan] the settings file sets the frame send / back end threads itself: no plan applied, no guard\n");
        PlaceAudioAndFiles(-1);
        return 0;
    }
    int level = 0;
    int failed = 0;
    if (want && *want && std::strcmp(want, "auto") != 0) {
        level = (*want == 'A' || *want == 'a') ? 2 : (*want == 'S' || *want == 's') ? 3 : ((*want == 'C' || *want == 'c') && (want[1] == 'n' || want[1] == 'N')) ? 1 : 0;
        std::fprintf(stderr, "[plan] MSC_PLAN=%s: plan %s chosen by hand, no guard\n", want, LevelName(level));
    } else {
        // A settings reread is still the same launch, not another failed start.
        if (!g_guardStarted) {
            g_startFailures = ReadGuard();
            g_guardStarted = true;
        }
        if (!g_guardReserved && !g_cleared) {
            WriteGuard(g_startFailures + 1);
            g_guardReserved = true;
        }
        failed = g_startFailures;
        level = failed <= 0 ? 0 : failed == 1 ? 1 : failed == 2 ? 2 : 3;
        g_guarded = true;
        std::fprintf(stderr, "[plan] guard %s: %d earlier start(s) did not get through %d frames -> plan %s\n", kGuardFile, failed < 0 ? 0 : failed, kClearFrames, LevelName(level));
    }
    g_level = level;
    struct Setting { const char* name; const char* value; };
    // (MSC_FS_PIPE: the two-stage pipeline, docs/wave5-PIPE.md; plan C only, so that the first step down of the guard takes it away and keeps the threads.
    //  kPipeInPlanC is the one switch that puts it in the player's default. True since the proof runs are in (docs/wave5-PIPE.md 1.4-1.6: no hang, equal pictures and digests
    //  with NOASLR on match60 / survey / movie / full match in serial and thread mode, race check clean); the guard's first step down (Cn) takes it away again.)
    static const bool kPipeInPlanC = true;
    static const Setting kC[] = {{"MSC_FRAMESEND", "thread"}, {"MSC_VGX_BACKEND_THREAD", "1"}, {"MSC_VITA_CORE_FRAMESEND", "1"}, {"NP_VITA_CORE_THP", "12"}, {"MSC_FS_PIPE", "1"}};
    static const Setting kA[] = {{"MSC_FRAMESEND", "thread"}, {"NP_VITA_CORE_THP", "12"}};
    static const Setting kS[] = {{"MSC_FRAMESEND", "off"}};
    const Setting* list = level <= 1 ? kC : level == 2 ? kA : kS;
    const int count = level == 0 ? (kPipeInPlanC ? 5 : 4) : level == 1 ? 4 : level == 2 ? 2 : 1;
    char line[256];
    size_t n = 0;
    for (int i = 0; i < count; ++i) {
        setenv(list[i].name, list[i].value, 0);
        if (names && i < maxNames) names[i] = list[i].name;
        const int w = std::snprintf(line + n, sizeof(line) - n, "%s%s=%s", n ? " " : "", list[i].name, list[i].value);
        if (w > 0 && n + static_cast<size_t>(w) < sizeof(line)) n += static_cast<size_t>(w);
    }
    std::fprintf(stderr, "[plan] core plan %s: %s\n", LevelName(level), line);
    PlaceAudioAndFiles(level);
    return count < maxNames ? count : maxNames;
}

// From the platform's watch thread (a few seconds apart).
void Heartbeat(unsigned long long frames) {
    GuardLock lock;
    if (!lock) return;
    if (frames >= static_cast<unsigned long long>(kClearFrames) && NpChangeGuardVita::Stable) NpChangeGuardVita::Stable();   // (the same stable point; a no-op after the first)
    if (g_launchMarked && NpLaunchGuard::Stable(frames, ProcessMs(), NpLaunchGuard::MinutesFromText(std::getenv("MSC_LAUNCH_GUARD_MIN")))) LaunchGuardClear("stable play");
    if (!g_guarded || g_cleared || frames < static_cast<unsigned long long>(kClearFrames)) return;
    g_cleared = true;
    g_guardReserved = false;
    WriteGuard(0);
    std::fprintf(stderr, "[plan] %llu frames presented: the guard is cleared (plan %s works)\n", frames, LevelName(g_level));
}

// A stall or a crash: the next start takes the next plan down.
void Failed(const char* why) {
    // A fault may have interrupted a mutex holder. Never wait for it; a surviving holder repairs its last write before returning.
    g_failurePending.store(true);
    GuardLock lock(true);
    if (lock && g_failurePending.exchange(false)) FailedUnlocked(why);
}

// A clean leave (the game's quit, the settings screen's exit, the end of main): the launch marker goes back to 0.
void LaunchClean(const char* why) {
    GuardLock lock;
    if (!lock) return;
    LaunchGuardClear(why ? why : "clean exit");
}

int Level() {
    GuardLock lock;
    return lock ? g_level : -1;
}

} // namespace VitaPlan
