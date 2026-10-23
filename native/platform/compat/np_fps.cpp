// MSC_FPS: presentation rate policy and its controller. See include/np/fps.h, np_fps_ctl.h and docs/wave5-FPS.md.
#include "np/fps.h"
#include "np_fps_ctl.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" {
int64_t NpMonotonicNs(void) __attribute__((weak));        // platform/harness np_time.h
int64_t NpVirtualNowNs(void) __attribute__((weak));
int NpVirtualTimeActive(void) __attribute__((weak));
uint32_t NpVI_GetRetraceCount(void) __attribute__((weak));   // platform/sdk/src/vi_native.cpp: the count without the virtual-time side effects
// The console's measurements (platform/vita/src/np_fps_probe.cpp): per-core busy time and the calling thread's CPU time. Absent elsewhere.
int NpFpsPlatformProbe(NpFpsProbeOut* out) __attribute__((weak));
double NpFpsPlatformThreadClockMs(void) __attribute__((weak));
// W5 RT (the late wait, below): the game's own retrace calls, and the input-delay record of the frame (np/latency.h)
unsigned long VIGetRetraceCount(void) __attribute__((weak));
void VIWaitForRetrace(void) __attribute__((weak));
void NpLatLatch(void) __attribute__((weak));
// The renderer's predicted GPU time per frame at the smallest picture it would use in this scene (vgx_backend_gl.cpp, dynamic resolution); <= 0: unknown
float VgxGpuMinMs(void) __attribute__((weak));
}

