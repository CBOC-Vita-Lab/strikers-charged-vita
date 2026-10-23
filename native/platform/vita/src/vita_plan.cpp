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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#endif

namespace VitaPlan {
namespace {

constexpr const char* kGuardFile = "ux0:data/mscharged/np-plan.txt";
// A start that presented this many frames worked (17 Oct: 300, about 10 s; it was 900: a look at the loading screen and a quit counted as a failure).
constexpr int kClearFrames = 300;
int g_level = -1;          // plan that is running (0 C, 1 A, 2 S), -1 = no plan applied
bool g_guarded = false;    // the guard file is in use
bool g_cleared = false;

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

bool Has(const char* name) {
    const char* e = std::getenv(name);
    return e && *e;
}

const char* LevelName(int level) { return level == 0 ? "C" : level == 1 ? "Cn" : level == 2 ? "A" : "S"; }

} // namespace

// Called after the settings file was read. Returns the names of the variables it set in `names` (so that a second reading can take them back).
int Apply(const char** names, int maxNames) {
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
        static const char* const kSpeed[][2] = {{"MSC_VITA_VGL_SKIP", "1"}, {"MSC_VGX_UNIDUP", "2"}, {"MSC_VGX_PROMOSMALL", "3"},
                                                {"MSC_SKIN_GPU", "1"}, {"MSC_VGX_SOFTINST", "16"}};
        char line[256];
        size_t n = 0;
        for (const auto& s : kSpeed) {
            if (Has(s[0])) continue;
            setenv(s[0], s[1], 0);
            const int w = std::snprintf(line + n, sizeof(line) - n, "%s%s=%s", n ? " " : "", s[0], s[1]);
            if (w > 0 && n + static_cast<size_t>(w) < sizeof(line)) n += static_cast<size_t>(w);
        }
        if (n) std::fprintf(stderr, "[plan] speed switches on by default (=0 in env.txt / np-player.txt turns one off): %s\n", line);
    }
    // W5 RT (docs/wave5-RT.md): the timers of the scene changes, of the GPU and of the first draw after a target change, and the input-delay record
    // (pad read -> the refresh that shows the frame) are on in every LAB build: their cost is a clock read per scene change and per timed draw, one
    // GPU notification and one wake-up of a watcher thread per scene, a few clock reads per frame; they print the [rt] and [lat] lines and the lat_*
    // fields of [frame-timing]. The player build has them off (MSC_VITA_RT_TIMERS / MSC_VGX_RTTIME / MSC_LAT in np-player.txt turn them on).
    {
        static const char* const kLab[][2] = {{"MSC_VITA_RT_TIMERS", "3"}, {"MSC_VGX_RTTIME", "1"}, {"MSC_LAT", "1"}};
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
    const char* want = std::getenv("MSC_PLAN");
    if (!player && !(want && *want)) return 0;
    if (Has("MSC_FRAMESEND") || Has("MSC_VGX_BACKEND_THREAD") || Has("MSC_VITA_CORE_FRAMESEND")) {
        std::fprintf(stderr, "[plan] the settings file sets the frame send / back end threads itself: no plan applied, no guard\n");
        return 0;
    }
    int level = 0;
    int failed = 0;
    if (want && *want && std::strcmp(want, "auto") != 0) {
        level = (*want == 'A' || *want == 'a') ? 2 : (*want == 'S' || *want == 's') ? 3 : ((*want == 'C' || *want == 'c') && (want[1] == 'n' || want[1] == 'N')) ? 1 : 0;
        std::fprintf(stderr, "[plan] MSC_PLAN=%s: plan %s chosen by hand, no guard\n", want, LevelName(level));
    } else {
        failed = ReadGuard();
        level = failed <= 0 ? 0 : failed == 1 ? 1 : failed == 2 ? 2 : 3;
        g_guarded = true;
        WriteGuard(failed + 1);
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
    return count < maxNames ? count : maxNames;
}

// From the platform's watch thread (a few seconds apart).
void Heartbeat(unsigned long long frames) {
    if (!g_guarded || g_cleared || frames < static_cast<unsigned long long>(kClearFrames)) return;
    g_cleared = true;
    WriteGuard(0);
    std::fprintf(stderr, "[plan] %llu frames presented: the guard is cleared (plan %s works)\n", frames, LevelName(g_level));
}

// A stall or a crash: the next start takes the next plan down.
void Failed(const char* why) {
    if (!g_guarded) return;
    g_cleared = true;   // (no later heartbeat may undo it)
    WriteGuard(g_level + 1);
    std::fprintf(stderr, "[plan] %s: the next start uses plan %s\n", why, LevelName(g_level + 1 > 3 ? 3 : g_level + 1));
}

int Level() { return g_level; }

} // namespace VitaPlan