namespace {

using npfps::Policy;
using npfps::kPolicyGame;
using npfps::kPolicy30;
using npfps::kPolicy60;
using npfps::kPolicyUnlocked;

// The mode the build starts in when MSC_FPS is not set. 0 game, 1 = 30, 2 = 60, 3 = unlocked, 4 = auto. The Vita starts in auto (23 Oct: 60 where
// every CPU core and the GPU hold it, the renderer's picture size helping within limits; the settings row's default in platform/vita/src/np_settings.cpp
// matches); host builds keep 30, which their deterministic runs and tests assume.
#if defined(__vita__)
const int kDefaultMode = 4;
#else
const int kDefaultMode = 1;
#endif

enum { kModeGame = 0, kMode30 = 1, kMode60 = 2, kModeUnlocked = 3, kModeAuto = 4 };

const double kNtscRefresh = 1001.0 / 60000.0;
const char* const kPolicyName[4] = {"game", "30", "60", "unlocked"};

// ---- the cost model (MSC_FPS_MODEL), for runs on the virtual clock ------------------------------------------------------------------------------------------
// One row per kind of scene: the game thread's cost P (ms per frame) + M (ms per simulation step), the frame send W, the decoding D, the back end B (ms per
// frame) and the audio / helper threads A (ms per refresh). Plan C: core 0 = game, core 1 = B + W, core 2 = D + audio. The charge to the virtual clock is
// the busiest core's time (the steady-state period of the pipeline). The numbers of "pred" are the predictions of docs/wave5-FPS.md (armhf instruction
// counts at 230 M instructions per second, the back end from the console's unit costs), not measurements of a console.
struct ModelRow {
    const char* label;
    unsigned stateMask;    // eGameTaskState bits (0 = any)
    int sceneId;           // top scene id of the front end (-1 = any)
    double P, M, W, D, B, A;
};
const ModelRow kPredRows[] = {
    // label                   states    scene   P     M    W    D     B     A
    {"title/menu",            0x00004,   -1,    4.0,  0.0, 2.0, 6.0,  6.0,  1.5},
    {"gameplay",              0x00003,   -1,    9.0,  5.9, 8.5, 19.6, 16.8, 1.5},
    {"replay",                0x00008,   -1,    9.0,  0.0, 8.5, 19.6, 16.8, 1.5},
    {"cut-scene",             0x00010,   -1,   20.0,  0.0, 8.5, 12.0, 14.0, 1.5},
    {"loading",               0xFFFF0000u, -1, 38.0,  0.0, 2.0, 6.0,  6.0,  1.5},
};
// (Replaced by measured tables in tools/fps; the unit-test model "const:" below is what the proof runs use.)

struct Model {
    bool on;
    bool table;
    double P, M, W, D, B, A;
    int lastRow;
} g_model = {false, false, 0, 0, 0, 0, 0, 0, -1};

struct State {
    bool inited;
    int mode;                  // what MSC_FPS asked for
    Policy policy;             // in force
    int logLevel;
    double costScale;
    npfps::Auto* ctl;
    NpFpsProbeFn probe;
    NpFpsThreadClockFn threadClock;
    // per frame accumulators
    int64_t waitStartNs;
    int64_t waitNs;
    int64_t waitNsPrev;
    double simStartMs;
    double simMsAcc;
    int simSteps;
    bool lockedThisFrame;
    // previous frame's cumulative probe values
    bool haveProbe;
    NpFpsProbeOut probePrev;
    // modelled costs of the frame that just ended (NpFpsModelCharge)
    bool haveModel;
    double modelCpu[npfps::kMaxCores];
    double modelTime[npfps::kMaxCores];
    double modelSimMs;
    int modelSteps;
    double lastInterval;
    // statistics
    uint64_t frames;
    uint64_t framesPolicy[4];
    uint64_t framesLocked;
    uint64_t hist[4];          // intervals of ~1, 2, 3, 4+ refreshes (all frames)
    uint64_t histWin[4];
    uint64_t held[6];          // refreshes between two hand-overs of a picture: 0 (two in one refresh), 1, 2, 3, 4, 5+
    uint32_t lastHandover;
    bool haveHandover;
    double intervalSumWin;
    int framesWin;
    // what the per-core probe delivered in the log window (auto only): frames with costs, frames the probe reported nothing for, frames without a probe, summed costs per core
    int winProbeOk, winProbeZero, winNoProbe;
    double winCpu[npfps::kMaxCores];
    uint64_t switches;
    double firstNow;
    double sumInterval;
    unsigned lastScene;
    double lastPred60, lastPredNow;
    int lastPredCore;
    bool summaryDone;
    uint64_t movieRuns;
} g;

double Refresh()
{
    return kNtscRefresh;
}

double NowSeconds()
{
    if (&NpVirtualTimeActive != 0 && NpVirtualTimeActive() && &NpVirtualNowNs != 0)
    {
        return (double)NpVirtualNowNs() * 1e-9;   // (without advancing the virtual clock)
    }
    if (&NpMonotonicNs != 0)
    {
        return (double)NpMonotonicNs() * 1e-9;
    }
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int64_t NowNs()
{
    return (int64_t)(NowSeconds() * 1e9);
}

double DefaultThreadClockMs()
{
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    {
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
    }
#endif
    return 0.0;
}

bool IEqual(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
    {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

void ParseModel(const char* spec)
{
    if (!spec || !*spec || IEqual(spec, "off") || IEqual(spec, "0")) return;
    if (IEqual(spec, "pred"))
    {
        g_model.on = true;
        g_model.table = true;
        return;
    }
    if (strncmp(spec, "const:", 6) == 0)
    {
        double v[6] = {0, 0, 0, 0, 0, 0};
        int n = sscanf(spec + 6, "%lf,%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        if (n >= 1)
        {
            g_model.on = true;
            g_model.table = false;
            g_model.P = v[0]; g_model.M = v[1]; g_model.W = v[2]; g_model.D = v[3]; g_model.B = v[4]; g_model.A = v[5];
            return;
        }
    }
    fprintf(stderr, "[fps] MSC_FPS_MODEL=%s not understood (pred, const:P,M,W,D,B,A or off): no model\n", spec);
}

void Summary();

void Init()
{
    if (g.inited) return;
    g.inited = true;
    g.mode = kDefaultMode;
    const char* e = getenv("MSC_FPS");
    if (e && *e)
    {
        if (IEqual(e, "30")) g.mode = kMode30;
        else if (IEqual(e, "60")) g.mode = kMode60;
        else if (IEqual(e, "auto")) g.mode = kModeAuto;
        else if (IEqual(e, "unlocked") || IEqual(e, "unlock") || IEqual(e, "free")) g.mode = kModeUnlocked;
        else if (IEqual(e, "game") || IEqual(e, "wii") || IEqual(e, "off") || IEqual(e, "0")) g.mode = kModeGame;
        else fprintf(stderr, "[fps] MSC_FPS=%s not understood (30, auto, 60, unlocked, game): using the default\n", e);
    }
    const char* l = getenv("MSC_FPS_LOG");
    g.logLevel = (l && *l) ? atoi(l) : 0;
    const char* cs = getenv("MSC_FPS_COST_SCALE");
    g.costScale = (cs && *cs) ? atof(cs) : 1.0;
    if (!(g.costScale > 0.0)) g.costScale = 1.0;
    ParseModel(getenv("MSC_FPS_MODEL"));
    if (!g.threadClock) g.threadClock = (&NpFpsPlatformThreadClockMs != 0) ? NpFpsPlatformThreadClockMs : DefaultThreadClockMs;
    if (!g.probe && &NpFpsPlatformProbe != 0) g.probe = NpFpsPlatformProbe;
    switch (g.mode)
    {
    case kModeGame: g.policy = kPolicyGame; break;
    case kMode30: g.policy = kPolicy30; break;
    case kMode60: g.policy = kPolicy60; break;
    case kModeUnlocked: g.policy = kPolicyUnlocked; break;
    default: g.policy = kPolicy30; break;   // auto starts at 30
    }
    if (g.mode == kModeAuto)
    {
        g.ctl = new npfps::Auto();
    }
    atexit(Summary);
    static const char* const modeName[5] = {"game (the build's own swap modes)", "30", "60", "unlocked", "auto"};
    fprintf(stderr, "[fps] MSC_FPS: %s%s (MSC_FPS=30|auto|60|unlocked|game); policy now %s%s%s\n", modeName[g.mode], (e && *e) ? "" : " (default)", kPolicyName[g.policy],
            g_model.on ? "; cost model on (virtual clock runs)" : "", g.costScale != 1.0 ? "; cost scale set" : "");
}

void Summary()
{
    if (!g.inited || g.summaryDone) return;
    g.summaryDone = true;
    if (g.frames == 0) return;
    fprintf(stderr,
            "[fps] summary: mode=%d frames=%llu (game %llu, 30 %llu, 60 %llu, unlocked %llu, cut-scene lock %llu) frame lengths in refreshes 1/2/3/4+ = %llu/%llu/%llu/%llu "
            "mean %.2f ms; pictures stayed 0/1/2/3/4/5+ refreshes = %llu/%llu/%llu/%llu/%llu/%llu; switches=%llu\n",
            g.mode, (unsigned long long)g.frames, (unsigned long long)g.framesPolicy[0], (unsigned long long)g.framesPolicy[1],
            (unsigned long long)g.framesPolicy[2], (unsigned long long)g.framesPolicy[3], (unsigned long long)g.framesLocked, (unsigned long long)g.hist[0],
            (unsigned long long)g.hist[1], (unsigned long long)g.hist[2], (unsigned long long)g.hist[3], g.sumInterval * 1000.0 / (double)(g.frames ? g.frames : 1),
            (unsigned long long)g.held[0], (unsigned long long)g.held[1], (unsigned long long)g.held[2], (unsigned long long)g.held[3], (unsigned long long)g.held[4],
            (unsigned long long)g.held[5], (unsigned long long)g.switches);
}

const ModelRow* FindRow(unsigned state, int scene)
{
    for (size_t i = 0; i < sizeof(kPredRows) / sizeof(kPredRows[0]); ++i)
    {
        const ModelRow& r = kPredRows[i];
        if ((r.stateMask == 0 || (state & r.stateMask) != 0) && (r.sceneId < 0 || r.sceneId == scene)) return &r;
    }
    return 0;
}

} // namespace

// W5 BOOT: the start-up at a picture per refresh. Until the game shows its boot screens (NpBootMark "boot screens" -> NpFpsBootDone) nothing of
// the game is on screen (the loading screen covers it) and the game's loader advances a step per frame (the maintainer console log of 17 Oct: frames 1-81
// at 33 ms each, almost none of it work), so those frames are paced per refresh instead of every second one. Only where the policy is 30 and the
// game asked for a retrace wait; a cut-scene lock or swap mode 0 is left alone. MSC_BOOT_FPS=60 (default on the Vita) | 0 | title (until the title
// screen: the boot screens and the intro movie too, where the front end's loading goes on behind them; menus run on elapsed time and movies by the
// retrace count, so neither changes speed: np/fps.h).
namespace {
int g_bootFast = -1;   // -1 undecided, 1 on (until the boot screens), 0 off / over
bool g_bootUntilTitle = false;
bool BootFast()
{
    if (g_bootFast < 0)
    {
        const char* e = getenv("MSC_BOOT_FPS");
        g_bootUntilTitle = e && IEqual(e, "title");
#if defined(__vita__)
        g_bootFast = (e && *e && !g_bootUntilTitle && atoi(e) == 0) ? 0 : 1;
#else
        g_bootFast = (e && *e && (g_bootUntilTitle || atoi(e) != 0)) ? 1 : 0;
#endif
        if (g_bootFast) fprintf(stderr, "[fps] start-up at a picture per refresh until the game's %s (MSC_BOOT_FPS=0 turns it off)\n", g_bootUntilTitle ? "title screen" : "boot screens");
    }
    return g_bootFast == 1;
}
} // namespace

extern "C" {

void NpFpsBootDone(int atTitle)
{
    if (g_bootFast < 0) BootFast();
    if (g_bootUntilTitle && !atTitle) return;
    if (g_bootFast == 1) fprintf(stderr, "[fps] start-up over: back to the %s policy\n", kPolicyName[g.policy]);
    g_bootFast = 0;
}

int NpFpsMode(void)
{
    Init();
    return g.mode;
}

int NpFpsDisplayInterval(void)
{
    // Read on the display-queue thread (vgx_host_vita.cpp, the gate that holds a picture to its vblank): how many vblanks a picture is shown for.
    // Never starts the policy from that thread (the game thread's first frame does): until then there is no gate.
    if (!g.inited) return 0;
    switch (g.policy)
    {
    case kPolicy30: return g_bootFast == 1 ? 1 : 2;
    case kPolicy60: return 1;
    default: return 0;   // unlocked, the build's own pacing: no gate
    }
}

int NpFpsPolicy(void)
{
    Init();
    return (int)g.policy;
}

void NpFpsSetProbe(NpFpsProbeFn fn)
{
    g.probe = fn;
}

void NpFpsSetThreadClock(NpFpsThreadClockFn fn)
{
    g.threadClock = fn;
}

int NpFpsSwapMode(int gameMode)
{
    Init();
    if (g.policy == kPolicyGame) return gameMode;
    if (gameMode == 2)
    {
        g.lockedThisFrame = true;   // the game's own cut-scene lock stays in every mode
        return 2;
    }
    switch (g.policy)
    {
    case kPolicy30: return BootFast() ? (gameMode == 0 ? 0 : 1) : 4;
    case kPolicy60: return gameMode == 0 ? 0 : 1;
    case kPolicyUnlocked: return gameMode == 0 ? 0 : 3;
    default: return gameMode;
    }
}

void NpFpsSwapWaitBegin(void)
{
    if (g.policy == kPolicyGame) return;
    g.waitStartNs = NowNs();
}

void NpFpsSwapWaitEnd(void)
{
    if (&NpLatLatch != 0) NpLatLatch();   // W5 RT: the frame is handed over now (np/latency.h)
    if (&NpVI_GetRetraceCount != 0)
    {
        // the refresh the picture of this frame will appear in is the next one after this count: the difference to the previous picture is how many
        // refreshes that one stayed on the screen (1 = a new picture every refresh)
        const uint32_t r = NpVI_GetRetraceCount();
        if (g.haveHandover)
        {
            const uint32_t d = r - g.lastHandover;
            ++g.held[d > 5 ? 5 : d];
        }
        g.lastHandover = r;
        g.haveHandover = true;
    }
    if (g.policy == kPolicyGame) return;
    const int64_t d = NowNs() - g.waitStartNs;
    if (d > 0) g.waitNs += d;
}

void NpFpsSimBegin(void)
{
    if (g.mode != kModeAuto && g.logLevel == 0) return;
    g.simStartMs = g.threadClock ? g.threadClock() : 0.0;
}

void NpFpsSimStep(void)
{
    ++g.simSteps;
}

void NpFpsSimEnd(void)
{
    if (g.mode != kModeAuto && g.logLevel == 0) return;
    const double t = g.threadClock ? g.threadClock() : 0.0;
    if (t > g.simStartMs) g.simMsAcc += t - g.simStartMs;
}

void NpFpsMovieEnd(unsigned decoded, unsigned retraces)
{
    ++g.movieRuns;
    fprintf(stderr, "[fps] movie: %u decode calls in %u retraces (%.3f per retrace; the speed check is the length in retraces: 2 per frame of a 29.97 fps movie) under policy %s\n", decoded, retraces,
            retraces ? (double)decoded / (double)retraces : 0.0, kPolicyName[g.policy]);
}

long long NpFpsModelCharge(unsigned taskState, int sceneId, unsigned steps)
{
    Init();
    if (!g_model.on) return 0;
    double P = g_model.P, M = g_model.M, W = g_model.W, D = g_model.D, B = g_model.B, A = g_model.A;
    if (g_model.table)
    {
        const ModelRow* r = FindRow(taskState, sceneId);
        if (!r) r = &kPredRows[0];
        P = r->P; M = r->M; W = r->W; D = r->D; B = r->B; A = r->A;
        const int idx = (int)(r - kPredRows);
        if (idx != g_model.lastRow)
        {
            g_model.lastRow = idx;
            if (g.logLevel >= 1) fprintf(stderr, "[fps] model row: %s (task state 0x%X scene %d)\n", r->label, taskState, sceneId);
        }
    }
    const double R = Refresh();
    const double timeBased = A * (g.lastInterval > 0 ? g.lastInterval / R : 1.0);   // the audio and helper threads follow time
    const double sim = M * (double)steps;
    g.modelCpu[0] = P + sim;
    g.modelCpu[1] = B + W;
    g.modelCpu[2] = D + timeBased;
    g.modelCpu[3] = 0;
    g.modelTime[0] = g.modelTime[1] = g.modelTime[3] = 0;
    g.modelTime[2] = timeBased;
    g.modelSimMs = sim;
    g.modelSteps = (int)steps;
    g.haveModel = true;
    double busiest = g.modelCpu[0];
    if (g.modelCpu[1] > busiest) busiest = g.modelCpu[1];
    if (g.modelCpu[2] > busiest) busiest = g.modelCpu[2];
    return (long long)(busiest * 1e6);
}

void NpFpsBeginFrame(float realSeconds, unsigned taskState)
{
    Init();
    const double now = NowSeconds();
    const double R = Refresh();
    ++g.frames;
    if (g.frames == 1)
    {
        g.firstNow = now;
        g.lastScene = taskState;
        g.waitNs = 0;
        g.simMsAcc = 0;
        g.simSteps = 0;
        g.lockedThisFrame = false;
        if (g.probe)
        {
            NpFpsProbeOut o;
            memset(&o, 0, sizeof o);
            if (g.probe(&o)) { g.probePrev = o; g.haveProbe = true; }
        }
        return;   // no previous frame yet
    }
    const double interval = realSeconds > 0.0f ? (double)realSeconds : 0.0;
    g.lastInterval = interval;
    g.sumInterval += interval;
    const double ratio = interval / R;
    ++g.framesPolicy[g.policy];
    if (g.lockedThisFrame) ++g.framesLocked;
    const int bin = ratio < 1.5 ? 0 : (ratio < 2.5 ? 1 : (ratio < 3.5 ? 2 : 3));
    ++g.hist[bin];
    ++g.histWin[bin];
    g.intervalSumWin += interval;
    ++g.framesWin;

    if (g.mode == kModeAuto)
    {
        npfps::FrameIn in;
        memset(&in, 0, sizeof in);
        in.now = now;
        in.interval = interval;
        in.scene = taskState;
        in.locked = g.lockedThisFrame;
        in.steps = g.simSteps;
        in.simMs = g.simMsAcc;
        in.gameCore = 0;
        in.gpuMinMs = (&VgxGpuMinMs != 0) ? (double)VgxGpuMinMs() : 0.0;
        if (g_model.on)
        {
            in.costsValid = g.haveModel;
            in.steps = g.modelSteps;
            in.simMs = g.modelSimMs;
            for (int c = 0; c < npfps::kMaxCores; ++c) { in.cpuMs[c] = g.modelCpu[c]; in.timeMs[c] = g.modelTime[c]; }
        }
        else if (g.probe)
        {
            NpFpsProbeOut o;
            memset(&o, 0, sizeof o);
            if (g.probe(&o) && g.haveProbe)
            {
                in.costsValid = true;
                in.gameCore = o.gameCore;
                for (int c = 0; c < npfps::kMaxCores; ++c)
                {
                    double dc = o.cpuMs[c] - g.probePrev.cpuMs[c];
                    double dt = o.timeMs[c] - g.probePrev.timeMs[c];
                    in.cpuMs[c] = (dc > 0 ? dc : 0) * g.costScale;
                    in.timeMs[c] = (dt > 0 ? dt : 0) * g.costScale;
                }
                in.simMs *= g.costScale;
                g.probePrev = o;
                g.haveProbe = true;
                // a probe that reports no CPU time at all for a whole frame is not delivering (a real frame costs something somewhere):
                // fall back to the game thread's own busy time rather than read "free" as "can hold 60"
                double total = 0;
                for (int c = 0; c < npfps::kMaxCores; ++c) total += in.cpuMs[c];
                if (total < 0.1) in.costsValid = false;
                if (in.costsValid)
                {
                    ++g.winProbeOk;
                    for (int c = 0; c < npfps::kMaxCores; ++c) g.winCpu[c] += in.cpuMs[c];
                }
                else ++g.winProbeZero;
            }
            else if (g.probe(&o))
            {
                g.probePrev = o;
                g.haveProbe = true;
            }
            else ++g.winNoProbe;
        }
        else if (!g_model.on) ++g.winNoProbe;
        if (!in.costsValid)
        {
            const double busy = interval * 1000.0 - (double)g.waitNs * 1e-6;
            in.gameBusyMs = (busy > 0 ? busy : 0) * g.costScale;
            in.simMs *= 1.0;
        }
        const npfps::Decision d = g.ctl->Frame(in);
        g.lastPred60 = g.ctl->LastPrediction().max60;
        g.lastPredNow = g.ctl->LastPrediction().maxNow;
        g.lastPredCore = g.ctl->LastPrediction().core;
        if (d.switched)
        {
            ++g.switches;
            fprintf(stderr, "[fps] auto: %s -> %s at t=%.3f s, frame %llu, task state 0x%X (%s; predicted 60 Hz cost of the busiest core %.1f ms (core %d), measured %.1f ms, "
                    "refresh %.1f ms; penalty %d, cool-down until t=%.1f)\n",
                    kPolicyName[g.policy], kPolicyName[d.policy], now, (unsigned long long)g.frames, taskState, d.reason, g.lastPred60, g.lastPredCore, g.lastPredNow, R * 1000.0,
                    g.ctl->Penalty(), g.ctl->CooldownUntil());
            g.policy = d.policy;
        }
    }
    if (taskState != g.lastScene)
    {
        if (g.logLevel >= 1) fprintf(stderr, "[fps] t=%.3f frame %llu task state 0x%X -> 0x%X (policy %s)\n", now, (unsigned long long)g.frames, g.lastScene, taskState, kPolicyName[g.policy]);
        g.lastScene = taskState;
    }
    if (g.logLevel >= 2)
    {
        fprintf(stderr, "[fps-frame] %llu t=%.4f interval=%.2f ms policy=%s wait=%.2f ms steps=%d sim=%.2f ms\n", (unsigned long long)g.frames, now, interval * 1000.0, kPolicyName[g.policy],
                (double)g.waitNs * 1e-6, g.simSteps, g.simMsAcc);
    }
    if (g.logLevel >= 1 && g.frames % 120 == 0)
    {
        char probeText[160];
        if (g.mode != kModeAuto) snprintf(probeText, sizeof probeText, "probe not read (MSC_FPS is not auto)");
        else if (g_model.on) snprintf(probeText, sizeof probeText, "cost model");
        else if (g.winProbeOk > 0)
        {
            const double n = (double)g.winProbeOk;
            snprintf(probeText, sizeof probeText, "probe ok in %d frames (%d without costs): cpu ms/frame core0 %.1f core1 %.1f core2 %.1f core3 %.1f", g.winProbeOk, g.winProbeZero,
                     g.winCpu[0] / n, g.winCpu[1] / n, g.winCpu[2] / n, g.winCpu[3] / n);
        }
        else if (g.winProbeZero > 0) snprintf(probeText, sizeof probeText, "probe reports no CPU time (%d frames): the game thread's busy time is used", g.winProbeZero);
        else snprintf(probeText, sizeof probeText, "no probe (%d frames): the game thread's busy time is used", g.winNoProbe);
        fprintf(stderr, "[fps] t=%.3f frame %llu policy=%s interval mean %.2f ms, in refreshes 1/2/3/4+ = %llu/%llu/%llu/%llu of %d frames; predicted 60 Hz cost %.1f ms (core %d) measured %.1f ms; %s\n",
                now, (unsigned long long)g.frames, kPolicyName[g.policy], g.framesWin ? g.intervalSumWin * 1000.0 / g.framesWin : 0.0, (unsigned long long)g.histWin[0],
                (unsigned long long)g.histWin[1], (unsigned long long)g.histWin[2], (unsigned long long)g.histWin[3], g.framesWin, g.lastPred60, g.lastPredCore, g.lastPredNow, probeText);
        g.winProbeOk = g.winProbeZero = g.winNoProbe = 0;
        for (int c = 0; c < npfps::kMaxCores; ++c) g.winCpu[c] = 0;
        memset(g.histWin, 0, sizeof g.histWin);
        g.intervalSumWin = 0;
        g.framesWin = 0;
    }
    g.waitNsPrev = g.waitNs;
    g.waitNs = 0;
    g.simMsAcc = 0;
    g.simSteps = 0;
    g.lockedThisFrame = false;
    g.haveModel = false;
}

} // extern "C"

// ---- W5 RT: the late wait (MSC_LATE_WAIT; np/fps.h) -----------------------------------------------------------------------------------------------
// Under policy 30 swap_Post waits (swap mode 4) until two retraces have passed since the previous hand-over, BEFORE this frame is handed over:
// the pads were read at the start of the frame (FixedUpdateTask), and a frame that is ready early sits out the rest of the period on the game
// thread before it goes to the GPU and the display. So the picture of a pad read is handed over a whole period after it, whatever the frame cost.
//  MSC_LATE_WAIT=1 (hand over first): the frame is handed over (sent) as soon as it is built and the same wait is made right after the send
//    (glplatSendFrame / glplatSendFrameForked: NpFpsLateWaitAfterSend), until two retraces after the previous frame's wait ended. Every game task
//    keeps its place against the wait (the frame starts at the same retrace and every task measures the same dt); only the hand-over moves before
//    it. (Two retraces after the previous HAND-OVER would be wrong here: the hand-overs now come early; a frame whose work crosses a retrace
//    after the frame start would push the next one a retrace further, 20 fps: tools/rt/late_sim.cpp shows it.) The display gate (vgx
//    GateDisplay) keeps every picture on the screen for two refreshes whatever the hand-over times do.
//  MSC_LATE_WAIT=2 (wait before the pad read; experiment, not for the default): the wait moves to the start of the next frame's FixedUpdateTask,
//    just before the pads are read, until two retraces after the previous frame's pad read. FixedUpdateTask's own dt is then measured before the
//    wait (nlTask reads the ticker before Run) and the tasks before it (BeginFrame, the pad rumble, loading) and the network task run before the
//    wait: on host32 the menus react 2-3 frames differently (a scripted match diverges). A frame in which FixedUpdateTask does not run makes the
//    owed wait in swap_Post (NpFpsLateWaitRun(1)), against the previous hand-over as before.
namespace {
struct LateWait {
    int on = -1;            // MSC_LATE_WAIT: 0 off, 1 hand over first, 2 wait before the pad read
    bool sendOwed = false;  // (1) this frame skipped its wait in swap_Post: it is made after the send
    unsigned long sendRef = 0;
    bool armNext = false;   // this frame used swap mode 4 with the late wait: its hand-over owes the next frame a wait
    bool owed = false;
    unsigned long ref = 0;  // the retrace count the owed wait waits for
    unsigned long anchor = 0;   // the retrace count when this frame's wait ended (its pad read)
    bool anchored = false;      // ... set in this frame
    uint32_t early = 0, fallback = 0;   // waits made where they belong (after the send / before the pad read), (2) in swap_Post
    double waitedMs = 0;
} g_late;
bool LateOn()
{
    if (g_late.on < 0)
    {
        const char* e = getenv("MSC_LATE_WAIT");
        g_late.on = (e && *e) ? atoi(e) : 0;
        if (g_late.on < 0 || g_late.on > 2) g_late.on = 1;
        if (g_late.on > 0 && (&VIGetRetraceCount == 0 || &VIWaitForRetrace == 0)) g_late.on = 0;
        if (g_late.on == 1) fprintf(stderr, "[fps] late wait on (MSC_LATE_WAIT=1): a frame is handed over before the 30 fps wait\n");
        if (g_late.on == 2) fprintf(stderr, "[fps] late wait on (MSC_LATE_WAIT=2): the 30 fps wait is made before the pads are read\n");
    }
    return g_late.on > 0;
}
} // namespace

extern "C" {

int NpFpsLateWait(int swapMode, unsigned long refRetrace)
{
    if (swapMode != 4 || !LateOn()) return 0;
    if (g_late.on == 1)
    {
        // two retraces after the previous frame's wait ended; refRetrace (two after the previous hand-over, the wait swap_Post makes) when the
        // previous frame made none
        g_late.sendOwed = true;
        g_late.sendRef = g_late.anchored ? g_late.anchor + 2 : refRetrace;
        g_late.anchored = false;
        return 1;
    }
    g_late.armNext = true;
    return 1;
}

void NpFpsLateWaitAfterSend(void)
{
    if (!g_late.sendOwed) return;
    g_late.sendOwed = false;
    const bool count = g.policy != kPolicyGame;
    const int64_t t0 = NowNs();
    while ((long)(VIGetRetraceCount() - g_late.sendRef) < 0) VIWaitForRetrace();
    g_late.anchor = VIGetRetraceCount();
    g_late.anchored = true;
    const int64_t d = NowNs() - t0;
    if (d > 0)
    {
        if (count) g.waitNs += d;   // (the controller of MSC_FPS=auto counts waits, not cost: this one is swap_Post's)
        g_late.waitedMs += d / 1e6;
    }
    ++g_late.early;
}

void NpFpsLateWaitOwe(unsigned long refRetrace)
{
    if (!g_late.armNext) return;
    g_late.armNext = false;
    g_late.owed = true;
    // two retraces after this frame's pad read; refRetrace (two after the hand-over) when the frame had no wait of its own
    g_late.ref = g_late.anchored ? g_late.anchor + 2 : refRetrace;
    g_late.anchored = false;
}

void NpFpsLateWaitRun(int where)
{
    if (!g_late.owed) return;
    g_late.owed = false;
    const bool count = g.policy != kPolicyGame;
    const int64_t t0 = NowNs();
    while ((long)(VIGetRetraceCount() - g_late.ref) < 0) VIWaitForRetrace();
    if (where == 0)
    {
        g_late.anchor = VIGetRetraceCount();
        g_late.anchored = true;
    }
    const int64_t d = NowNs() - t0;
    if (d > 0)
    {
        if (count && where == 0) g.waitNs += d;   // (the controller of MSC_FPS=auto counts waits, not cost; in swap_Post NpFpsSwapWaitEnd counts it)
        g_late.waitedMs += d / 1e6;
    }
    if (where == 0) ++g_late.early; else ++g_late.fallback;
}

void NpFpsLateWaitStats(uint32_t* early, uint32_t* fallback, double* waitedMs)
{
    *early = g_late.early;
    *fallback = g_late.fallback;
    *waitedMs = g_late.waitedMs;
    g_late.early = g_late.fallback = 0;
    g_late.waitedMs = 0;
}

} // extern "C"
