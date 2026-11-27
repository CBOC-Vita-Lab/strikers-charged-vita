// MSC_FPS: presentation rate policy and its controller. See include/np/fps.h, np_fps_ctl.h and docs/wave5-FPS.md.
#include "np/fps.h"
#include "np/gsplit.h"   // gt27 (MSC_GSPLIT): the late pad sleep is a wait, the per-core probe its own phase
#include "np_fps_ctl.h"
#include "np_fps_between.h"
#include "np_fps_fif.h"
#include "np_slip.h"         // wave 9 (MSC_SLIP_BUDGET_MS): the slip budget, pure code
#include "np_speed.h"        // wave 9 (MSC_SPEED_LOG): the game-speed meter, pure code
#include "np_fps_cadence.h"   // wave 7 (MSC_CADENCE): the scene classes' own rates, decided at the cuts
#include "np_fps_cad3.h"      // wave 11 (MSC_HELD_CADENCE=auto): a steady 3-refresh cadence for a held scene that cannot make 33 ms
#include "np_fps_bp.h"
#include "np_late_cause.h"   // lessons17 (MSC_LATE_CAUSE): which lane made a late picture late, pure code
#include "np_head17.h"        // wave 17 (MSC_HEAD_STAT): the headroom line, pure code
#include "np_crowdgov.h"      // crowd25 (MSC_CROWD_GOV): the adaptive crowd-cost governor, pure code
#include "np_pair_rate.h"    // pacing13 (MSC_PAIR_RATE): the pair-or-even-30 decision, pure code
#include "np_game_clock.h"   // pacing12 (MSC_CLOCK=debt): the game clock of live play, pure code        // wave 11 (MSC_LAT_BP): back-pressure before the pad, pure code
#include "np_lat16.h"   // wave 11 (press16, MSC_LAT_SAMPLE): the pad read aligned to the controller samples
#include "../sdk/input/np_feel.h"   // wave 6 (MSC_PAD_LATE): the late-latch controller, pure code
#include "np_fps_phase.h"                // pace10 (MSC_PHASE_SYNC): the late start with fault attribution, pure code
#include "np_fps45.h"                     // helper f45 (MSC_FPS=45): the 1, 1, 2 cadence, its display gate, show-time clock and sticky fallback, pure code
#include "np_show_clock.h"                // (LatencyLearner, for the 45 display gate)
#include "np_fps_tiermem.h"                // pace13 (MSC_TIER_MEMORY): the tier a stadium settled at, remembered per picture mode, pure code
#include "np_fps_tier.h"                  // Fable (MSC_LOCK60_TIERS): Smooth 60 in three tiers, 60 -> the strict 45 -> the full-size 30

#include <atomic>
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
int np_display_last_vblank(uint32_t* vcount, uint64_t* ns) __attribute__((weak));   // platform/sdk/vita/np_plat_vita.cpp: the last vblank the VI thread saw and its time (the 45 lead's margin)
// The console's measurements (platform/vita/src/np_fps_probe.cpp): per-core busy time and the calling thread's CPU time. Absent elsewhere.
int NpFpsPlatformProbe(NpFpsProbeOut* out) __attribute__((weak));
double NpFpsPlatformThreadClockMs(void) __attribute__((weak));
// W5 RT (the late wait, below): the game's own retrace calls, and the input-delay record of the frame (np/latency.h)
unsigned long VIGetRetraceCount(void) __attribute__((weak));
void VIWaitForRetrace(void) __attribute__((weak));
void NpLatLatch(void) __attribute__((weak));
// The renderer's predicted GPU time per frame at the smallest picture it would use in this scene (vgx_backend_gl.cpp, dynamic resolution); <= 0: unknown
float VgxGpuMinMs(void) __attribute__((weak));
double NpFsJoinWaitedSeconds(void) __attribute__((weak));   // pace24 (platform/vita/src/np_framesend.cpp): the game thread's blocked time in NpFsJoin so far, s (a log figure only)
double NpLaneWaitedSeconds(void) __attribute__((weak));   // hold60b (platform/vita/src/np_framesend.cpp): the game thread's blocked time on the helper lane so far (the async blend's join), s
int VgxHoldStadium(void) __attribute__((weak));   // pace13 (MSC_TIER_MEMORY): the stadium id of the match (-1: not known), vgx_backend_gl.cpp
// The renderer's Mega Strike scene (vgx_backend_gl.cpp): AUTO keeps those at 30 with the full-size picture (MSC_FPS_MEGA=auto lets them try 60)
int VgxMegaActive(void) __attribute__((weak));
const char* NpPresentationFunction(void) __attribute__((weak));   // scenes30 (Presentation.cpp): the match presentation's current function (GameBegin, Goal*, Megastrike*, ...), for [head]'s moments
// 1.3 MSC_BG30 (np/bg30.h, platform/compat/np_bg30.cpp): the per-picture cost by kind, and what a background-at-30 pair saves while live play is held at 30
void NpBg30NoteCost(double perPictureMs) __attribute__((weak));
int NpBg30HeldEligible(void) __attribute__((weak));
int NpBg30Phase(void) __attribute__((weak));
int NpBg30Mode(void) __attribute__((weak));
// wave 10 (bg30b): the renderer's GPU ms of the last picture measured by phase (1 full, 2 light), 0 unknown (renderer/src/vgx_backend_gl.cpp)
float VgxBg30GpuMs(int phase) __attribute__((weak));
extern "C" int NpFpsPairReport(int phase, float gameMs, float gpuMs, int heldRefreshes, int live, double now);
static bool PairControlled();
double NpBg30PairSavingMs(void) __attribute__((weak));
// 1.2: the renderer is told when the scene is one the frame-rate policy holds at 30 (replay, celebration, cut-scene: MSC_FPS_REPLAY), so its dynamic
// resolution may go deeper there to keep the 30 even; and it tells the controller that it changed its picture size a few frames ago
void VgxPacedSceneHint(int on) __attribute__((weak));
void VgxCadenceFull(int on) __attribute__((weak));   // wave 7 MSC_CADENCE_FULL: the scene is held at 30 by the cadence policy: drawn at the game's own size (vgx_backend_gl.cpp)
int VgxDynResGrace(void) __attribute__((weak));
int VgxDynResRoom(void) __attribute__((weak));   // 1.2 (MSC_FPS_LADDERHOLD): the ladder can still go down at 60
void VgxWarmGoal(void) __attribute__((weak));    // wave 7 (MSC_DL_WARM): a goal in play: the front end re-decodes the lists of the scenes that follow, a few a picture
void VgxWarmScene(int cls) __attribute__((weak)); // wave 7: the scene class now (0 other, 1 open, 2 replay, 3 cut-scene): what the front end learns and warms
// 1.3 (MSC_PRESENT_SYNC): the display's own vblank counter, the clock the display callback holds a picture by; the number of the frame just handed over (np_latency.cpp)
int sceDisplayGetVcount(void) __attribute__((weak));
uint64_t NpLatHandedOverTag(void) __attribute__((weak));
// pace18 (src/Game/NpSimRun17.cpp): where np_sim sits, one bit a core (0 = not known / not pinned)
unsigned NpSimtCoreMask(void) __attribute__((weak));
}
bool NpLanActive() __attribute__((weak));   // a LAN or online session exists (platform/sdk/net/np_lan.h)
// wave 11 (MSC_HELD_CADENCE): a LAN, online or recorded/played-back game (Game/InputManager.cpp). It dereferences the session objects, so they are tested first (all weak: the host units link this file alone).
bool IsNetworkOrRecordedGame() __attribute__((weak));
extern void* g_pNetworkSessionBase __attribute__((weak));
extern void* gNetworkInputRecording __attribute__((weak));
#ifndef NP_PROBE_EVERY_BREAK
#define NP_PROBE_EVERY_BREAK 0   // host test mutants (tools/fps/fps_probe_unit.cpp, case every): 1 every picture reads (no skipping), 2 the force rules are ignored (never a forced read)
#endif
static bool NpFpsNetOrRecorded()
{
    if (&IsNetworkOrRecordedGame == 0 || &g_pNetworkSessionBase == 0 || &gNetworkInputRecording == 0) return false;   // (no game linked: a host unit)
    if (g_pNetworkSessionBase == 0 || gNetworkInputRecording == 0) return true;   // (session objects not made yet: not a moment to change a pace)
    return IsNetworkOrRecordedGame();
}
// wave 9 helper smooth (np_smooth.cpp, MSC_SMOOTH_CLOCK): the game time of a frame under a held cadence = its picture's predicted show time
extern "C" double NpSmoothFrameBegin(double realSeconds, int interval, int haveVcount, uint32_t vcount, int livePlay) __attribute__((weak));   // (weak: the host units link np_fps.cpp alone)
extern "C" void NpSmoothHanded(uint64_t tag) __attribute__((weak));

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

enum { kModeGame = 0, kMode30 = 1, kMode60 = 2, kModeUnlocked = 3, kModeAuto = 4, kMode45 = 5 };   // (kMode45: helper f45, MSC_FPS=45 / MSC_CADENCE45=1: np_fps45.h; policy 30 outside live play)
enum { kTaskPaused = 1, kTaskGameplay = 2, kTaskReplay = 8, kTaskNis = 16, kTaskDebugReplay = 131072, kTaskHomeMenu = 33554432 };   // eGameTaskState (Game/Task/GameTaskState.h)

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
    int replayMode;            // MSC_FPS_REPLAY: 0 fit (default), 1 = 30, 2 = auto
    int sceneNoPause;          // MSC_FPS_SCENE_PAUSE=0 clears it: 1 (default) = pausing or the home-button menu does not start a new scene for the controller
    int hyst;                  // MSC_FPS_HYST: 2 = the 1.2 thresholds (default), 1 = the 1.1 ones
    bool lock60;               // 1.3 MSC_LOCK60: the LOCKED 60 controller (np_fps_ctl.h Config::Lock60); only under auto
    int betweenMode;           // MSC_FPS_BETWEEN: 0 = held 30 (default), 1 = paced (see NpFpsPaced)
    npfps::Between* between;   // ... when (np_fps_between.h)
    bool cadMenus;             // wave 7 MSC_CADENCE_MENUS: the menus (front end, stadium viewer, pause) are a held class too
    bool cadMenuHeld;          // (the last frame's verdict: the renderer's paced-scene hint)
    bool cadFull;              // wave 7 MSC_CADENCE_FULL: tell the renderer which scenes the cadence policy holds at 30 (it draws them at the game's own size)
    npfps::Cadence* cad;       // wave 7 MSC_CADENCE: the scene classes' own rates (np_fps_cadence.h); null when off
    npfps::Cad3* cad3;         // wave 11 MSC_HELD_CADENCE=auto (np_fps_cad3.h); null when off
    int cad3Period;            // refreshes a held picture is shown for by the late wait: 2, or 3 while the steady cadence is in force
    uint64_t cad3Frames, cad3Logged;
    uint64_t cadCuts, cadHolds, cadResumes, cadGo60, cadDefers, cadHoldFrames, cadLogged;
    bool cadPacedBack;         // the cut back to open play found AUTO's free run in force before the scene: Between::Resume at this frame
    bool paced;                // in force now: AUTO's 30 is shown at the pipeline's own rate
    bool lastCostsValid;       // the controller's last frame had per-core costs
    int presentSync;           // MSC_PRESENT_SYNC: -1 not set, 0 off, 1 measure, 2 aligned
    uint32_t turnVcount;       // the display's vblank when this frame began
    bool haveTurn;
    uint64_t framesPaced, pacedChanges;
    uint64_t bg30Discounted;   // (MSC_BG30=2) pictures whose cost the controller heard with a pair's saving taken off
    NpFpsProbeFn probe;
    NpFpsThreadClockFn threadClock;
    // per frame accumulators
    int64_t waitStartNs;
    int64_t waitNs;
    int64_t waitNsPrev;
    double simStartMs;
    double simMsAcc;
    int64_t simStartNs;      // pace24: the wall clock at NpFpsSimBegin (the steps' wall ms, MSC_HOLD60_WALLSIM)
    double simWallAccMs;
    int simSteps;
    double simWinMsAcc, simJoinMsAcc, simWallMsAcc;   // pace18 (np_simpace18.h): the picture's window, join wait and np_sim wall ms (MSC_SIMT=1|2; filled by NpFpsSimOverlapNote)
    int simNoteMode;                                    // the mode of the last note (0 none)
    uint64_t simNoteFrame;                              // the frame it came in
    bool lockedThisFrame;
    // previous frame's cumulative probe values
    bool haveProbe;
    NpFpsProbeOut probePrev;
    // wave 18 (MSC_FPS_PROBE_EVERY): the probe is read once in N pictures; the pictures between use the last window's per-picture mean
    int probeEvery;            // 1 = every picture (today)
    int probeAge;              // pictures since the last real read, this one included (only counted when probeEvery > 1)
    bool probeCacheOk;         // the cache below holds a valid window
    double probeAvgCpu[npfps::kMaxCores], probeAvgTime[npfps::kMaxCores];   // ms per picture of the last window, before costScale
    int probeAvgCore;
    double probeIv;            // the previous picture's interval (a change of more than half a refresh forces a read)
    bool probeForceNext;       // a policy or free-run change forces the next read
    uint64_t probeReads, probeSkips;
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
    double lastPred60, lastPredNow, lastPredFree;
    int lastPredCore;
    bool summaryDone;
    uint64_t movieRuns;
} g;
// wave 11 (MSC_HELD_CADENCE=auto): refreshes a held picture is shown for (2, the 30 cap; 3 while np_fps_cad3.h holds its steady cadence)
static inline int HeldPeriod() { return g.cad3 && g.cad3Period == 3 ? 3 : 2; }

// ---- helper f45 (18 Nov 2026): MSC_FPS=45 (alias MSC_CADENCE45=1), np_fps45.h. The state; the code is with NpFpsBeginFrame (F45Frame) and the display's calls (NpFps45Due / NpFps45Shown).
// Live single-player play is made at 45 (swap mode 0, the back-pressure gate at a fixed depth, the display holding each picture to its slot of the 1, 1, 2 pattern, the game time of each frame
// = the vblank its picture goes up on); everything else (held scenes, Mega Strikes, pauses, menus, loads, LAN / online / recorded games, the fallback) is the locked 30 of MSC_FPS=30 exactly.
struct F45State {
    npfps::Clock45 clock;          // game thread
    npfps::Fallback45 fall;        // game thread
    npfps::Lead45 lead;            // game thread: refreshes before its slot a frame starts (learned 3..5; MSC_F45_LEAD=3|4|5 fixes it)
    bool live = false;             // this frame is made at 45 (decided at its start, used by its swap and its gate)
    // Fable (MSC_LOCK60_TIERS=1 with MSC_LOCK60=1, np_fps_tier.h): Smooth 60's relief in live play is the 45 above (and only its fallback the 30); at 60 the frame's game time is the
    // vblank its picture is shown on too (clock60: the same clock on the every-vblank pattern; MSC_LOCK60_SHOWCLOCK=0 leaves 60's time as it was), its pacing is 60's own
    bool tiers = false, clock60On = true, live60 = false;
    uint32_t retryPics = 0;
    // wave 24 f50 (MSC_LOCK60_USE50, default 0): the mid tier is the strict 50 (5 pictures in 6 refreshes, 1-1-1-1-2) instead of the 45 (np_fps45.h Pattern45(5, 6)); implies the mid tier is allowed (USE45 is not needed),
    // MSC_FPS=45 stays the 45. pat is the pattern of the Gate45 / Clock45 / budget below; midName the tier's name in the logs.
    bool use50 = false;
    npfps::Pattern45 pat;
    const char* midName = "45";
    uint32_t f50Slips0 = 0, f50H0[4] = {0, 0, 0, 0}, f50Pics0 = 0;
    bool use45 = false;           // wave 24 p98 (MSC_LOCK60_USE45, default 0 = the owner's decision of 24 Nov: live play is 60 <-> 30 only; 1 = the 60 / 45 / 30 tiers as before; the 45 code stays compiled; MSC_FPS=45 is not affected)
    bool p98 = false, p98Trial = false;   // wave 24 p98 (MSC_LOCK60_P98, MSC_LOCK60_TRIAL): 60 held on the delivered picture, 45 / 30 returned by trial (np_fps_ctl.h Config::p98, np_fps45.h trialAfter)
    // p98 transitions (tier or picture size): the pictures around each one, and the transition hitches (a repeat or a picture over its slot within 3 pictures after it)
    double p98Prev[3] = {0, 0, 0}, p98Before[3] = {0, 0, 0}, p98After[3] = {0, 0, 0};
    int p98Pend = 0, p98Kind = 0; uint32_t p98Trans = 0, p98TransHitch = 0, p98TransSize = 0, p98TransTier = 0; bool p98HitchedThis = false, p98GraceWas = false; double p98ThrMs = 0; double p98At = 0;
    uint32_t p98Pics = 0;
    // p98 clusters: repeats per scene class (0 play, 1 cut-in (first 6 s after a scene change), 2 goal / kick-off (other scene change within play)) and visible-hitch clusters
    uint32_t p98Rep[3] = {0, 0, 0}, p98Hitch[3] = {0, 0, 0}, p98Pict[3] = {0, 0, 0}; double p98SceneAt = -1e9; unsigned p98Scene = 0xFFFFFFFFu; double p98RepT[4] = {-1e9, -1e9, -1e9, -1e9};
    bool retry = false;            // trap24 MSC_F45_RETRY: the fallback 30's proof is judged on the mean cost once the hold has been over for MSC_F45_RETRY_SECS
    bool lateOnly = false;         // pace24 MSC_F45_LATEONLY: Fallback45 hears true late pictures and the delivered rate, not planned holds
    uint32_t seenLateFb = 0, seenHoldSum = 0;
    double stallInSteps = 0.0; uint32_t stallN = 0;   // pace24: wall ms the game thread lost inside the steps beyond their CPU ms (blocked or preempted), summed since the last [hold60] line
    double spreadCapMs = 0.0;      // pace24 MSC_LOCK60_SPREAD_CAP: debt (ms) up to which the 60's catch-up stays one step a picture (0: one step's worth, 20)
    bool hold = false;             // hold60 MSC_LOCK60_HOLD=1 (np_fps_ctl.h Config::Lock60Hold, Fallback45Config::Hold): 60 judged on the delivered rate, the climb on measured work
    uint32_t wNoFitGame = 0, wNoFitGpu = 0, wNoFitLane = 0;
    double feMean = 0.0, beMean = 0.0;   // hold60: the lanes' running means (newest value a picture, 1/16 weight)
    bool spread = false;           // hold60b MSC_LOCK60_SPREAD: the 60 tier's catch-up one step a picture (np_game_clock.h Spread60Cap)
    uint32_t spreadPics = 0, spreadFrom = 0;   // pictures at 60 held to one step while owing time / of them the first after a late picture (logging, since the last [hold60] line)
    uint32_t feSeen = 0, beSeen = 0;   // the fallback 30's proof: pictures that did not fit, by the first input over (logging)
    npfps::Clock45 clock60;        // game thread
    npfps::TierStats ts;
    // pace13 MSC_TIER_MEMORY (default off; np_fps_tiermem.h): the tier a stadium settled at, per picture mode, in <data dir>/np-tier-memory.txt (never in the saves)
    bool memOn = false, memLoaded = false, memIn = false, memSeeded = false;
    int memStadium = -1;
    double memHold = 60.0, memStart[3] = {0, 0, 0};
    unsigned memSeeds = 0, memSaves = 0;
    npfps::TierMem mem;
    uint32_t seen60[4] = {};       // the display's 60 counters at the last [tier] line
    uint64_t tierLines = 0;
    uint32_t stamp = 0;            // the vblank this frame began at
    int depth = 2;                 // MSC_F45_DEPTH (1..3): pictures unshown before a frame may start while the gate has no slot to aim at (the first frames of a stretch); with a slot the
                                   // start is `lead` refreshes before it and the count is only a cap (depth + 1: at lead 5 the picture three back is reported on that very vblank)
    double lq = 0.8;               // MSC_F45_LQ: the quantile of start-to-ready refreshes the clock plans with
    double lq60 = -1.0;            // hold60 (MSC_F60_LQ; 0.95 with MSC_LOCK60_HOLD=1, else lq): the 60 tier's plan quantile (host test hold60_unit.cpp 9: 1 repeat in 10 puts 2 pictures off their show vblank per repeat at 0.8, none at 0.95)
    double fitMs = 20.0;           // MSC_F45_FIT_MS: the cost a picture may have at 45 for the fallback's proof (90 % of 22.24 ms)
    uint32_t seenShown = 0, seenSlips = 0, seenLate = 0, seenJudged = 0, seenRoomy = 0;
    uint32_t wGateSlot = 0, wGateCount = 0, wGateCap = 0;   // the [f45] window: frames the gate started on their slot's lead / on the count of pictures only; waits cut at 8 refreshes
    double stepMs = 6.5;           // the step cost (moving mean), for the cost at 45 of a picture measured at 30
    unsigned matchOut = 0;
    // the [f45] window (frames of mode 45, by what they were made at) and the display totals at its start
    uint32_t win = 0, w45 = 0, wHeld = 0, wFall = 0, wOther = 0, wPred = 0, wOwn = 0, wFit = 0, wNoFit = 0, wBpWait = 0, wBpRefresh = 0;
    uint32_t d0[16] = {};
    uint64_t lines = 0;
#if !defined(__vita__)
    uint32_t hostSlot = 0;         // host builds (no display thread): the game thread waits for its slot itself (MSC_F45 host pacing), so the 45 path runs in host proofs
    bool hostHave = false;
#endif
} g45;
std::atomic<int> g45Live{0};      // (read by the renderer: NpFps45Live, the mid tier's budget of the picture ladder: 1 = the 45, 2 = the 50)
void F45SetPattern(const npfps::Pattern45& pat, bool seamless);   // (f50: the display side's gate takes the mid tier's pattern; defined with it)
// display thread -> game thread (and back): per-frame records by tag, the newest report, the learned latency, the counters
std::atomic<uint64_t> g45Target[64];   // (tag30 << 32) | G (the slot the frame's game time stands for)
std::atomic<uint64_t> g45Stamp[64];    // (tag29 << 32) | (has a target << 31) | (a 60-tier picture << 30) | (frame start vblank & 0x3fffffff)
std::atomic<uint64_t> g45Fb{0};        // ((tag29 << 3 | phase) << 32) | shown vblank; 0 = none (phase 4: a 60-tier picture)
std::atomic<int> g45L{-1};
std::atomic<int> g60L{-1};             // (the 60 tier's start-to-ready quantile)
enum { kD60Shown, kD60Late, kD60ErrN, kD60ErrOff, kD60H2, kD60Count };
std::atomic<uint32_t> g60d[kD60Count];
enum { kD45Shown, kD45Slips, kD45Late, kD45Planned, kD45Off, kD45Resyncs, kD45Stalls, kD45H1, kD45H2, kD45H3, kD45ErrN, kD45ErrOff, kD45ErrAbs, kD45Judged, kD45Roomy, kD45HoldSum, kD45Count };
std::atomic<uint32_t> g45d[kD45Count];
std::atomic<uint32_t> g45ErrWorst{0};
static inline uint32_t F45Tag(uint64_t tag) { return (uint32_t)(tag & 0x1FFFFFFFull); }   // (29 bits: the report packs the pattern's phase, 0..4, beside it)
void F45Announce()
{
    if (const char* d = getenv("MSC_F45_DEPTH")) { const int v = atoi(d); if (v >= 1 && v <= 3) g45.depth = v; }
    if (const char* q = getenv("MSC_F45_LQ")) { const double v = atof(q); if (v >= 0.5 && v <= 1.0) g45.lq = v; }
    if (const char* f = getenv("MSC_F45_FIT_MS")) { const double v = atof(f); if (v >= 10.0 && v <= 22.3) g45.fitMs = v; }
    if (const char* l = getenv("MSC_F45_LEAD")) { const int v = atoi(l); if (v >= 3 && v <= 5) g45.lead.Fix(v); }   // (auto, the default: learned)
    for (int i = 0; i < kD45Count; ++i) g45d[i].store(0, std::memory_order_relaxed);
    fprintf(stderr, "[f45] 45 cadence on (MSC_CADENCE45=1, or SMOOTH 60 in tiers): live single-player play at %s pictures a second on the fixed pattern %s refreshes (the display holds each picture to its slot; a late picture "
            "takes a later slot, the phase is kept, a stall over 8 refreshes starts the pattern again); each frame's game time is the vblank its picture goes up on; a frame starts (and reads the pad) "
            "%d refreshes before its picture's slot (%.0f ms start to picture; MSC_F45_LEAD=%s: up at once when pictures come ready after their slot, down after 8 s of proven room, 3 to 5), "
            "at most %d pictures unshown; held scenes, Mega Strikes, menus, loads, LAN / online / recorded games at the locked 30; "
            "more than 8 %% of 90 pictures off their slot, or more than 4 %% of 360 -> the locked 30 at full size, back only after 25 s (doubled per fallback in 180 s; the third of a match "
            "latches it) and 12 s of pictures whose cost at 45 fits %.1f ms; a [f45] line every 300 pictures\n", g45.use50 ? "50" : "45", g45.use50 ? "1, 1, 1, 1, 2" : "1, 1, 2", g45.lead.Lead(), g45.lead.Lead() * 1001.0 / 60.0,
            getenv("MSC_F45_LEAD") && atoi(getenv("MSC_F45_LEAD")) >= 3 && atoi(getenv("MSC_F45_LEAD")) <= 5 ? "fixed" : "auto", g45.depth + 1, g45.fitMs);
}

// wave 9 (MSC_SLIP_BUDGET_MS): defined with the slip code at the end of the file
void SlipInit();
void SlipFrame(float realSeconds, unsigned taskState);
double g_smBasis = -1.0;     // (np_smooth.cpp) this frame's game seconds from the show clock, -1 = its own time
float g_slipAdvAbs = 0.0f;  // the frame's decided advance (seconds), when the show clock gave the time
void SlipNoteStep();
void SpeedFrame(unsigned taskState);

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
bool LateAnchorBeforeGate();   // MSC_LAT_ANCHOR (below, with the late wait)
void LateForgetAnchor();       // the wait-to-wait cadence of policy 30 is broken (MSC_FPS_BETWEEN)

double EnvDouble(const char* name, double def)
{
    const char* e = getenv(name);
    return (e && *e) ? atof(e) : def;
}

// MSC_FPS_HYST=1|2 and the single thresholds (all optional): see np_fps_ctl.h. Fractions are of one refresh (0.8 = 80 %), times are seconds.
npfps::Config MakeAutoConfig()
{
    const char* h = getenv("MSC_FPS_HYST");
    g.hyst = (h && *h && atoi(h) == 1) ? 1 : 2;
    npfps::Config c = g.lock60 ? (g45.tiers ? (g45.hold ? npfps::Config::Lock60Hold() : npfps::Config::Lock60Tiered()) : npfps::Config::Lock60()) : (g.hyst == 1 ? npfps::Config::V11() : npfps::Config());
    if (c.lock60) { if (const char* st = getenv("MSC_LOCK60_STICKY")) c.sticky = atoi(st) != 0; c.stickyMinHold = EnvDouble("MSC_LOCK60_STICKY_HOLD", c.stickyMinHold); c.stickyProof = EnvDouble("MSC_LOCK60_STICKY_PROOF", c.stickyProof); fprintf(stderr, "[fps] MSC_LOCK60_STICKY=%d (after a relief to the full-size 30 a gameplay scene stays there at least %.0f s, doubled per relief in %.0f s, latched %.0f s after %d; 60 returns only after %.0f s of headroom under %.0f %% of a refresh)\n", c.sticky ? 1 : 0, c.stickyMinHold, c.stickyWindow, c.stickyLatchSecs, c.stickyLatchAfter, c.stickyProof, c.stickyMargin * 100.0); }
    if (c.hold60)
    {   // held30 (23 Nov; owner: everything outside live play at a steady 30): a held scene starts at 30 under the hold; MSC_LOCK60_HELD30=0 = as before (starts at 60)
        const char* h3 = getenv("MSC_LOCK60_HELD30");
        c.held30 = !(h3 && *h3 && atoi(h3) == 0);
        // pace24 (24 Nov), both default OFF (pacing policy, console A/B): the 2 s fast window and the cut-in grace; the climb's sim priced in wall ms
        c.holdFastGrace = EnvDouble("MSC_LOCK60_FASTGRACE", 0.0) != 0.0;
        c.holdPanicRate = EnvDouble("MSC_LOCK60_PANIC_RATE", c.holdPanicRate);
        c.holdWallSim = EnvDouble("MSC_HOLD60_WALLSIM", 0.0) != 0.0;   // nxr4c (25 Nov): stays OFF: the clock under-read is real (wallsim25_unit) but it moves the climb cost only on pictures with 2+ steps (the 30 tier), not at 1 step; console A/B wallsim25
        {   // wave 24 p98 (default OFF; the owner's rule: a 2 s spike must not take 60 away for the rest of the game). MSC_LOCK60_P98=1; knobs MSC_P98_REPEAT_PCT (1.5), MSC_P98_WINDOW (10 s), MSC_P98_SUSTAIN (3 s), MSC_P98_HITCH_SECS (5)
            c.p98 = EnvDouble("MSC_LOCK60_P98", 0.0) != 0.0;
            c.p98RepeatPct = EnvDouble("MSC_P98_REPEAT_PCT", c.p98RepeatPct); if (c.p98RepeatPct < 0.1 || c.p98RepeatPct > 20.0) c.p98RepeatPct = 1.5;
            c.p98Window = EnvDouble("MSC_P98_WINDOW", c.p98Window); if (c.p98Window < 3.0 || c.p98Window > 15.0) c.p98Window = 10.0;
            c.p98Sustain = EnvDouble("MSC_P98_SUSTAIN", c.p98Sustain); if (c.p98Sustain < 0.5 || c.p98Sustain > 20.0) c.p98Sustain = 3.0;
            c.p98HitchSecs = EnvDouble("MSC_P98_HITCH_SECS", c.p98HitchSecs); if (c.p98HitchSecs < 1.0) c.p98HitchSecs = 5.0;
            c.p98Trials = c.p98 && EnvDouble("MSC_LOCK60_TRIAL", 0.0) != 0.0;
            c.p98TrialAfter = EnvDouble("MSC_LOCK60_TRIAL_AFTER", c.p98TrialAfter); if (c.p98TrialAfter < 1.0) c.p98TrialAfter = 4.0;
            c.p98TrialSecs = EnvDouble("MSC_LOCK60_TRIAL_SECS", c.p98TrialSecs); if (c.p98TrialSecs < 1.0) c.p98TrialSecs = 2.0;
            c.p98BackoffMax = EnvDouble("MSC_P98_BACKOFF_MAX", c.p98BackoffMax); if (c.p98BackoffMax < c.p98TrialAfter) c.p98BackoffMax = 20.0;
            g45.p98 = c.p98; g45.p98Trial = c.p98Trials;
            // wave 25 flip25: the trial of 60 by time and the 60 <-> 50 flips (np_fps_ctl.h Config::p98Defer / p98Calm)
            c.p98Defer = EnvDouble("MSC_LOCK60_TRIAL_DEFER", 1.0) != 0.0;
            c.p98Calm = c.p98Trials && EnvDouble("MSC_LOCK60_TRIAL_CALM", 1.0) != 0.0;   // (nxpace25, owner policy 25 Nov 16:04: pacing only, model-proven and det-equal: default ON, =0 restores)
            c.p98CalmFails = (int)EnvDouble("MSC_LOCK60_TRIAL_CALM_FAILS", (double)c.p98CalmFails); if (c.p98CalmFails < 1) c.p98CalmFails = 2;
            c.p98CalmFit = EnvDouble("MSC_LOCK60_TRIAL_CALM_FIT", c.p98CalmFit); if (c.p98CalmFit < 0.3 || c.p98CalmFit > 1.5) c.p98CalmFit = 0.90;
            c.p98CalmSettle = EnvDouble("MSC_LOCK60_TRIAL_CALM_SETTLE", c.p98CalmSettle); if (c.p98CalmSettle < 0.0 || c.p98CalmSettle > 60.0) c.p98CalmSettle = 6.0;
            if (c.p98Trials)
                fprintf(stderr, "[p98] flip25: MSC_LOCK60_TRIAL_DEFER=%d (the trial of 60 by time honours the 30 -> 50 -> 60 order: no 60 trial while the 50 is on its relief); MSC_LOCK60_TRIAL_CALM=%d: %s\n", c.p98Defer ? 1 : 0, c.p98Calm ? 1 : 0,
                        c.p98Calm ? "a 60 trial by time waits 6 s after a scene change, is skipped while the predicted busiest-core cost at 60 is over 90 % of a refresh, and stops after 2 failed trials in a row in a scene (the cut back into play still tries)" : "off");
            if (c.p98)
                fprintf(stderr, "[p98] MSC_LOCK60_P98=1: 60 is kept while over the last %.0f s of live play the repeated refreshes are at most %.1f %% and visible hitches (2 repeats in a row, or 3 within 0.5 s) at most one per %.0f s; "
                        "it falls only when that was violated for %.1f s while repeats still happen (not for one burst); the slow-window and busy-core rules are not used; the collapse guard (under %.0f a second over %.0f s outside the cut-in grace) stays; "
                        "trials MSC_LOCK60_TRIAL=%d: %s\n", c.p98Window, c.p98RepeatPct, c.p98HitchSecs, c.p98Sustain, c.holdFastRate, c.holdFastSecs, c.p98Trials ? 1 : 0,
                        c.p98Trials ? "the relief tries 60 for 2 s after 4 s (backoff x2, at most 20 s) and at every cut back into play, never when the GPU is over its budget" : "off (the old climb proof decides)");
        }
        if (c.holdFastGrace || c.holdWallSim)
            fprintf(stderr, "[fps] pace24: MSC_LOCK60_FASTGRACE=%d (inside the cut-in's %.0f s grace the 2 s window falls 60 only under %.0f a second, not under %.0f), MSC_HOLD60_WALLSIM=%d (the climb prices the steps in wall ms, as the frame)\n",
                    c.holdFastGrace ? 1 : 0, c.holdGrace, c.holdPanicRate, c.holdFastRate, c.holdWallSim ? 1 : 0);
        fprintf(stderr, "[fps] MSC_LOCK60_HELD30=%d: under SMOOTH 60 a scene outside live play (walk-out, replay, celebration, cut-scene, menu) %s\n", c.held30 ? 1 : 0, c.held30 ? "starts at 30 and stays there until it ends" : "starts at 60, as before");
    }
    if (c.lock60 && g45.tiers)
    {   // pace13: Galaxy's governor rules against flapping, host model native/tools/fps/tier_flap_unit.cpp. Both default OFF (a behaviour change under Smooth 60, console A/B first).
        c.tierDwell = EnvDouble("MSC_TIER_DWELL", 0.0);
        const char* rl = getenv("MSC_TIER_RELAPSE");
        c.tierRelapse = rl && *rl && atoi(rl) != 0;
        if (c.tierDwell > 0.0 || c.tierRelapse)
            fprintf(stderr, "[tier] MSC_TIER_DWELL=%.1f MSC_TIER_RELAPSE=%d: %s%s%s\n", c.tierDwell, c.tierRelapse ? 1 : 0, c.tierDwell > 0.0 ? "a fall from 60 by the miss / slow-mean rules waits for the dwell after a rise (a core or the GPU over budget still falls at once)" : "",
                    c.tierDwell > 0.0 && c.tierRelapse ? "; " : "", c.tierRelapse ? "a fall within 20 s of a rise counts twice in the sticky history (60: 25 -> 50 s; 30 -> 45 likewise)" : "");
    }
    c.upMargin = EnvDouble("MSC_FPS_UP_MARGIN", c.upMargin);
    c.upHold = EnvDouble("MSC_FPS_UP_HOLD", c.upHold);
    c.upFrames = (int)EnvDouble("MSC_FPS_UP_FRAMES", c.upFrames);
    c.upOutliers = EnvDouble("MSC_FPS_UP_OUTLIERS", c.upOutliers);
    c.downBusy = EnvDouble("MSC_FPS_DOWN_BUSY", c.downBusy);
    c.busyFrames = (int)EnvDouble("MSC_FPS_DOWN_FRAMES", c.busyFrames);
    c.coolBase = EnvDouble("MSC_FPS_COOL_BASE", c.coolBase);
    c.coolMax = EnvDouble("MSC_FPS_COOL_MAX", c.coolMax);
    c.failWindow = EnvDouble("MSC_FPS_FAIL_WINDOW", c.failWindow);
    if (const char* l = getenv("MSC_FPS_LADDERHOLD")) c.ladderHold = atoi(l) != 0;   // 1.2: a GPU-limited miss at 60 waits for the dynamic resolution's ladder (np_fps_ctl.h)
    c.ladderHoldMax = EnvDouble("MSC_FPS_LADDERHOLD_MAX", c.ladderHoldMax);
    if (const char* b = getenv("MSC_FPS_BACKOFF")) { if (IEqual(b, "scene")) c.backoffByScene = true; else if (IEqual(b, "downs")) c.backoffByScene = false; }
    if (c.upMargin < 0.3) c.upMargin = 0.3;
    if (c.downBusy < c.upMargin) c.downBusy = c.upMargin;
    if (c.busyFrames < 1) c.busyFrames = 1;
    if (c.upFrames < 1) c.upFrames = 1;
    if (c.coolBase < 0.5) c.coolBase = 0.5;
    if (c.coolMax < c.coolBase) c.coolMax = c.coolBase;
    fprintf(stderr, "[fps] auto thresholds (MSC_FPS_HYST=%d): up when every core and the GPU fit within %.0f%% of a refresh for %.1f s (%d frames, %.0f%% outliers); down after %d frames over %.0f%%; "
            "cool-down %.0f s doubled per failed try %s (at most %.0f s)\n", g.hyst, c.upMargin * 100.0, c.upHold, c.upFrames, c.upOutliers * 100.0, c.busyFrames, c.downBusy * 100.0, c.coolBase,
            c.backoffByScene ? "of the scene" : "(downs in the last minute)", c.coolMax);
    return c;
}

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
        else if (IEqual(e, "45"))
        {
            // owner, 18 Nov: no separate 45 FPS choice (Smooth 60 uses the 45 cadence as its middle tier). A settings file of the s29-f45 test builds that still says 45 is the 30.
            g.mode = kMode30;
            fprintf(stderr, "[fps] MSC_FPS=45 is read as 30 (the 45 FPS choice is gone: SMOOTH 60 MODE falls to the 45 cadence by itself; MSC_CADENCE45=1 is the developer switch for 45 alone)\n");
        }
        else fprintf(stderr, "[fps] MSC_FPS=%s not understood (30, 45, auto, 60, unlocked, game): using the default\n", e);
    }
    {   // helper f45: MSC_CADENCE45=1 is MSC_FPS=45 when MSC_FPS names no rate of its own (unset or auto)
        const char* c45 = getenv("MSC_CADENCE45");
        if (c45 && *c45 && atoi(c45) != 0 && (!e || !*e || IEqual(e, "auto"))) g.mode = kMode45;
    }
    {   // 1.3 MSC_LOCK60=1: the LOCKED 60 picture mode. AUTO's controller with the 60-first rules (np_fps_ctl.h Config::Lock60), a replay / Mega Strike that is not held at 30 by name
        // (MSC_FPS_REPLAY and MSC_FPS_MEGA are ignored: the renderer's presets and the ladder's floor decide, the relief is the controller's), no "between" pacing. A host build keeps its
        // deterministic 30 unless MSC_FPS is named. MSC_FPS=30 / 60 / unlocked / game named by hand wins over the mode.
        const char* lk = getenv("MSC_LOCK60");
        g.lock60 = lk && *lk && atoi(lk) != 0 && (!e || !*e || IEqual(e, "auto")) && g.mode != kMode45;
        if (g.lock60) g.mode = kModeAuto;
        {   // Fable: MSC_LOCK60_TIERS (code default 1 with LOCKED 60, owner 18 Nov: Smooth 60 defends 60 and degrades gently; =0 is Smooth 60 as it was): three tiers (np_fps_tier.h)
            const char* t = getenv("MSC_LOCK60_TIERS");
            g45.tiers = g.lock60 && !(t && *t && atoi(t) == 0);
            {   // pace13: MSC_TIER_MEMORY (default OFF: a behaviour change, console run first), MSC_TIER_MEMORY_HOLD seconds (default 60) before the way up is tried
                const char* tm = getenv("MSC_TIER_MEMORY");
                g45.memOn = g45.tiers && tm && *tm && atoi(tm) != 0;
                g45.memHold = EnvDouble("MSC_TIER_MEMORY_HOLD", 60.0);
            }
            {   // p98 / owner 24 Nov: "Concentrate on 60 and 30 only. Remove 45 from it for now." MSC_LOCK60_USE45=1 brings the 45 tier back (the code is kept)
                const char* u45 = getenv("MSC_LOCK60_USE45");
                g45.use45 = g45.tiers && u45 && *u45 && atoi(u45) != 0;
                // f50 (24 Nov, owner: "could we keep 50fps+ ... as the floor before dropping to 30"): MSC_LOCK60_USE50=1 makes the mid tier a strict 50 (5 pictures in 6 refreshes); default 0
                const char* u50 = getenv("MSC_LOCK60_USE50");
                g45.use50 = g45.tiers && g.mode != kMode45 && u50 && *u50 && atoi(u50) != 0;
                g45.pat = npfps::MidPattern(g.mode == kMode45, g45.use50);
                if (g45.use50) { g45.midName = "50"; g45.fitMs = 0.9 * g45.pat.PeriodRefreshes() * 1001.0 / 60.0; }
                g45.clock = npfps::Clock45(g45.pat);
                F45SetPattern(g45.pat, g45.use50);
            }
            const char* sc = getenv("MSC_LOCK60_SHOWCLOCK");
            g45.clock60On = !(sc && *sc && atoi(sc) == 0);
            // hold60 (23 Nov, owner: "getting to 60 smooth should be the goal"): MSC_LOCK60_HOLD=1 keeps 60 down to a delivered 51 pictures a second, climbs back on measured work, 45 -> 30
            // only when 45 truly cannot hold and back sooner (np_fps_ctl.h Config::Lock60Hold, np_fps45.h Fallback45Config::Hold). Code default 0 (=0: the tiers as before), console A/B first.
            const char* hd = getenv("MSC_LOCK60_HOLD");
            g45.hold = g45.tiers && hd && *hd && atoi(hd) != 0;
            // hold60: the 60 tier's show-time clock plans each picture for the vblank that 95 % of the recent pictures could make (0.8 before): a repeated refresh then costs the one picture
            // that overran, not it and the catch-up after it (about half a refresh more latency on average when pictures vary, none when they do not)
            g45.lq60 = g45.hold ? 0.95 : -1.0;
            // hold60b: at the 60 tier a late picture's catch-up is spread over the next pictures, one step each (np_game_clock.h Spread60Cap); default on with the hold, MSC_LOCK60_SPREAD=0 off
            const char* sp = getenv("MSC_LOCK60_SPREAD");
            g45.spread = g45.hold && !(sp && *sp && atoi(sp) == 0);
            // pace24 MSC_LOCK60_SPREAD_CAP (ms, 20..60; default 0 = one step's worth, 20): the debt up to which the 60's catch-up stays one step a picture (np_game_clock.h Spread60Cap)
            g45.spreadCapMs = g45.spread ? EnvDouble("MSC_LOCK60_SPREAD_CAP", 0.0) : 0.0;
            if (g45.spreadCapMs != 0.0 && (g45.spreadCapMs < 20.0 || g45.spreadCapMs > 60.0)) g45.spreadCapMs = 0.0;
            if (g45.spreadCapMs > 0.0) fprintf(stderr, "[hold60] MSC_LOCK60_SPREAD_CAP=%.0f: the 60's catch-up stays one step a picture while up to %.0f ms are owed (20 before)\n", g45.spreadCapMs, g45.spreadCapMs);
            if (const char* q6 = getenv("MSC_F60_LQ")) { const double v = atof(q6); if (v >= 0.5 && v <= 1.0) g45.lq60 = v; }
        }
    }
    const char* l = getenv("MSC_FPS_LOG");
    g.logLevel = (l && *l) ? atoi(l) : 0;
    const char* cs = getenv("MSC_FPS_COST_SCALE");
    g.costScale = (cs && *cs) ? atof(cs) : 1.0;
    if (!(g.costScale > 0.0)) g.costScale = 1.0;
    {   // wave 18 (probe18): MSC_FPS_PROBE_EVERY=N (1..8, default 1 = today): the per-core probe (one sceKernelGetThreadInfo per thread) is read once in N pictures in auto mode; see the skip rule in NpFpsBeginFrame
        const char* pe = getenv("MSC_FPS_PROBE_EVERY");
        g.probeEvery = (pe && *pe) ? atoi(pe) : 1;
        if (g.probeEvery < 1) g.probeEvery = 1;
        if (g.probeEvery > 8) g.probeEvery = 8;
    }
    ParseModel(getenv("MSC_FPS_MODEL"));
    {
        const char* r = getenv("MSC_FPS_REPLAY");
        g.replayMode = g.lock60 ? 2 : 0;   // (LOCKED 60: a replay is not held at 30 by name: auto, with the controller's relief)
        if (r && *r && !g.lock60)
        {
            if (IEqual(r, "30")) g.replayMode = 1;
            else if (IEqual(r, "auto")) g.replayMode = 2;
            else if (!IEqual(r, "fit")) fprintf(stderr, "[fps] MSC_FPS_REPLAY=%s not understood (fit, 30 or auto): fit\n", r);
        }
    }
    {
        const char* sp = getenv("MSC_FPS_SCENE_PAUSE");
        g.sceneNoPause = (sp && *sp == '0') ? 0 : 1;
    }
    {
        const char* b = getenv("MSC_FPS_BETWEEN");
        if (b && *b && !g.lock60)
        {
            if (IEqual(b, "pace"))
            {
                npfps::BetweenConfig c;
                c.enterMs = EnvDouble("MSC_FPS_BETWEEN_ENTER", c.enterMs);
                c.leaveMs = EnvDouble("MSC_FPS_BETWEEN_LEAVE", c.leaveMs);
                if (c.leaveMs < c.enterMs) c.leaveMs = c.enterMs;
                // wave 9 (sticky switch): the cool-down after a way out, how long a try must last not to count as failed (the cool-down doubles for a failed one), and the frames over the limit that end a try
                c.cool = EnvDouble("MSC_FPS_BETWEEN_COOL", c.cool);
                c.coolMax = EnvDouble("MSC_FPS_BETWEEN_COOLMAX", c.coolMax);
                c.fail = EnvDouble("MSC_FPS_BETWEEN_FAIL", c.fail);
                c.outFrames = (int)EnvDouble("MSC_FPS_BETWEEN_OUT", (double)c.outFrames);
                if (c.cool < 0) c.cool = 0;
                if (c.coolMax < c.cool) c.coolMax = c.cool;
                if (c.outFrames < 1) c.outFrames = 1;
                g.betweenMode = 1;
                g.between = new npfps::Between(c);
                fprintf(stderr, "[fps] MSC_FPS_BETWEEN=pace: AUTO's 30 runs free in a match when the busiest stage stays under %.1f ms a frame, back to 30 over %.1f ms for %d frames (cool-down %.0f s, doubling for a try under %.0f s, at most %.0f s)\n", c.enterMs, c.leaveMs, c.outFrames, c.cool, c.fail, c.coolMax);
            }
            else if (!IEqual(b, "30")) fprintf(stderr, "[fps] MSC_FPS_BETWEEN=%s not understood (30 or pace): 30\n", b);
        }
        const char* ps = getenv("MSC_PRESENT_SYNC");
        g.presentSync = (ps && *ps) ? atoi(ps) : -1;
        if (g.presentSync > 2) g.presentSync = 2;
        if (ps && *ps && g.presentSync < 0) g.presentSync = 0;
    }
    if (!g.threadClock) g.threadClock = (&NpFpsPlatformThreadClockMs != 0) ? NpFpsPlatformThreadClockMs : DefaultThreadClockMs;
    if (!g.probe && &NpFpsPlatformProbe != 0) g.probe = NpFpsPlatformProbe;
    switch (g.mode)
    {
    case kModeGame: g.policy = kPolicyGame; break;
    case kMode30: g.policy = kPolicy30; break;
    case kMode60: g.policy = kPolicy60; break;
    case kModeUnlocked: g.policy = kPolicyUnlocked; break;
    default: g.policy = g.lock60 ? kPolicy60 : kPolicy30; break;   // auto starts at 30; LOCKED 60 at 60 (45: the policy is 30, and live play is made at 45 on top of it: F45Frame)
    }
    if (g.mode == kModeAuto)
    {
        g.ctl = new npfps::Auto(MakeAutoConfig());
        const char* cd = getenv("MSC_CADENCE");
        if (cd && *cd && atoi(cd) != 0 && !g.lock60)
        {
            npfps::CadenceConfig cc;
            cc.fitMargin = EnvDouble("MSC_CADENCE_FIT", cc.fitMargin);
            cc.visitsToFree = (int)EnvDouble("MSC_CADENCE_VISITS", cc.visitsToFree);
            cc.visitsMax = (int)EnvDouble("MSC_CADENCE_VISITS_MAX", cc.visitsMax);
            if (cc.visitsToFree < 1) cc.visitsToFree = 1;
            if (cc.visitsMax < cc.visitsToFree) cc.visitsMax = cc.visitsToFree;
            g.cad = new npfps::Cadence(cc);
            { const char* cm = getenv("MSC_CADENCE_MENUS"); g.cadMenus = cm && *cm && atoi(cm) != 0; }
            const char* cf = getenv("MSC_CADENCE_FULL");
            g.cadFull = cf && *cf && atoi(cf) != 0 && &VgxCadenceFull != 0;
            if (g.cadFull) fprintf(stderr, "[fps] MSC_CADENCE_FULL=1: a scene held at 30 is drawn at the game's own size from its cut (the 60 ladder's size comes back at the cut that ends the hold)\n");
            fprintf(stderr, "[fps] MSC_CADENCE=1: a replay and a cut-scene start held at a locked 30 (their rate is decided at the cut, never inside the shot; %d fit visits in a row release a class, "
                    "a failed release asks for twice as many up to %d); the policy before them is put back at the cut to open play; a goal makes the controller wait for its cut%s%s\n",
                    cc.visitsToFree, cc.visitsMax, g.replayMode == 0 ? "" : " (MSC_FPS_REPLAY is not 'fit': the policy only waits for the cut after a goal)",
                    g.cadMenus ? "; MSC_CADENCE_MENUS: the front-end menus, the stadium viewer and the pause menu are held classes too (never loading, never open play)" : "");
        }
    }
    if (g.mode == kModeAuto)
    {
        // wave 11 (cadence16): MSC_HELD_CADENCE=auto, default off (a deliberate drop to 20 fps where 30 is missed: the owner's call). Needs the late wait (MSC_LATE_WAIT>=1) that makes the period.
        const char* hc = getenv("MSC_HELD_CADENCE");
        if (hc && IEqual(hc, "auto") && !g.lock60)
        {
            g.cad3 = new npfps::Cad3();
            fprintf(stderr, "[fps] MSC_HELD_CADENCE=auto: a held scene (cut-scene, replay) in which more than a third of the last 20 pictures missed 33 ms is shown at a steady 3 refreshes (20 fps); "
                    "2 again after 40 pictures of work under 28 ms; one entry and one exit a scene; never in play, menus, loads, LAN or online, a Mega Strike or a locked cut-scene; needs MSC_LATE_WAIT>=1\n");
        }
    }
    atexit(Summary);
    static const char* const modeName[6] = {"game (the build's own swap modes)", "30", "60", "unlocked", "auto", "45 (live play at 45: 1, 1, 2 refreshes; everything else at a locked 30)"};
    if (g.lock60) fprintf(stderr, "[fps] LOCKED 60 (MSC_LOCK60): auto with the 60-first controller: every scene starts at 60; 30 only as a relief when the renderer's ladder is at its floor and frames still miss\n");
    if (g.mode == kMode45 || g45.tiers) F45Announce();
    if (g45.use50) fprintf(stderr, "[f50] MSC_LOCK60_USE50=1: the mid tier is a STRICT 50 (5 pictures in 6 refreshes, 1-1-1-1-2: 49.95 a second, 20 ms an average slot): 60 -> 50 -> 30, each picture shows the world at the vblank it goes up on "
            "(the only artefact is one held refresh in six), a frame starts %d refreshes before its slot; the 30 only when the 50 cannot hold; MSC_LOCK60_USE45 is not used while this is on; the ladder's budget at 50 is %.1f ms, the fallback's proof fits %.1f ms\n",
            g45.lead.Lead(), g45.pat.PeriodRefreshes() * 1001.0 / 60.0, g45.fitMs);
    if (g45.tiers) fprintf(stderr, "[p98] MSC_LOCK60_USE45=%d: live play is %s (the owner's decision of 24 Nov: 60 and 30 only; =1 brings the 45 tier back, its code is kept)\n", g45.use45 ? 1 : 0, g45.use50 ? "60 / 50 / 30 (MSC_LOCK60_USE50)" : g45.use45 ? "60 / 45 / 30" : "60 <-> 30 (a steady 30 is 2 refreshes a picture)");
    if (g45.tiers)
    {
        { const char* rl = getenv("MSC_TIER_RELAPSE"); if (rl && *rl && atoi(rl) != 0) g45.fall.SetRelapseSecs(20.0); }   // pace13 (default off)
        // pace24 MSC_F45_LATEONLY (default off): the 45's fallback counts the pictures that MISSED their slot (not the ones planned for a later slot) and the delivered rate (under MSC_F45_RATE_MIN, 37, over 135 pictures)
        g45.lateOnly = g45.hold && EnvDouble("MSC_F45_LATEONLY", 0.0) != 0.0;
        if (g45.hold)
        {
            npfps::Fallback45Config fc = npfps::Fallback45Config::Hold();
            fc.fps = g45.pat.Fps();   // (f50: the trial's length in pictures follows the mid tier's rate)   // hold60: 15 % of 90 / 5 % of 360, 10 s hold doubling to 120 s, no latch, sliding 5 s proof
            if (g45.lateOnly) { fc.rateWindow = 135; fc.rateMin = EnvDouble("MSC_F45_RATE_MIN", 37.0); fprintf(stderr, "[f45] MSC_F45_LATEONLY=1: the fallback counts pictures that missed their slot, not planned holds; and the delivered rate under %.0f a second over 135 pictures\n", fc.rateMin); }
            g45.retry = EnvDouble("MSC_F45_RETRY", 0.0) != 0.0;
            if (g45.retry)
            {
                fc.relaxAfter = EnvDouble("MSC_F45_RETRY_SECS", 20.0); fc.relaxMean = EnvDouble("MSC_F45_RETRY_MEAN", 18.0);
                if (fc.relaxAfter < 1.0) fc.relaxAfter = 1.0;
                if (fc.relaxMean < 8.0) fc.relaxMean = 8.0;
                if (fc.relaxMean > 22.0) fc.relaxMean = 22.0;
                fprintf(stderr, "[f45] MSC_F45_RETRY=1: a fallback 30 whose per-picture proof does not pass (more than 10 %% of the pictures over %.1f ms) is judged %.0f s after its hold on the MEAN cost of the last 120 pictures (at most %.1f ms) "
                        "with the GPU and lanes still judged per picture; a retry that fails falls again with the doubled hold\n", g45.fitMs, fc.relaxAfter, fc.relaxMean);
            }
            if (g45.p98 && g45.p98Trial && (g45.use50 || (g45.retry && g45.use45)))   // (f50: the 30 -> 50 return is by trial with MSC_LOCK60_USE50=1 and P98 + TRIAL, no MSC_F45_RETRY needed; the 45 as before)
            {   // p98: the 30 -> 45 return by trial (with MSC_F45_RETRY=1: the retry of trap24 stays as the second way back)
                fc.trialAfter = EnvDouble("MSC_LOCK60_TRIAL_AFTER", 4.0); if (fc.trialAfter < 1.0) fc.trialAfter = 4.0;
                fc.trialSecs = EnvDouble("MSC_LOCK60_TRIAL_SECS", 2.0); if (fc.trialSecs < 1.0) fc.trialSecs = 2.0;
                fc.trialMax = EnvDouble("MSC_P98_BACKOFF_MAX", 20.0);
                fprintf(stderr, "[p98] the fallback 30 tries the 45 for %.0f s after %.0f s and at every cut back into play (kept at 98 %% of the pictures on their slot; failed: wait x2, at most %.0f s; never while the GPU or a lane is over %.1f ms)\n", fc.trialSecs, fc.trialAfter, fc.trialMax, g45.fitMs);
            }
            g45.fall = npfps::Fallback45(fc);
        }
        else g45.fall.SetLatchSecs(240.0);   // (the third fallback of a match latches the 30 for Lock60's 240 s, not for the match: 60 has to stay reachable)
        for (int i = 0; i < kD60Count; ++i) g60d[i].store(0, std::memory_order_relaxed);
        fprintf(stderr, "[tier] Smooth 60 in tiers (MSC_LOCK60_TIERS=1, the default with SMOOTH 60 MODE; =0: as before): live play is 60, defended (12 repeated refreshes in 180 pictures, a mean over 1.25 refreshes across 60, or a core over 98 %% for "
                "45 pictures send it down; a lone burst does not), then the strict 45 cadence (the [f45] lines), then the full-size 30 only when 45 cannot hold; up again in order and on proof "
                "(30 -> 45: 25 s and 12 s of pictures that fit; 45 -> 60: the sticky hold and its 12 s proof); at 60 each frame's game time is the vblank its picture is shown on (%s); "
                "held scenes, menus, LAN / online / recorded games as Smooth 60 without tiers; a [tier] line every 300 live pictures\n", g45.clock60On ? "on" : "MSC_LOCK60_SHOWCLOCK=0: off");
        if (g45.hold)
            fprintf(stderr, "[hold60] on (MSC_LOCK60_HOLD=1; =0 the tiers above): 60 is kept while it delivers 50 pictures a second over 5 s (not judged in the first 6 s after a cut into play) and 36 over 2 s "
                    "(a repeated refresh is shown on the vblank its game time stands for); a core or the GPU over budget only counts while the pictures do not come; 45 -> 60 after 8 s and the last 240 pictures with at most 6 %% whose WORK at 60 "
                    "(game thread less waits with one step, front end, back end, GPU at the floor) is over 97 %% of a refresh; the wait after a fall 10 s, doubled per failed try in 180 s, at most 120 s, "
                    "no latch; the 45 falls to 30 on 15 %% of 90 or 5 %% of 360 pictures off their slot and comes back after 10 s (doubling, at most 120) and 8 s / 120 pictures with at most 10 %% "
                    "not fitting; a [hold60] line with every [tier] line\n");
    }
    fprintf(stderr, "[fps] MSC_FPS: %s%s (MSC_FPS=30|auto|60|unlocked|game; MSC_CADENCE45=1); policy now %s%s%s\n", modeName[g.mode], (e && *e) ? "" : " (default)", kPolicyName[g.policy],
            g_model.on ? "; cost model on (virtual clock runs)" : "", g.costScale != 1.0 ? "; cost scale set" : "");
}

void NpFpsBpHeldSummary();   // (held24: at the end of the file, next to nothing else: the gate's counters live in the anonymous namespace)
void Summary()
{
    if (!g.inited || g.summaryDone) return;
    g.summaryDone = true;
    if (g.frames == 0) return;
    fprintf(stderr,
            "[fps] summary: mode=%d frames=%llu (game %llu, 30 %llu, 60 %llu, unlocked %llu, cut-scene lock %llu) frame lengths in refreshes 1/2/3/4+ = %llu/%llu/%llu/%llu "
            "mean %.2f ms; pictures stayed 0/1/2/3/4/5+ refreshes = %llu/%llu/%llu/%llu/%llu/%llu; switches=%llu; paced frames=%llu\n",
            g.mode, (unsigned long long)g.frames, (unsigned long long)g.framesPolicy[0], (unsigned long long)g.framesPolicy[1],
            (unsigned long long)g.framesPolicy[2], (unsigned long long)g.framesPolicy[3], (unsigned long long)g.framesLocked, (unsigned long long)g.hist[0],
            (unsigned long long)g.hist[1], (unsigned long long)g.hist[2], (unsigned long long)g.hist[3], g.sumInterval * 1000.0 / (double)(g.frames ? g.frames : 1),
            (unsigned long long)g.held[0], (unsigned long long)g.held[1], (unsigned long long)g.held[2], (unsigned long long)g.held[3], (unsigned long long)g.held[4],
            (unsigned long long)g.held[5], (unsigned long long)g.switches, (unsigned long long)g.framesPaced);
    NpFpsBpHeldSummary();
    if (g.cad3)
        fprintf(stderr, "[fps] MSC_HELD_CADENCE: %llu visits to held scenes judged, %llu entries to 3 refreshes, %llu starts in 3 from the cut, %llu exits to 2; %llu pictures at the steady 3\n", (unsigned long long)g.cad3->Visits(),
                (unsigned long long)g.cad3->Entries(), (unsigned long long)g.cad3->Carried(), (unsigned long long)g.cad3->Exits(), (unsigned long long)g.cad3Frames);
    if (g.bg30Discounted) fprintf(stderr, "[fps] MSC_BG30: %llu pictures judged at a background-at-30 pair's cost\n", (unsigned long long)g.bg30Discounted);
    if (g.cad)
        fprintf(stderr, "[fps] cadence: %llu cuts, %llu frames held at a locked 30 by class, %llu resumes of open play, %llu released-class starts at 60, %llu frames the controller waited for a goal's cut; "
                "held now: goal celebration %d, other cut-scene %d, replay %d\n", (unsigned long long)g.cadCuts, (unsigned long long)g.cadHoldFrames, (unsigned long long)g.cadResumes,
                (unsigned long long)g.cadGo60, (unsigned long long)g.cadDefers, g.cad->Held(npfps::kCadCut, 1) ? 1 : 0, g.cad->Held(npfps::kCadCut, 2) ? 1 : 0, g.cad->Held(npfps::kCadReplay, 0) ? 1 : 0);
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
// 1.3 MSC_LOAD_FPS=60 (default off): the same pace while a match loads or is left (task states FE_TO_GAME, GAME_TO_FE). The game's loader works in slices of a frame, its read
// completions are taken once a frame (BeginFrameTask: nlServiceFileSystem), and about 85 frames pass there whatever the card and the CPU can do, so a slice that has finished
// sits out the rest of a 33 ms period. No simulation runs in these states. Not in a LAN or online session, where the two consoles' loads are paced as before.
bool g_loadFast = false;
int g_loadMode = -1;
bool LoadPace(unsigned taskState, bool lan)
{
    if (g_loadMode < 0)
    {
        const char* e = getenv("MSC_LOAD_FPS");
        g_loadMode = (e && atoi(e) == 60) ? 1 : 0;
        if (g_loadMode) fprintf(stderr, "[fps] MSC_LOAD_FPS=60: a picture per refresh while a match loads\n");
    }
    return g_loadMode && !lan && (taskState & (0x200000u | 0x400000u)) != 0;   // TASK_FE_TO_GAME, TASK_GAME_TO_FE (Game/Task/GameTaskState.h)
}
// nxpace25 (log only, default ON): how long a match load or the way back to the menu takes, so the console A/B of MSC_LOAD_FPS / MSC_VITA_TEXCOPY reads one line instead of two [nav] lines and a stopwatch.
// One record per visit of the task states FE_TO_GAME / GAME_TO_FE (np_fps_load unit: load_fps_unit.cpp). Reads the clock and counts pictures; changes nothing.
struct LoadClock
{
    bool on = false;
    unsigned state = 0;
    double t0 = 0.0, last = 0.0;
    unsigned long long pics = 0;
};
#ifndef NP_LOADTR_BREAK
#define NP_LOADTR_BREAK 0   // host test mutants (load_fps_unit.cpp): 1 the pictures are not counted, 2 the time stops at the last picture inside the state, 3 a change from one load state to the other does not close the visit
#endif
// Returns true when a visit has just ended (state, ms and pictures say which).
bool LoadClockStep(LoadClock& c, unsigned taskState, double now, unsigned& state, double& ms, unsigned long long& pics)
{
    const unsigned load = taskState & (0x200000u | 0x400000u);
    bool ended = false;
    if (c.on && (load == 0 || (NP_LOADTR_BREAK != 3 && load != c.state)))
    {
        state = c.state;
        ms = ((NP_LOADTR_BREAK == 2 ? c.last : now) - c.t0) * 1000.0;
        pics = c.pics;
        c.on = false;
        ended = true;
    }
    if (load != 0)
    {
        if (!c.on) { c.on = true; c.state = load; c.t0 = now; c.pics = 0; }
        if (NP_LOADTR_BREAK != 1) ++c.pics;
        c.last = now;
    }
    return ended;
}
LoadClock g_loadClock;
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

static std::atomic<int> g_pairLive{0};   // the pair controller wants the pair cadence and the game is making pairs (set once a picture on the game thread, read on the display thread)

int NpFpsDisplayInterval(void)
{
    // Read on the display-queue thread (vgx_host_vita.cpp, the gate that holds a picture to its vblank): how many vblanks a picture is shown for.
    // Never starts the policy from that thread (the game thread's first frame does): until then there is no gate.
    if (!g.inited) return 0;
    if (g_pairLive.load(std::memory_order_relaxed)) return 1;   // wave 10 (MSC_PAIR_RATE=1 with MSC_BG30=2): the pair cadence: every picture is shown for one refresh
    switch (g.policy)
    {
    case kPolicy30: return g.paced ? 0 : ((g_bootFast == 1 || g_loadFast) ? 1 : 2);
    case kPolicy60: return 1;
    default: return 0;   // unlocked, the build's own pacing: no gate
    }
}

int NpFpsPolicy(void)
{
    Init();
    return g.paced ? (int)kPolicyUnlocked : (int)g.policy;
}

// ---- 1.3: between 30 and 60 (MSC_FPS_BETWEEN=pace, default 30; Settings > Frame Rate: UP TO 60) -----------------------------------------------------------------
// AUTO knows two rates. A scene whose pipeline manages a picture every 20-28 ms (35-50 a second) cannot hold 60, so it is shown at 30, and the game waits out the difference.
// With pace on, AUTO's 30 in a match (policy 30 in task state gameplay, outside a Mega Strike or the replay class, after the start-up) uses the game's own free-running swap
// mode 3 with no display gate: a picture per frame, held 1 or 2 refreshes, as many as the pipeline makes. The controller is not touched: it goes on deciding 60 or "30" from
// the same costs, so 60 is reached and left as before. A scene whose pipeline does not make more than about 34 pictures a second stays held (np_fps_between.h). Everything that looks at the rate keeps the 30 budget: the renderer's dynamic resolution (interval 0 is the 30 budget:
// no picture is made smaller for it), the GPU queue, the bounded slip (NpSlipDelta). Frames-in-flight (MSC_LAT_DEPTH) still holds the queues short, with one frame more: a picture held
// to its game time (MSC_PRESENT_SYNC) is reported shown later, and at the plain depth the gate then throttled a 42 fps scene to 34 on the model.
// Never in a LAN or online session: there this file behaves exactly as without the switch (presentation does not reach the fixed-step simulation, but AUTO's own 30/60 paths
// are the ones that have been played between two consoles). tools/fps/pace_mid_model.cpp has the comparison; the verdict on a console is still to come (docs/CONSOLE-TESTS.md).
int NpFpsPaced(void)
{
    return g.paced ? 1 : 0;
}

// Wave 7: a goal in play (Presentation::OnGoalScored, game thread). Notes the time (MSC_CADENCE: the controller waits for the cut that follows) and tells the renderer's front end (MSC_DL_WARM).
// Nothing here is read by the simulation or the network.
void NpFpsNoteGoal(void)
{
    Init();
    if (g.cad) g.cad->NoteGoal(NowSeconds());
    if (&VgxWarmGoal != 0) VgxWarmGoal();
}

// ---- 1.3: each picture on the vblank of its game time (MSC_PRESENT_SYNC; the Galaxy port's GXV_PRESENT_SYNC, vgx_present_sched.h PresentSync) --------------------------
// The game's picture k shows the world at the start of its frame (the renderer interpolates the 50 Hz steps to it), so the time between two pictures on the screen should be
// the time between the starts of their frames. Each frame's vblank at its start is kept here by frame number; the display callback asks for it (vgx_present.cpp SyncTarget).
namespace {
std::atomic<uint64_t> g_stamps[64];   // frame number << 16 | vblank (low 16 bits); 0: empty
}

int NpFpsPictureStamp(uint64_t tag, uint16_t* vcount)
{
    if (!tag) return 0;
    const uint64_t v = g_stamps[tag % 64].load(std::memory_order_acquire);
    if ((v >> 16) != (tag & 0xFFFFFFFFFFFFull)) return 0;
    *vcount = (uint16_t)(v & 0xFFFFu);
    return 1;
}

// 0 off, 1 measure only, 2 hold pictures. Read on the display-queue thread; never starts the policy from there. Unset: 2 while paced (the only rate here with no fixed cadence
// by default), 1 otherwise, so a log has the "present:" line and nothing else changes. A cadence the display gate already holds (policy 30), or a LAN session, is only measured.
int NpFpsPresentSyncMode(void)
{
    if (!g.inited) return 0;
    const int want = g.presentSync >= 0 ? g.presentSync : (g.paced ? 2 : 1);
    if (want < 2) return want;
    if (NpFpsDisplayInterval() == 2 || (&NpLanActive != 0 && NpLanActive())) return 1;
    return 2;
}

int NpFpsDescribe(char* out, int size)
{
    Init();
    static const char* const modeName[6] = {"game", "30", "60", "unlocked", "auto", "45"};
    const char* le = getenv("MSC_LATE_WAIT");
    const char* ld = getenv("MSC_LAT_DEPTH");
    int n = snprintf(out, (size_t)size, "fps=%s (policy now %s", modeName[g.mode], kPolicyName[g.policy]);
    if (n > 0 && n < size)   // (the replay class is listed in every mode: the Settings row REPLAYS & CUT-SCENES is only in force under auto, and the log says so)
        n += snprintf(out + n, (size_t)size - (size_t)n, "; replay %s%s", g.replayMode == 1 ? "30" : (g.replayMode == 2 ? "auto" : "fit"), g.mode == kModeAuto ? "" : " (not used in this mode)");
    if (g.mode == kModeAuto && n > 0 && n < size)
        n += snprintf(out + n, (size_t)size - (size_t)n, "; hysteresis %d, mega %s, between %s", g.hyst ? g.hyst : 2, getenv("MSC_FPS_MEGA") && IEqual(getenv("MSC_FPS_MEGA"), "auto") ? "auto" : "30",
                      g.betweenMode ? "pace" : "30");
    if (n > 0 && n < size)
        n += snprintf(out + n, (size_t)size - (size_t)n, "); late wait %s, frames in flight %s, gate anchor %s, retry %s", (le && *le) ? le : "default", (ld && *ld) ? ld : "2",
                      LateAnchorBeforeGate() ? "before" : "after", (getenv("MSC_LAT_RETRY") && *getenv("MSC_LAT_RETRY") == '0') ? "off" : "on");
    if (n > 0 && n < size && getenv("MSC_SLIP") && *getenv("MSC_SLIP") && *getenv("MSC_SLIP") != '0')
        n += snprintf(out + n, (size_t)size - (size_t)n, "; bounded slip %s", getenv("MSC_SLIP"));
    return n;
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
    if (g45.live) return 0;   // helper f45: no wait at the swap; the back-pressure gate after the send (NpFpsLateWaitAfterSend) and the display's slots set the pace
    switch (g.policy)
    {
    case kPolicy30:
        if (g.paced && !BootFast()) return gameMode == 0 ? 0 : 3;   // (MSC_FPS_BETWEEN=pace: see NpFpsPaced)
        return (BootFast() || g_loadFast) ? (gameMode == 0 ? 0 : 1) : 4;
    case kPolicy60: return gameMode == 0 ? 0 : 1;
    case kPolicyUnlocked: return gameMode == 0 ? 0 : 3;
    default: return gameMode;
    }
}

// ---- helper f45: the display's side (vgx_host_vita.cpp GateDisplay, on the display-queue thread; a host build calls it from the game thread, F45HostPace) ----------------------------
namespace {
npfps::Gate45 g45Gate;                // display thread only
npfps::LatencyLearner g45Learn;       // display thread only
npfps::Gate45 g60Gate(npfps::Gate45Config(), true);   // (MSC_LOCK60_TIERS) the 60 tier's pictures: every vblank is a slot
npfps::LatencyLearner g60Learn;
bool g45CurIs60 = false;
bool g45PrevIs60 = false;             // (f50) the picture decided before this one was a 60-tier picture: the strict 50 entered from the 60 keeps that picture at 1 refresh (no repeat at the seam)
uint64_t g45CurTag = 0;               // the picture NpFps45Due was just asked about
uint32_t g45CurG = 0;
bool g45CurHasT = false;
#if !defined(__vita__)
uint32_t g45HostLast = 0;
bool g45HostHave = false;
#endif
extern "C++" void F45SetPattern(const npfps::Pattern45& pat, bool seamless)
{
    npfps::Gate45Config gc;
    gc.enterOnTarget = seamless;   // (f50, the strict 50 only: the first picture of a stretch goes up on the slot its seeded game time stands for)
    g45Gate = npfps::Gate45(gc, false, pat);
    g60Gate = npfps::Gate45(gc, true);
}
}
// A 45 picture (its frame was made at 45: np_fps.cpp left a record under its number)? Then *due = the vblank it is to go up on (Gate45: its next slot, its predicted slot when later, or the
// first slot it can make), and 1; the caller waits for it. 0: not a 45 picture (the 30 rule of the display gate applies; a 45 stretch that was in force ends). earliest: the first vblank the
// picture can be on; prevShown / havePrev: the vblank of the picture before it.
extern "C" uint64_t np_time_real_ns(void) __attribute__((weak));
extern "C" int NpFps45Due(uint64_t tag, uint32_t earliest, uint32_t prevShown, int havePrev, uint32_t* due)
{
    g45CurTag = 0;
    const uint64_t st = tag ? g45Stamp[tag % 64].load(std::memory_order_acquire) : 0;
    if (!tag || (uint32_t)(st >> 32) != F45Tag(tag)) { if (g45Gate.Have()) g45Gate.Leave(); if (g60Gate.Have()) g60Gate.Leave(); g45PrevIs60 = false; return 0; }
    const uint64_t tg = g45Target[tag % 64].load(std::memory_order_relaxed);
    const bool hasT = (st & 0x80000000ull) != 0 && (uint32_t)(tg >> 32) == F45Tag(tag);
    const uint32_t natural = (earliest - (uint32_t)(st & 0x3FFFFFFFull)) & 0x3FFFFFFFu;
    if ((st & 0x40000000ull) != 0)
    {
        // a 60-tier picture (MSC_LOCK60_TIERS): the next vblank after the one before, or the vblank its game time asks for when that is later, or the first it can make
        if (g45Gate.Have()) g45Gate.Leave();
        if (natural <= 15) { g60Learn.Add(natural); if (g60Learn.Ready()) g60L.store(g60Learn.Quantile(g45.lq60 > 0.0 ? g45.lq60 : g45.lq), std::memory_order_relaxed); }
        *due = g60Gate.Due(earliest, hasT, (uint32_t)tg, havePrev != 0 && !g60Gate.Have(), havePrev != 0 ? prevShown - 1u : 0u);   // (the picture before keeps 1 refresh, not the 45 entry's 2)
        g45CurTag = tag;
        g45CurG = (uint32_t)tg;
        g45CurHasT = hasT;
        g45CurIs60 = true;
        g45PrevIs60 = true;
        return 1;
    }
    g45CurIs60 = false;
    if (g60Gate.Have()) g60Gate.Leave();
    if (natural <= 15) { g45Learn.Add(natural); if (g45Learn.Ready()) g45L.store(g45Learn.Quantile(g45.lq), std::memory_order_relaxed); }
    // (Fable) the lead's evidence: was this picture ready after its slot, and, for the picture after the long hold (the only one the display can see early, and the heaviest: the
    // 2-refresh advance), how long before its slot's vblank it was ready
    const bool had = g45Gate.Have();
    const unsigned prevHold = had ? g45Gate.Pat().Hold(g45Gate.LastShown(), g45Gate.Phase()) : 0u;
    const uint32_t resyncs0 = g45Gate.resyncs_;
    // (f50, strict 50 only: the 45 enters exactly as before) the picture before a 50 stretch that was a 60 picture is held 1 refresh, like at 50's own pace, not the 30's 2: no repeat at the 60 -> 50 seam
    const uint32_t entryPrev = (g45.use50 && g45PrevIs60) ? prevShown - 1u : prevShown;
    g45PrevIs60 = false;
    *due = g45Gate.Due(earliest, hasT, (uint32_t)tg, havePrev != 0 && !g45Gate.Have(), entryPrev);
    if (had && g45Gate.resyncs_ == resyncs0)
    {
        double marginMs = -1.0;
#if defined(__vita__)
        uint32_t v = 0;
        uint64_t ns = 0;
        if (&np_display_last_vblank != 0 && &np_time_real_ns != 0 && np_display_last_vblank(&v, &ns))
        {
            const int32_t dv = (int32_t)(*due - v);
            const double m = (double)dv * (1001.0 / 60.0) - (double)(int64_t)(np_time_real_ns() - ns) * 1e-6;
            if (dv >= 0 && dv < 16 && m > -2.0 && m < 120.0) marginMs = m < 0.0 ? 0.0 : m;
        }
#endif
        const npfps::Lead45Verdict lv = npfps::Lead45Verdict::Of(*due, earliest, prevHold, marginMs, g45Gate.LastLate(), g45Gate.LastPlanned(), npfps::Lead45Config().guardMs, 1001.0 / 60.0);
        if (lv.judged) g45d[kD45Judged].fetch_add(1, std::memory_order_relaxed);
        if (lv.roomy) g45d[kD45Roomy].fetch_add(1, std::memory_order_relaxed);
    }
    g45CurTag = tag;
    g45CurG = (uint32_t)tg;
    g45CurHasT = hasT;
    return 1;
}
// The 45 picture just decided went up on vblank `shown`: the gate's record, the counters, the interpolation error (its game time's slot against where it really is), the report for the clock.
extern "C" void NpFps45Shown(uint64_t tag, uint32_t shown)
{
    if (!g45CurTag || tag != g45CurTag) return;
    g45CurTag = 0;
    if (g45CurIs60)
    {
        const uint32_t before = g60Gate.holds_[2] + g60Gate.holds_[3];
        g60Gate.Shown(shown);
        g60d[kD60Late].store(g60Gate.late_, std::memory_order_relaxed);
        g60d[kD60H2].fetch_add(g60Gate.holds_[2] + g60Gate.holds_[3] - before, std::memory_order_relaxed);
        if (g45CurHasT) { g60d[kD60ErrN].fetch_add(1, std::memory_order_relaxed); if (shown != g45CurG) g60d[kD60ErrOff].fetch_add(1, std::memory_order_relaxed); }
        g60d[kD60Shown].store(g60Gate.pictures_, std::memory_order_release);
        g45Fb.store(((uint64_t)((F45Tag(tag) << 3) | npfps::Pattern45::kEvery) << 32) | shown, std::memory_order_release);
        return;
    }
    g45Gate.Shown(shown);
    const npfps::Gate45& q = g45Gate;
    g45d[kD45Shown].store(q.pictures_, std::memory_order_relaxed);
    g45d[kD45Late].store(q.late_, std::memory_order_relaxed);
    g45d[kD45Planned].store(q.planned_, std::memory_order_relaxed);
    g45d[kD45Off].store(q.offPattern_, std::memory_order_relaxed);
    g45d[kD45Resyncs].store(q.resyncs_, std::memory_order_relaxed);
    g45d[kD45Stalls].store(q.stalls_, std::memory_order_relaxed);
    g45d[kD45H1].store(q.holds_[1], std::memory_order_relaxed);
    g45d[kD45H2].store(q.holds_[2], std::memory_order_relaxed);
    g45d[kD45H3].store(q.holds_[3], std::memory_order_relaxed);
    g45d[kD45HoldSum].store(q.holdSum_, std::memory_order_relaxed);
    if (g45CurHasT) {
        const int32_t e = (int32_t)(shown - g45CurG);
        const uint32_t ae = (uint32_t)(e < 0 ? -e : e);
        g45d[kD45ErrN].fetch_add(1, std::memory_order_relaxed);
        if (ae) { g45d[kD45ErrOff].fetch_add(1, std::memory_order_relaxed); g45d[kD45ErrAbs].fetch_add(ae, std::memory_order_relaxed); if (ae > g45ErrWorst.load(std::memory_order_relaxed)) g45ErrWorst.store(ae, std::memory_order_relaxed); }
    }
    g45d[kD45Slips].store(q.slips_, std::memory_order_release);   // (last: the game thread reads the shown count first)
    g45Fb.store(((uint64_t)((F45Tag(tag) << 3) | (q.Phase() & 7u)) << 32) | shown, std::memory_order_release);
}
// The renderer's picture ladder (vgx_backend_gl.cpp): the frame being drawn was made at 45 (its budget is 22.2 ms, not the 30's 33.4).
extern "C" int NpFps45Live(void) { return g45Live.load(std::memory_order_relaxed); }

// ---- wave 11: back-pressure before the pad (MSC_LAT_BP, default 1; np_fps_bp.h) -------------------------------------------------------------------
// Replaces the frames-in-flight limit under a held cadence (30 or 60, single player): the game thread starts a picture, and reads the pad, only when at most `depth` earlier
// pictures are unshown (depth 1 at a clean 30), and in a heavy moment it waits here, one refresh at a time, at most 6, instead of standing aside; the depth is found by np_fps_bp.h
// (raised when late pictures follow a wait at this gate and the raise helps, lowered when the cadence is clean). Free runs keep the old limit. MSC_LAT_BP=0: the old limit.
// Not measured on the console: host model only (tools/fps/bp_unit.cpp).
extern "C" uint64_t NpLatHandedOverTag(void) __attribute__((weak));
extern "C" uint64_t NpLatShownTag(void) __attribute__((weak));
namespace {
// held24 (MSC_LAT_BP_HELD_DEPTH=1..4, default 2): the gate's depth in a held scene, so at most depth + 1 pictures are in flight (the first heldpipe23 build allowed 5-6: sound ahead of the picture by 0.3 s)
npfps::BpConfig BpCfgFromEnv() { npfps::BpConfig c; const char* e = getenv("MSC_LAT_BP_HELD_DEPTH"); if (e && *e) { const int v = atoi(e); if (v >= 1 && v <= (int)c.maxDepth) c.heldDepth = (unsigned)v; } return c; }
npfps::BackPressure g_bp(BpCfgFromEnv());
uint32_t g_bpHeldPics = 0, g_bpHeldWaited = 0, g_bpRelPics = 0, g_bpRelWaited = 0;   // held24 action counters: held pictures through the gate / that waited; pictures in the release window after a held scene / that waited
uint32_t g_padSleepUs = 0;   // the pad late sleep put into the frame being reported (lessons17: the waits it counted are ours, not the cause of a late picture; MSC_LAT_BP_SLEEP takes it off the gate's room)
unsigned long g_bpLastEntry = 0;
double g_bpBusyMs = 20.0;
bool g_bpHaveEntry = false;
bool g_bpHeldNow = false;   // MSC_LAT_BP_HELD: this picture is a replay / cut-scene / walk-out (set at the picture's begin)
bool BpHeldOn() { static const bool v = [] { const char* e = getenv("MSC_LAT_BP_HELD"); const bool on = !(e && *e && atoi(e) == 0); if (on) fprintf(stderr, "[fps] MSC_LAT_BP_HELD=1: in a held scene (replay, cut-scene, walk-out) the back-pressure gate waits only on a backlog of more than %u pictures (MSC_LAT_BP_HELD_DEPTH, so at most that many + 1 in flight); the learned depth comes back at the cut to play, with one refresh of wait at most for %u pictures\n", BpCfgFromEnv().heldDepth, BpCfgFromEnv().releasePics); return on; }(); return v; }
uint32_t g_bpFrames = 0, g_bpWaited = 0, g_bpRefreshes = 0, g_bpMaxWait = 0, g_bpCapped = 0;
}
static bool BackPressureGate()
{
    static const bool on = [] { const char* e = getenv("MSC_LAT_BP"); const bool v = !(e && *e && atoi(e) == 0); if (v) fprintf(stderr, "[fps] back-pressure on (MSC_LAT_BP=1): a picture starts, and the pad is read, only when at most 1 to 4 earlier pictures are unshown (found by the cadence), waiting at most 6 refreshes\n"); return v; }();
    if (!on || !NpLatHandedOverTag || !NpLatShownTag) return false;
    if (&NpLanActive != 0 && NpLanActive()) return false;
    const int shownFor = NpFpsDisplayInterval();
    if (shownFor <= 0 || g.paced || g.policy == kPolicyGame) { g_bpHaveEntry = false; return false; }
    const int interval = shownFor == 2 ? HeldPeriod() : shownFor;   // wave 11: a held picture at the steady 3 has 3 refreshes of room
    npfps::BackPressure& bp = g_bp;   // (a 45 frame never comes here: F45Gate)
    const unsigned long now = VIGetRetraceCount();
    const unsigned long gap = now - g_bpLastEntry;
    const bool haveGap = g_bpHaveEntry && gap <= 8;
    g_bpLastEntry = now;
    g_bpHaveEntry = true;
    bp.SetHeld(g_bpHeldNow);
    const uint64_t sent = NpLatHandedOverTag();
    // the refreshes this picture can wait and still be finished in its period: the period less the thread's own busy time (the last frames' length less their waits) less 3 ms
    const double busyMs = g.lastInterval * 1000.0 - (double)g.waitNsPrev * 1e-6;
    if (busyMs > 0.0 && busyMs < 200.0) g_bpBusyMs += (busyMs - g_bpBusyMs) / 8.0;
    static const bool sleepOff = [] { const char* e = getenv("MSC_LAT_BP_SLEEP"); return e && *e && atoi(e) > 0; }();   // lessons17 (default off): the pad late sleep that follows the gate comes out of the same room
    const unsigned slack = npfps::GateRoomRefreshes((double)interval * (1001.0 / 60.0), g_bpBusyMs, sleepOff ? g_padSleepUs : 0u, 3.0, 1001.0 / 60.0);
    unsigned waited = 0;
    while (bp.Waits(sent, NpLatShownTag(), waited, slack)) { VIWaitForRetrace(); ++waited; }
    if (bp.Held()) { ++g_bpHeldPics; if (waited) ++g_bpHeldWaited; }
    if (bp.ReleaseLeft()) { ++g_bpRelPics; if (waited) ++g_bpRelWaited; }
    bp.After(waited, static_cast<unsigned>(gap), haveGap, interval);
    ++g_bpFrames;
    if (waited) { ++g_bpWaited; g_bpRefreshes += waited; if (waited > g_bpMaxWait) g_bpMaxWait = waited; if (waited >= bp.MaxRefreshes()) ++g_bpCapped; }
    if (g_bpFrames >= 1800) {
        fprintf(stderr, "[fps] back-pressure: depth now %u; %u of the last %u pictures waited before the pad (%u refreshes, at most %u in a row, %u at the cap); raised %u times, %u raises taken back (the GPU was the limit), %u descents; held scenes: %u pictures through the gate, %u waited (depth %u); release after a held scene: %u pictures, %u waited%s\n",
                bp.Depth(), g_bpWaited, g_bpFrames, g_bpRefreshes, g_bpMaxWait, g_bpCapped, bp.Raises(), bp.Reverted(), bp.Descents(), g_bpHeldPics, g_bpHeldWaited, bp.GateDepthHeld(), g_bpRelPics, g_bpRelWaited, bp.Aside() ? ", standing aside (the display reports nothing)" : "");
        g_bpFrames = g_bpWaited = g_bpRefreshes = g_bpMaxWait = g_bpCapped = 0;
    }
    return true;
}

// ---- 1.1: frames in flight (MSC_LAT_DEPTH) -----------------------------------------------------------------------------------------------------
// Console test, 23 Oct: a button reached the screen 150-175 ms after it was read at 30 fps. Every stage after the game thread (frame send, decode,
// back end, GPU, display) is faster than a frame, but the display shows a picture only every second refresh, so each stage's queue filled up and
// every frame waited about a frame at every hand-off. The game thread now starts the next frame (and reads the pads) only when at most
// MSC_LAT_DEPTH of its frames are still on their way to the display (default 2; 0 = off). The wait is at most one refresh per probe and
// switches itself off when it keeps hitting that limit (a pipeline that cannot go faster), backing off when the cadence slips.
namespace {
int g_fifDepth = -1, g_fifCapHits = 0;
// MSC_LAT_RETRY (default 1): a gate that switched itself off ("the display cannot keep up") tries again after a growing number of frames
// (900, 1800, 3600, 7200 at 30 fps: 30 s to 4 min) instead of staying off for the rest of the session: a heavy stretch (GPU-bound scene, a
// replay) must not cost the input-latency gain of the next calm one. 0: off for good (1.1).
int g_fifRetry = -1;
uint32_t g_fifCalls = 0, g_fifRetryAt = 0, g_fifStrikes = 0;
uint32_t g_fifDrains = 0, g_fifWaits = 0, g_fifFrames = 0, g_fifVblanks = 0, g_fifPaused = 0, g_fifPauses = 0;
unsigned long g_fifLastCheck = 0;
bool g_fifHaveCheck = false;
}
extern "C" uint64_t NpLatHandedOverTag(void) __attribute__((weak));
extern "C" uint64_t NpLatShownTag(void) __attribute__((weak));
// (frames are numbered at the hand-over (np_latency.cpp NpLatLatch) and the display reports the newest number it was given: pictures the platform
// draws itself carry no number and do not count)
static void FramesInFlightGate()
{
    if (BackPressureGate()) return;   // wave 11: a held cadence has its own limit (above)
    if (g_fifDepth < 0) {
        const char* e = getenv("MSC_LAT_DEPTH");
        g_fifDepth = (e && *e) ? atoi(e) : 2;
        if (g_fifDepth < 0 || g_fifDepth > 8) g_fifDepth = 2;
        fprintf(stderr, "[fps] frames in flight: at most %d after the hand-over (MSC_LAT_DEPTH; 0 = off)\n", g_fifDepth);
    }
    if (g_fifDepth == 0 || !NpLatHandedOverTag || !NpLatShownTag) return;
    if (g_fifRetry < 0) {
        const char* e = getenv("MSC_LAT_RETRY");
        g_fifRetry = (e && *e) ? (atoi(e) != 0) : 1;
    }
    ++g_fifCalls;
    const uint64_t sent = NpLatHandedOverTag();
    if (g_fifCapHits >= 30 && g_fifRetry && g_fifCalls >= g_fifRetryAt) {   // (the cut-off of a pipeline that could not go faster is tried again)
        g_fifCapHits = 0;
        g_fifHaveCheck = false;
        fprintf(stderr, "[fps] frames in flight: trying the limit again\n");
    }
    if (NpLatShownTag() == 0 || g_fifCapHits >= 30) return;   // (nothing shown with a number yet; or a pipeline that cannot go faster)
    // Back off when a frame took longer than its two refreshes, including an extra refresh introduced by this gate.
    // The limit stands aside for 150 frames. (First console run: always waiting cost 5 fps in heavy scenes.)
    const unsigned long now = VIGetRetraceCount();
    const bool late = g_fifHaveCheck && static_cast<long>(now - g_fifLastCheck) > HeldPeriod();
    if (late) { if (g_fifPaused == 0) ++g_fifPauses; g_fifPaused = 150; }
    if (g_fifPaused) {
        // wave 10 (MSC_LAT_DRAIN, plan 1): at a held 30 the standing aside still drains a backlog of more than depth + 1 pictures (np_fps_fif.h)
        static const bool drain = [] { const char* e = getenv("MSC_LAT_DRAIN"); return e && *e && atoi(e) != 0; }();
        const bool held = NpFpsDisplayInterval() == 2 && !g.paced;
        if (drain && npfps::FifWaits(true, held, sent - NpLatShownTag(), static_cast<uint64_t>(g_fifDepth), true) && sent > NpLatShownTag()) { VIWaitForRetrace(); ++g_fifDrains; }
        --g_fifPaused; g_fifLastCheck = now; g_fifHaveCheck = true; return;
    }
    ++g_fifFrames;
    int waited = 0;
    // (paced: NpFpsPaced above.) Wave 6, MSC_LAT_DEPTH_PACED=0|1|2 (default 1, as before): the frames more than MSC_LAT_DEPTH that a free-running pace may keep on
    // their way. Every frame in flight in a pipeline-bound scene is a frame of button delay (3 x 24 ms = 72 ms at 42 pictures a second): 0 is the
    // console A/B of that against the throughput the model found with 1.
    static const unsigned pacedExtra = [] { const char* e = getenv("MSC_LAT_DEPTH_PACED"); const int v = (e && *e) ? atoi(e) : 1; return (unsigned)(v < 0 ? 0 : v > 3 ? 3 : v); }();
    const uint64_t depth = static_cast<uint64_t>(g_fifDepth) + (g.paced ? pacedExtra : 0u);
    while (sent > NpLatShownTag() + depth && waited < 1) {
        VIWaitForRetrace();
        ++waited;
    }
    if (waited) { ++g_fifWaits; g_fifVblanks += static_cast<uint32_t>(waited); }
    // Thirty unresolved gated waits without a successful probe disable the limit; paused frames do not count as probes.
    if (waited >= 1 && sent > NpLatShownTag() + depth) ++g_fifCapHits; else g_fifCapHits = 0;
    // Measure entry to entry, including this gate's own wait (exit to entry would hide a refresh the gate itself added). With MSC_LAT_ANCHOR=1
    // the gate's refresh is inside the two-refresh period, so a gated frame is not "late"; with 0 it is, and the gate stands aside afterwards.
    g_fifLastCheck = now;
    g_fifHaveCheck = true;
    if (g_fifCapHits == 30) {
        const uint32_t span = 900u << (g_fifStrikes < 3 ? g_fifStrikes : 3);
        g_fifRetryAt = g_fifCalls + span;
        if (g_fifStrikes < 8) ++g_fifStrikes;
        fprintf(stderr, "[fps] frames in flight: the display cannot keep up with the limit, the limit is off%s\n", g_fifRetry ? " (for now)" : "");
    }
    if (g_fifFrames >= 1800) {
        fprintf(stderr, "[fps] frames in flight: %u of the last %u frames waited for the display (%u refreshes); stood aside %u times for heavy moments, %u waits drained a backlog while standing aside\n", g_fifWaits, g_fifFrames, g_fifVblanks, g_fifPauses, g_fifDrains);
        g_fifFrames = g_fifWaits = g_fifVblanks = g_fifPauses = g_fifDrains = 0;
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
    if ((g45.live || g45.live60) && &NpLatHandedOverTag != 0)
    {
        // helper f45: this frame's number now exists: the display finds by it that the picture is a 45 one, the slot its game time stands for, and the vblank it began at
        const uint64_t tag = NpLatHandedOverTag();
        npfps::Clock45& ck = g45.live ? g45.clock : g45.clock60;
        ck.Handed(F45Tag(tag));
        uint32_t gs = 0;
        const bool ht = ck.NewestTarget(&gs);
        const uint64_t hi = (uint64_t)F45Tag(tag) << 32;
        g45Target[tag % 64].store(hi | gs, std::memory_order_relaxed);
        g45Stamp[tag % 64].store(hi | (ht ? 0x80000000ull : 0ull) | (g45.live ? 0ull : 0x40000000ull) | (uint64_t)(g45.stamp & 0x3FFFFFFFu), std::memory_order_release);
    }
    if (&NpLatHandedOverTag != 0 && &NpSmoothHanded != 0) NpSmoothHanded(NpLatHandedOverTag());   // wave 9 (np_smooth.cpp): this frame's number, for the show clock and the [smooth] line
    if (g.haveTurn && &NpLatHandedOverTag != 0)
    {
        // 1.3 (MSC_PRESENT_SYNC): this frame's number now exists: it goes with the vblank its frame began at
        const uint64_t tag = NpLatHandedOverTag();
        g_stamps[tag % 64].store(((tag & 0xFFFFFFFFFFFFull) << 16) | (uint64_t)(g.turnVcount & 0xFFFFu), std::memory_order_release);
    }
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
    if (g.mode != kModeAuto && g.mode != kMode45 && g.logLevel == 0) return;
    g.simStartMs = g.threadClock ? g.threadClock() : 0.0;
    g.simStartNs = NowNs();
}

void NpFpsSimStep(void)
{
    ++g.simSteps;
    SlipNoteStep();
}

void NpFpsSimEnd(void)
{
    if (g.mode != kModeAuto && g.mode != kMode45 && g.logLevel == 0) return;
    const double t = g.threadClock ? g.threadClock() : 0.0;
    if (t > g.simStartMs) g.simMsAcc += t - g.simStartMs;
    { const int64_t dn = NowNs() - g.simStartNs; if (dn > 0) g.simWallAccMs += (double)dn * 1e-6; }
}

// pace18: NpSimtRun (src/Game/NpSimRun17.cpp), game thread, after each join (MSC_SIMT=1|2): the picture's window (mode 2), the wait at the join and np_sim's wall time for the step, in microseconds.
void NpFpsSimOverlapNote(int mode, double windowUs, double joinUs, double simWallUs)
{
    g.simWinMsAcc += windowUs * 1e-3;
    g.simJoinMsAcc += joinUs * 1e-3;
    g.simWallMsAcc += simWallUs * 1e-3;
    g.simNoteMode = mode;
    g.simNoteFrame = g.frames;
}

// the mode the controller prices with: the last note's, while notes keep coming (a picture without a kick is no reason to change it; 30 pictures without one = the switch is not in force)
static int SimModeNow()
{
    return (g.simNoteMode != 0 && g.frames - g.simNoteFrame <= 30) ? g.simNoteMode : 0;
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

namespace { bool LateOn(); }   // (below: the late wait; MSC_HELD_CADENCE needs it)

// lessons17 (MSC_LATE_CAUSE=1, default 1, logging only; the lanes it feeds are also fed when MSC_HELD_CADENCE=auto, whose exit reads them): the "[late]" line, see np_late_cause.h. The renderer's threads feed the lanes (NpLateLaneNote), the game thread judges at each picture's begin.
namespace {
nplc::Ledger g_lc;
nphead::Stat g_head;   // wave 17 (MSC_HEAD_STAT, np_head17.h)
int g_headOn = -1;
int g_lcOn = -1;
bool LateCauseOn() { if (g_lcOn < 0) { const char* e = getenv("MSC_LATE_CAUSE"); g_lcOn = (e && *e && atoi(e) == 0) ? 0 : 1; if (g_lcOn) fprintf(stderr, "[late] on (MSC_LATE_CAUSE=1, logging only): every 300 live pictures a line charges each late picture (held more refreshes than the cadence) to the GPU, game CPU, render-thread CPU, front-end CPU or waits: a lane counts when its time in the picture (the largest of the last 3 for the renderer's lanes) is 80 %% of the period or more, else waits when the game thread's waits (less the pad late sleep) are 40 %% of the gap; an estimate, the pipeline is 2-3 pictures deep\n"); } return g_lcOn > 0; }
}
// held24: MSC_HELD_CADENCE's exit reads the lanes, so they are fed when the cadence is on even with MSC_LATE_CAUSE=0 (the lanes are not just for the [late] line any more)
static bool HeldCadenceWanted() { const char* hc = getenv("MSC_HELD_CADENCE"); return hc && IEqual(hc, "auto"); }
static bool CrowdGovOnLate() { const char* e = getenv("MSC_CROWD_GOV"); return e && *e && atoi(e) > 0; }   // crowd25: the governor reads the lanes
extern "C" int NpLateCauseOn(void) { 
#if NP_BP_BREAK == 12
    return LateCauseOn() ? 1 : 0;   // (host mutant: the lanes only for the [late] line)
#else
    return (LateCauseOn() || HeldCadenceWanted() || CrowdGovOnLate()) ? 1 : 0;
#endif
}
static double HeldLaneMaxMs() { double m = 0; for (int i = 0; i < 3; ++i) { const double v = g_lc.lane[i].Recent(); if (v > m) m = v; } return m; }
extern "C" void NpLateLaneNote(int lane, float ms) { if (lane >= 0 && lane < 3 && ms >= 0.0f && ms < 1000.0f) { g_lc.lane[lane].Note(ms); g_head.LaneNote(lane, ms); } }
extern "C" int vgx_host_dq_window(int id, unsigned* n, float* meanMs, float* maxMs, unsigned* over8, unsigned* over33) __attribute__((weak));   // diag13 (vgx_host_vita.cpp, MSC_DQ_TIMING): the display queue call's window for the [late] line
extern "C" int VgxLateCostTake(double* usPerPicture, unsigned long* reads) __attribute__((weak));   // vgx_draw.cpp: what the renderer's clock reads for the line cost


// crowd25 (MSC_CROWD_GOV, np_crowdgov.h): the adaptive crowd-cost governor. Default off here (the Vita plan sets it). MSC_CROWD_GOV_FORCE=n holds level n (0..5) with no controller (host pictures / proofs).
// The level is read by the render side (Game/Render/ImpostorManager.cpp) once per picture. Render only: nothing the simulation reads.
namespace {
npcg::Gov g_cgov;
int g_cgOn = -1, g_cgForce = -1, g_cgFloor60 = -1, g_cgFloorLive = -1, g_cgAt60 = 0;   // (core60: MSC_CROWD_FLOOR60, int33: MSC_CROWD_FLOOR60_LIVE, and whether this picture is live play at 60 / Smooth-60 live relief)
std::atomic<int> g_cgLiveRelief{0};   // int33: updated every picture regardless of MSC_CROWD_GOV/FORCE so AmortizeCrowdSixty sees Smooth-60 live open play cleanly
struct CgScript { unsigned long frame; int level; } g_cgScript[16];
int g_cgScriptN = 0;
unsigned long g_cgCount[24];   // 12..19: sprite nearest-figure height histogram (lines: <20 <40 <60 <80 <100 <140 <200 more)
   // 8..10: model-picture samples in size class 1 (near), 2, 3 (far)
// (0..7:)   // 0 sprites measured, 1 notch moves, 2 captures kept (due), 3 captures skipped, 4 frozen-section captures, 5 pictures governed
int CrowdGovOn()
{
    if (g_cgOn < 0)
    {
        const char* e = getenv("MSC_CROWD_GOV");
        g_cgOn = (e && *e && atoi(e) > 0) ? 1 : 0;
        const char* f = getenv("MSC_CROWD_GOV_FORCE");
        if (f && *f) { g_cgForce = atoi(f); if (g_cgForce < 0) g_cgForce = 0; if (g_cgForce > npcg::kMaxLevel) g_cgForce = npcg::kMaxLevel; g_cgOn = 1; }
        if (const char* sc = getenv("MSC_CROWD_GOV_SCRIPT"))   // host pictures only: "frame:level,frame:level" (the level from that picture on, whatever the controller says)
        {
            for (const char* p = sc; *p && g_cgScriptN < 16;)
            {
                char* e = 0; const unsigned long fr = strtoul(p, &e, 10);
                if (!e || *e != ':') break;
                const int lv = atoi(e + 1);
                g_cgScript[g_cgScriptN].frame = fr; g_cgScript[g_cgScriptN].level = lv < 0 ? 0 : (lv > npcg::kMaxLevel ? npcg::kMaxLevel : lv); ++g_cgScriptN;
                p = strchr(e, ','); if (!p) break; ++p;
            }
            if (g_cgScriptN) { g_cgOn = 1; if (g_cgForce < 0) g_cgForce = 0; }
        }
        { const char* pc = getenv("MSC_CROWD_PEND_CLEAR"); g_cgov.SetClearPend(!(pc && *pc && atoi(pc) == 0)); }   // (nxr4a, default on: a saving is not learned across a replay / cut-scene / menu or a change of period)
        if (g_cgOn) fprintf(stderr, "[crowdgov] on (MSC_CROWD_GOV=1%s): live play only, the crowd sprites' refresh gets longer by size class (far first) while the game thread is the limiting lane and over 87 %% of the period, and comes back when there is room\n", g_cgForce >= 0 ? ", FORCED level" : "");
    }
    return g_cgOn;
}
}
extern "C" uint64_t NpHarnessFrameCount(void) __attribute__((weak));   // host harness: the presented-frame count (the unit of MSC_CROWD_GOV_SCRIPT)
extern "C" int NpCrowdGovLevel(void)
{
    if (!CrowdGovOn()) return 0;
    if (g_cgScriptN) { int lv = 0; const unsigned long fnow = &NpHarnessFrameCount != 0 ? (unsigned long)NpHarnessFrameCount() : (unsigned long)g.frames; for (int i = 0; i < g_cgScriptN; ++i) if (fnow >= g_cgScript[i].frame) lv = g_cgScript[i].level; return lv; }
    const int lv = g_cgForce >= 0 ? g_cgForce : g_cgov.Level();
    if (g_cgFloor60 < 0) { const double f = EnvDouble("MSC_CROWD_FLOOR60", 0.0); g_cgFloor60 = f < 0.0 ? 0 : (f > npcg::kMaxLevel ? npcg::kMaxLevel : static_cast<int>(f)); }
    if (g_cgFloor60 == 0 || g_cgForce >= 0) return lv;   // (forced levels are host proofs: not touched)
    const int out = npcg::FloorAt60(lv, g_cgFloor60, g_cgAt60 != 0);
    static int s_said = -1;
    if (out != lv && s_said != 1) { s_said = 1; fprintf(stderr, "[crowdgov] MSC_CROWD_FLOOR60=%d: %s holds the crowd at level %d or more (the governor is at %d) from frame %llu\n", g_cgFloor60, g_cgLiveRelief.load(std::memory_order_relaxed) ? "Smooth 60 live play" : "live play at 60", g_cgFloor60, lv, (unsigned long long)g.frames); }
    else if (out == lv && s_said == 1 && !g_cgAt60) { s_said = 0; fprintf(stderr, "[crowdgov] MSC_CROWD_FLOOR60=%d: released (not live play in 60 mode now; the governor's level %d) at frame %llu\n", g_cgFloor60, lv, (unsigned long long)g.frames); }
    return out;
}
// int33: true while Smooth 60 (MSC_LOCK60=1) with MSC_CROWD_FLOOR60_LIVE=1 is in live open play (excludes Quality 30, menus, walk-outs, cut-scenes, replays, Mega Strikes).
extern "C" int NpCrowdLiveRelief(void)
{
    return g_cgLiveRelief.load(std::memory_order_relaxed);
}
extern "C" void NpCrowdGovCount(int which, unsigned n) { if (which >= 0 && which < 24) g_cgCount[which] += n; }
static void CrowdGovPicture(double now, double interval, double R, unsigned taskState)
{
    if (g_cgFloorLive < 0) { const char* fl = getenv("MSC_CROWD_FLOOR60_LIVE"); g_cgFloorLive = (fl && *fl && atoi(fl) != 0) ? 1 : 0; }
    const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
    const int di = (!g.paced && g.policy != kPolicyGame) ? NpFpsDisplayInterval() : 0;
    const bool live = masked == (unsigned)kTaskGameplay && (taskState & (kTaskPaused | kTaskHomeMenu)) == 0 && di > 0 && interval > 0.0;
    const bool megaNow = live && &VgxMegaActive != 0 && VgxMegaActive() != 0;
    const int liveRelief = (live && g.lock60 && g_cgFloorLive > 0 && !megaNow) ? 1 : 0;
    g_cgLiveRelief.store(liveRelief, std::memory_order_relaxed);
    if (!CrowdGovOn()) return;
    if (g_cgForce >= 0)   // forced / scripted (host proofs): the counters only
    {
        static unsigned long nf;
        if (++nf % 900 == 0) fprintf(stderr, "[crowdgov] forced level %d: sprites measured %lu, notch moves %lu, captures kept %lu skipped %lu (slow idle %lu), model size classes near/mid/far %lu/%lu/%lu, nearest-figure height histogram (<60 <64 <68 <72 <76 <80 <100 more) %lu %lu %lu %lu %lu %lu %lu %lu\n", NpCrowdGovLevel(), g_cgCount[0], g_cgCount[1], g_cgCount[2], g_cgCount[3], g_cgCount[4], g_cgCount[8], g_cgCount[9], g_cgCount[10], g_cgCount[12], g_cgCount[13], g_cgCount[14], g_cgCount[15], g_cgCount[16], g_cgCount[17], g_cgCount[18], g_cgCount[19]);
        return;
    }
    if (!live) { g_cgov.Idle(); g_cgAt60 = 0; return; }
    const int expected = di == 2 ? HeldPeriod() : di;
    const double busy = interval * 1000.0 - (double)g.waitNs * 1e-6;
    const double periodMs = g45.live ? g45.pat.PeriodRefreshes() * R * 1000.0 : (double)expected * R * 1000.0;   // (helper f45: the 45 budget, 22.2 ms)
    double lane = 0.0;
    for (int i = 0; i < 3; ++i) { const double v = g_lc.lane[i].Recent(); if (v > lane) lane = v; }
    // int33: with MSC_CROWD_FLOOR60_LIVE=1 in Smooth 60 live open play (not Mega Strikes, not Quality 30, not held scenes), keep crowd floor + amortization active across temporary 30-fps relief so GPU does not jump +4 ms and trap hold60 at 30.
    g_cgAt60 = ((periodMs < 20.0 && !g45.live) || liveRelief != 0) ? 1 : 0;
    const int before = g_cgov.Level();
    {   // why60 (MSC_CROWD_SEED60=1, default 0): the crowd level a 60 needs is remembered; a live picture at 60 that follows live pictures at another cadence (a trial of 60 after the relaxed 30 gave the crowd back to
        // level 0) starts at that level instead of walking down one level a second (a trial is judged after 0.5 s for 2 s). Render only (the crowd sprites' refresh interval).
        static int s_seed = -1, s_lvl60 = 0, s_was60 = -1;
        if (s_seed < 0) s_seed = EnvDouble("MSC_CROWD_SEED60", 0.0) != 0.0 ? 1 : 0;
        if (s_seed)
        {
            const bool at60 = periodMs < 20.0 && !g45.live;
            if (at60 && s_was60 == 0 && s_lvl60 > before && g_cgov.Seed(now, s_lvl60))
                fprintf(stderr, "[crowdgov] MSC_CROWD_SEED60: a 60 begins at level %d -> %d (the level the last 60 reached) at frame %llu\n", before, g_cgov.Level(), (unsigned long long)g.frames);
            if (at60 && g_cgov.Level() > s_lvl60) s_lvl60 = g_cgov.Level();
            s_was60 = at60 ? 1 : 0;
        }
    }
    if (g_cgov.Picture(now, busy > 0.0 ? busy : 0.0, periodMs, lane))
    {
        char line[700];
        snprintf(line, sizeof line, "[crowdgov] level %d -> %d at frame %llu: over budget %.0f %% of the window's pictures, game thread mean %.1f ms (period %.1f ms, lanes max %.1f ms)", before, g_cgov.Level(), (unsigned long long)g.frames,
                 100.0 * g_cgov.LastOverShare(), g_cgov.LastMean(), periodMs, lane);
        fprintf(stderr, "%s\n", line);
    }
    static unsigned long n;
    if (++n % 900 == 0)
    {
        char line[900];
        g_cgov.Line(line, sizeof line);
        fprintf(stderr, "%s; sprites measured %lu, notch moves %lu, captures kept %lu skipped %lu (slow idle %lu), model size classes near/mid/far %lu/%lu/%lu\n", line, g_cgCount[0], g_cgCount[1], g_cgCount[2], g_cgCount[3], g_cgCount[4], g_cgCount[8], g_cgCount[9], g_cgCount[10]);
    }
}

// wave 17 (MSC_HEAD_STAT=1, default 0, logging only; np_head17.h): the picture that just ended goes into the window of its scene class; every 30 s one "[head]" line per class. Reads only what this
// file already has (frame length, waits, steps, policy): no clock read, no game state, no random number.
static void Head17Picture(double now, double interval, double R, unsigned taskState)
{
    if (g_headOn < 0)
    {
        const char* e = getenv("MSC_HEAD_STAT");
        g_headOn = (e && *e && atoi(e) > 0) ? 1 : 0;
        if (g_headOn)
        {
            g_head.Init();
            if (!g_head.Ready()) g_headOn = 0;
            else fprintf(stderr, "[head] on (MSC_HEAD_STAT=1, logging only): every 30 s one line per scene class (play, replay, walk-out, celebration, mega, mega-cut, cut-scene, other; held classes count 'over' against 33.4 ms): game-thread ms a picture (frame length less waits) p50/p95/max for pictures with and without a simulation step, render-thread and GPU p95 (the lanes of MSC_LATE_CAUSE, which must stay on), late pictures, relief, pictures over 16.7 ms by game / render / GPU\n");
        }
        if (!g_headOn) return;
    }
    static double t0 = 0.0;
    static unsigned long sw0 = 0;
    if (t0 == 0.0) { t0 = now; sw0 = (unsigned long)g.switches; }
    const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
    // scenes30: the moment of a held picture (walk-out, celebration, Mega Strike and its cut-scenes, replay) from the presentation's function; np_head17.h MomentOf
    int moment = 0;
    if ((taskState & kTaskNis) && &NpPresentationFunction != 0)
    {
        const char* fn = NpPresentationFunction();
        if (fn)
        {
            moment = !strncmp(fn, "Megastrike", 10) ? 3 : !strncmp(fn, "GameBegin", 9) ? 1 : !strncmp(fn, "Goal", 4) ? 2 : 0;
            static char seen[24][24];
            static int nSeen = 0;
            int k = 0;
            while (k < nSeen && strncmp(seen[k], fn, sizeof seen[k] - 1) != 0) ++k;
            if (k == nSeen && nSeen < 24)   // (each presentation function once: the names the console's moments come from)
            {
                snprintf(seen[nSeen++], sizeof seen[0], "%s", fn);
                fprintf(stderr, "[head] presentation function \"%s\" in a cut-scene: class %s\n", fn, nphead::ClassName(nphead::MomentOf(false, false, true, false, false, moment)));
            }
        }
    }
    const bool megaNow = &VgxMegaActive != 0 && VgxMegaActive() != 0;
    const int cls = nphead::MomentOf((taskState & (kTaskPaused | kTaskHomeMenu)) != 0, (taskState & (kTaskReplay | kTaskDebugReplay)) != 0, (taskState & kTaskNis) != 0, masked == (unsigned)kTaskGameplay, megaNow, moment);
    const int di = (!g.paced && g.policy != kPolicyGame) ? NpFpsDisplayInterval() : 0;
    const int expected = di == 2 ? HeldPeriod() : (di > 0 ? di : 1);
    const int refreshes = interval > 0.0 ? (int)(interval / R + 0.5) : 0;
    const double busy = interval * 1000.0 - (double)g.waitNs * 1e-6;
    const bool kicked = g.simWallMsAcc > 0.0;   // pace18: the picture's step ran on np_sim (MSC_SIMT=1|2): its wall ms and the join wait go into the line
    // heldcpu24: the frame-send join's wait is inside `busy` (waitNs holds only the swap, late and pad waits): in the held scenes of the Vice logs (s53) it was 12-17 ms of a 40-44 ms "game ms"
    static double s_fsJoinSeen = -1.0;
    float fsJoinMs = -1.0f;
    if (&NpFsJoinWaitedSeconds != 0)
    {
        const double jw = NpFsJoinWaitedSeconds();
        fsJoinMs = (s_fsJoinSeen >= 0.0 && jw > s_fsJoinSeen) ? (float)((jw - s_fsJoinSeen) * 1000.0) : 0.0f;
        s_fsJoinSeen = jw;
    }
    g_head.Picture(cls, (float)(busy > 0 ? busy : 0), g.simSteps, refreshes > expected, g.policy == kPolicy30, kicked ? (float)g.simWallMsAcc : -1.0f, kicked ? (float)g.simJoinMsAcc : -1.0f, fsJoinMs);
    if (now - t0 >= nphead::kWindowS)
    {
        g_head.Pull();
        char line[1300];
        for (int c = 0; c < nphead::kClasses; ++c)
            if (g_head.Line(c, line, sizeof line, g.lock60, g.policy == kPolicy60 ? 60 : (g.policy == kPolicy30 ? 30 : 0), (int)((unsigned long)g.switches - sw0), now - t0)) fprintf(stderr, "%s\n", line);
        t0 = now;
        sw0 = (unsigned long)g.switches;
    }
}

// wave 18 (MSC_FPS_PROBE_EVERY=N, default 1): may this picture go without reading the probe? Only in auto, with a cost probe, N > 1. What the probe feeds: the per-core
// busy ms of the picture just finished (Auto::Predict: the 60 Hz cost, the free-run cost, "busiest core stayed over the budget"), counted per picture by the controller's rules and
// by the free-run gate through g.lastPredNow / lastPredFree. It feeds nothing else: not the simulation, the pictures, the pad or what a LAN peer is sent. A stale reading can only
// move a policy change by up to N-1 pictures, and every case where a late reading could matter reads at once: a net/LAN/recorded game or one not made yet (always every picture), no
// valid window yet, a scene change, a locked cut-scene frame, a policy or free-run change on the picture before, a picture whose length differs from the one before by more than half
// a refresh (a miss, a stall, a cadence change), or N pictures since the last read. Counts as a picture of the window either way.
// ---- helper f45: the frame's rate, its game time and the [f45] line (game thread) ---------------------------------------------------------------------------------------------------
static void F45Line(unsigned taskState)
{
    uint32_t d[kD45Count];
    for (int i = 0; i < kD45Count; ++i) { const uint32_t v = g45d[i].load(std::memory_order_relaxed); d[i] = v - g45.d0[i]; g45.d0[i] = v; }
    const uint32_t worst = g45ErrWorst.exchange(0, std::memory_order_relaxed);
    ++g45.lines;
    const npfps::Fallback45& f = g45.fall;
    char state[96];
    if (f.At45()) snprintf(state, sizeof state, "45");
    else if (f.Latched()) snprintf(state, sizeof state, "30, latched for this match");
    else snprintf(state, sizeof state, "30, held until t=%.0f s (+ 12 s proof)", f.HoldUntil());
    fprintf(stderr, "[f45] %u pictures (made at 45 %u, at 30: held scenes / menus %u, fallback %u, other %u); shown at 45 %u: held 1/2/3+ refreshes %u/%u/%u, pattern slips %u "
            "(late %u, held to a later predicted slot %u), off the pattern %u, phase taken %u (long stalls %u); interpolation error: %u of %u pictures off their show vblank, mean %.3f, worst %u refreshes; "
            "game time from the show slots %u frames, own time %u; L %d refreshes; lead %d refreshes (%.0f ms start to picture; raised %u, lowered %u, failed lowerings %u, late at the top %u; room: %u of %u judged); "
            "gate: %u frames on their slot's lead, %u on the count (cap %d unshown), %u waited (%u refreshes), %u cut at 8; fallback: now %s, %u fallbacks, %u returns, window %u/%u and %u/%u late; "
            "fallback proof %u fit / %u not (first input over: game %u, GPU %u, lanes %u); task state 0x%X\n",
            g45.win, g45.w45, g45.wHeld, g45.wFall, g45.wOther, d[kD45Shown], d[kD45H1], d[kD45H2], d[kD45H3], d[kD45Slips], d[kD45Late], d[kD45Planned], d[kD45Off], d[kD45Resyncs], d[kD45Stalls],
            d[kD45ErrOff], d[kD45ErrN], d[kD45ErrN] ? (double)d[kD45ErrAbs] / (double)d[kD45ErrN] : 0.0, worst, g45.wPred, g45.wOwn, g45L.load(std::memory_order_relaxed), g45.lead.Lead(), g45.lead.Lead() * 1001.0 / 60.0,
            g45.lead.raises_, g45.lead.lowers_, g45.lead.failed_, g45.lead.atMax_, d[kD45Roomy], d[kD45Judged], g45.wGateSlot, g45.wGateCount, g45.depth + 1, g45.wBpWait,
            g45.wBpRefresh, g45.wGateCap, state, f.Fallbacks(), f.Returns(), f.WindowLate(), f.WindowN(), f.SlowLate(), f.SlowN(), g45.wFit, g45.wNoFit, g45.wNoFitGame, g45.wNoFitGpu, g45.wNoFitLane, taskState);
    g45.wNoFitGame = g45.wNoFitGpu = g45.wNoFitLane = 0;
    g45.win = g45.w45 = g45.wHeld = g45.wFall = g45.wOther = g45.wPred = g45.wOwn = g45.wFit = g45.wNoFit = g45.wBpWait = g45.wBpRefresh = g45.wGateSlot = g45.wGateCount = g45.wGateCap = 0;
}

// ---- pace13 MSC_TIER_MEMORY: <data dir>/np-tier-memory.txt (the directory that holds NAND/, as np-camera-zoom.txt; MSC_TIER_MEMORY_FILE overrides the whole path for tests) ---------------------
#if __has_include("np/np_sdk.h")
#include "np/np_sdk.h"
extern "C" const NpSdkConfig* NpSdk_GetConfig(void) __attribute__((weak));
#define NP_FPS_HAVE_SDK_CFG 1
#endif
static const char* TierMemPath()
{
    static char path[1100];
    if (const char* f = getenv("MSC_TIER_MEMORY_FILE")) { if (*f) return f; }
    const char* dir = nullptr;
#ifdef NP_FPS_HAVE_SDK_CFG
    if (NpSdk_GetConfig) dir = NpSdk_GetConfig()->data_dir;
#endif
    if (!dir || !*dir) dir = getenv("NP_DATA_DIR");
    if (!dir || !*dir) dir = ".";
    snprintf(path, sizeof path, "%s/np-tier-memory.txt", dir);
    return path;
}
static void TierMemLoad()
{
    g45.memLoaded = true;
    char buf[4096];
    size_t n = 0;
    if (FILE* f = fopen(TierMemPath(), "rb")) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    g45.mem.Parse(buf);
    fprintf(stderr, "[tier] memory (MSC_TIER_MEMORY=1): %d stadium(s) remembered from %s; a match starts at the tier its stadium settled at (hold %.0f s, then the proof for the way up)\n", g45.mem.Count(), TierMemPath(), g45.memHold);
}
static void TierMemSave()
{
    char buf[4096];
    const int len = g45.mem.Format(buf, sizeof buf);
    if (len <= 0 || (size_t)len >= sizeof buf) return;
    if (FILE* f = fopen(TierMemPath(), "wb")) { fwrite(buf, 1, (size_t)len, f); fclose(f); ++g45.memSaves; }
}
// the first live picture of a match: start at the remembered tier (Auto::SeedRelief / Fallback45::SeedFallback: a relief with its hold and proof, never a latch nearer)
static void TierMemTick(bool inMatch, bool single, double now)
{
    if (!g45.memOn) return;
    if (!g45.memLoaded) TierMemLoad();
    if (inMatch && !g45.memIn) { g45.memIn = true; g45.memSeeded = false; g45.memStadium = -1; for (int i = 0; i < 3; ++i) g45.memStart[i] = g45.ts.secs[i]; }
    if (!g45.memIn || g45.memSeeded || !single || !g.ctl) return;
    const int st = &VgxHoldStadium != 0 ? VgxHoldStadium() : -1;
    if (st < 0) return;
    g45.memSeeded = true;
    g45.memStadium = st;
    const int t = g45.mem.Get(npfps::kTierMemSmooth60, st);
    if (!t) { fprintf(stderr, "[tier] memory: stadium %d: nothing remembered, the match starts at 60\n", st); return; }
    g.ctl->SeedRelief(now, g45.memHold);
    if (t == 30) g45.fall.SeedFallback(now, g45.memHold);
    ++g45.memSeeds;
    fprintf(stderr, "[tier] memory: stadium %d settled at %d last time: the match starts there (hold %.0f s, then the proof: 12 s that fit for the way up; seeds so far %u)\n", st, t, g45.memHold, g45.memSeeds);
}
// the match is over: the tier with the most live play time is what the stadium settled at (a 60 clears the entry)
static void TierMemMatchEnd()
{
    if (!g45.memOn || !g45.memIn) return;
    g45.memIn = false;
    if (g45.memStadium < 0) return;
    double d[3];
    for (int i = 0; i < 3; ++i) d[i] = g45.ts.secs[i] - g45.memStart[i];
    const bool changed = g45.mem.Settled(npfps::kTierMemSmooth60, g45.memStadium, d[0], d[1], d[2]);
    fprintf(stderr, "[tier] memory: match over at stadium %d: %.0f s at 60 / %.0f at 45 / %.0f at 30 of live play: %s\n", g45.memStadium, d[0], d[1], d[2],
            d[0] + d[1] + d[2] < npfps::TierMem::kMinPlaySecs ? "too little play, nothing stored" : (changed ? "stored" : "unchanged"));
    if (changed) TierMemSave();
}

// Decides whether this frame is made at 45, feeds the fallback with the display's verdicts (and, at the fallback 30, with the estimated cost at 45 of each live picture), and returns the
// game time of the frame in seconds from the show clock (-1: its own time). realSeconds and the g.* costs are the previous frame's (the frame that just ended).

// wave 24 p98: the pictures around every transition (tier or picture size), the transition hitches, and the repeat clusters per scene class (0 play, 1 cut-in: the first 6 s after a scene change, 2 the 3 pictures after a transition)
static void P98Transition(const char* what, double now, int kind, double R)
{
    for (int i = 0; i < 3; ++i) g45.p98Before[i] = g45.p98Prev[i];   // flip25: the pictures BEFORE the change (the line used to print Prev at its end, which by then held the first pictures after it)
    g45.p98Pend = 3; g45.p98Kind = kind; g45.p98HitchedThis = false; g45.p98At = now; ++g45.p98Trans; if (kind == 1) ++g45.p98TransTier; else ++g45.p98TransSize;
    (void)what; (void)R;
}
static void P98Picture(double rs, double R, bool tierChanged, int fromTier, int toTier, double now, unsigned taskState)
{
    const double ms = rs * 1000.0;
    ++g45.p98Pics;
    if (taskState != g45.p98Scene) { g45.p98Scene = taskState; g45.p98SceneAt = now; }
    const int cls = now - g45.p98SceneAt < 6.0 ? 1 : 0;
    const double thr = toTier == 0 ? 1.5 * R * 1000.0 : 2.5 * R * 1000.0;   // (60: a repeat; 45 / 30: a picture over its slot: the 45's longest hold is 2 refreshes, the 30's too)
    const bool slipped = ms > thr;
    if (g45.p98Pend > 0)
    {   // the pictures after a transition
        g45.p98After[3 - g45.p98Pend] = ms;
        if (slipped) g45.p98HitchedThis = true;
        if (slipped && toTier == 0) g45.p98Rep[2] += (unsigned)(ms / (R * 1000.0) + 0.5) - 1u;
        if (--g45.p98Pend == 0)
        {
            if (g45.p98HitchedThis) ++g45.p98TransHitch;
            if (g45.p98TransTier + g45.p98TransSize <= 120)
                fprintf(stderr, "[p98] transition %u (%s) at t=%.3f s: pictures before %.1f %.1f %.1f ms, after %.1f %.1f %.1f ms (limit %.1f ms): %s\n", g45.p98Trans, g45.p98Kind == 1 ? "tier" : "picture size", g45.p98At,
                        g45.p98Before[0], g45.p98Before[1], g45.p98Before[2], g45.p98After[0], g45.p98After[1], g45.p98After[2], thr, g45.p98HitchedThis ? "TRANSITION HITCH" : "seamless");
        }
    }
    if (toTier == 0 && ms > 1.5 * R * 1000.0)
    {   // a repeated refresh at 60: counted by scene class; two in a row or three within 0.5 s is a cluster
        const unsigned reps = (unsigned)(ms / (R * 1000.0) + 0.5) - 1u;
        g45.p98Rep[cls] += reps;
        for (int i = 0; i < 3; ++i) g45.p98RepT[i] = g45.p98RepT[i + 1];
        g45.p98RepT[3] = now;
        if (reps >= 2 || now - g45.p98RepT[1] < 0.5) ++g45.p98Hitch[cls];
    }
    ++g45.p98Pict[cls];
    g45.p98Prev[0] = g45.p98Prev[1]; g45.p98Prev[1] = g45.p98Prev[2]; g45.p98Prev[2] = ms;
    if (tierChanged) P98Transition("tier", now, 1, R);
    (void)fromTier;
}
static inline bool F45Allowed() { return npfps::MidAllowed(g.mode == kMode45, g45.use45, g45.use50); }   // p98: the 45 is a tier only with MSC_LOCK60_USE45=1 (or MSC_FPS=45)
static double F45Frame(float realSeconds, unsigned taskState, uint32_t vc, int haveVc)
{
    if (g.mode != kMode45 && !g45.tiers) return -1.0;
    const double now = NowSeconds();
    const double R = Refresh();
    const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
    const bool inMatch = (masked & (unsigned)(kTaskGameplay | kTaskReplay | kTaskNis | kTaskDebugReplay)) != 0;
    const bool lan = &NpLanActive != 0 && NpLanActive();
    const bool play = taskState == (unsigned)kTaskGameplay;   // (live play exactly: no pause or home-menu bit, no replay or cut-scene)
    // (f50, strict 50 only) the newest game slot of the tier before, for the first frame of the next one: a tier change shows its first picture at its display time (npfps::Clock45::Seed)
    uint32_t seedG = 0, seedStamp = g45.stamp;
    bool seed60 = false;
    if (g45.tiers)
    {
        // Fable (np_fps_tier.h): the tier of this picture and the [tier] line. The frame that just ended was at the tier decided a frame ago (g45.live / live60 / neither).
        const bool single = play && !lan && !NpFpsNetOrRecorded();
        TierMemTick(inMatch, single, now);   // (pace13, off by default)
        if (g45.hold)
        {   // hold60: the lanes' means, one newest value per new lane picture
            const uint32_t nf = g_lc.lane[0].Count(), nb = g_lc.lane[1].Count();
            if (nf != g45.feSeen) { const double v = g_lc.lane[0].Last(); g45.feMean = g45.feSeen ? g45.feMean + (v - g45.feMean) / 16.0 : v; g45.feSeen = nf; }
            if (nb != g45.beSeen) { const double v = g_lc.lane[1].Last(); g45.beMean = g45.beSeen ? g45.beMean + (v - g45.beMean) / 16.0 : v; g45.beSeen = nb; }
        }
        if (single && realSeconds > 0.0f)
        {
            const npfps::Tier was = g45.live60 || g.policy == kPolicy60 ? npfps::kTier60 : (g45.live ? npfps::kTier45 : npfps::kTier30);
            const npfps::Tier before = (npfps::Tier)(g45.ts.last < 0 ? (int)was : g45.ts.last);
            const bool p98Chg = g45.ts.Picture(was, (double)realSeconds, was == npfps::kTier60 && (double)realSeconds > 1.5 * R);
            if (g45.p98 || g45.use50) P98Picture((double)realSeconds, R, p98Chg, (int)before == npfps::kTier60 ? 0 : 1, was == npfps::kTier60 ? 0 : 1, now, taskState);
            if ((g45.p98 || g45.use50) && &VgxDynResGrace != 0) { const bool gr = VgxDynResGrace() != 0; if (gr && !g45.p98GraceWas) P98Transition("size", now, 2, R); g45.p98GraceWas = gr; }
            if (p98Chg && g45.ts.Changes() <= 200)
                fprintf(stderr, "[tier] %s -> %s at t=%.3f s, frame %llu (change %u)\n", before == npfps::kTier60 ? "60" : (before == npfps::kTier45 ? g45.midName : "30"),
                        was == npfps::kTier60 ? "60" : (was == npfps::kTier45 ? g45.midName : "30"), now, (unsigned long long)g.frames, g45.ts.Changes());
            if (g45.ts.pictures % 300u == 0u)
            {
                uint32_t d[kD60Count];
                for (int i = 0; i < kD60Count; ++i) { const uint32_t v = g60d[i].load(std::memory_order_relaxed); d[i] = i < 4 ? v - g45.seen60[i] : v; if (i < 4) g45.seen60[i] = v; }
                const npfps::TierStats& t = g45.ts;
                fprintf(stderr, "[tier] 300 live pictures: time at 60 / %s / 30 %.1f / %.1f / %.1f s (all so far %.0f / %.0f / %.0f s); tier changes so far %u (60->mid %u, mid->30 %u, 30->mid %u, mid->60 %u, "
                        "60<->30 %u); repeated refreshes at 60: %u in this window, %u so far, %u of them tolerated (60 was kept); 60 with the show-time clock: %u pictures shown, %u ready after their "
                        "vblank, %u of %u off the vblank their game time stands for; L %d refreshes; fallback of the 45: %s\n",
                        g45.midName, t.winSecs[0], t.winSecs[1], t.winSecs[2], t.secs[0], t.secs[1], t.secs[2], t.Changes(), t.changes[0][1], t.changes[1][2], t.changes[2][1], t.changes[1][0],
                        t.changes[0][2] + t.changes[2][0], t.winMisses60, t.misses60, t.Tolerated(), d[kD60Shown], d[kD60Late], d[kD60ErrOff], d[kD60ErrN], g60L.load(std::memory_order_relaxed),
                        g45.fall.At45() ? "at 45" : (g45.fall.Latched() ? "30, latched 240 s" : "30, hold + proof"));
                if (g45.use50)
                {   // f50: the strict 50 tier: its time, the display's holds and pattern slips since the last line, the transitions (the same counter as [p98])
                    uint32_t h[4], pics = g45d[kD45Shown].load(std::memory_order_relaxed), sl = g45d[kD45Slips].load(std::memory_order_relaxed);
                    h[1] = g45d[kD45H1].load(std::memory_order_relaxed); h[2] = g45d[kD45H2].load(std::memory_order_relaxed); h[3] = g45d[kD45H3].load(std::memory_order_relaxed);
                    const uint32_t dp = pics - g45.f50Pics0, ds = sl - g45.f50Slips0;
                    fprintf(stderr, "[f50] strict 50 (5 pictures in 6 refreshes, 1-1-1-1-2): time at 60 / 50 / 30 %.1f / %.1f / %.1f s in this window (all so far %.0f / %.0f / %.0f s); %u pictures shown at 50 in this window, "
                            "pattern slips %u (%.2f %%), held 1 / 2 / 3+ refreshes %u / %u / %u; off the pattern %u; tier changes so far %u (60->50 %u, 50->30 %u, 30->50 %u, 50->60 %u, 60<->30 %u); "
                            "transitions %u (tier %u, size %u), TRANSITION HITCHES %u; fallback of the 50: %s; fall limit: more than %.0f %% of 90 or %.0f %% of 360 pictures off their slot\n",
                            t.winSecs[0], t.winSecs[1], t.winSecs[2], t.secs[0], t.secs[1], t.secs[2], dp, ds, dp ? 100.0 * ds / dp : 0.0, h[1] - g45.f50H0[1], h[2] - g45.f50H0[2], h[3] - g45.f50H0[3],
                            g45d[kD45Off].load(std::memory_order_relaxed), t.Changes(), t.changes[0][1], t.changes[1][2], t.changes[2][1], t.changes[1][0], t.changes[0][2] + t.changes[2][0],
                            g45.p98Trans, g45.p98TransTier, g45.p98TransSize, g45.p98TransHitch, g45.fall.At45() ? "at 50" : (g45.fall.Latched() ? "30, latched" : "30, hold + proof"), g45.hold ? 15.0 : 8.0, g45.hold ? 5.0 : 4.0);
                    g45.f50Pics0 = pics; g45.f50Slips0 = sl; g45.f50H0[1] = h[1]; g45.f50H0[2] = h[2]; g45.f50H0[3] = h[3];
                }
                if (g.ctl && (g.ctl->DwellHeld() || g.ctl->Relapses() || g45.fall.Relapses()))   // pace13 counters (only with MSC_TIER_DWELL / MSC_TIER_RELAPSE)
                    fprintf(stderr, "[tier] flap rules: %u pictures' fall verdicts held by the dwell; relapses counted twice: %u at 60, %u at 45\n", g.ctl->DwellHeld(), g.ctl->Relapses(), g45.fall.Relapses());
                if (g45.hold && g.ctl)
                {
                    fprintf(stderr, "[hold60] delivered at 60 now %.1f a second over 2 s, %.1f over 5 s; climb cost of the last picture %.1f ms (fits %.1f), proof %d pictures with %.1f over; wait after the last fall %.0f s, "
                            "failed tries %u; the 30's proof for 45: %u pictures, %u not fitting\n", g.ctl->HoldRate(now, 2.0), g.ctl->HoldRate(now, 5.0), g.ctl->HoldUpCost(), 0.97 * R * 1000.0,
                            g.ctl->HoldProofN(), g.ctl->HoldProofOver(), g.ctl->HoldWait(), g.ctl->HoldFails(), g45.fall.ProofN(), g45.fall.ProofBad());
                    // hold60b: the climb cost's inputs over the pictures judged since the last line, and what the 60 measured meanwhile (the check of the estimate on the console)
                    const npfps::Auto::HoldParts hp = g.ctl->HoldTakeParts();
                    const npfps::Auto::At60 a6 = g.ctl->HoldTakeAt60();
                    const double n1 = hp.n > 0 ? (double)hp.n : 1.0, m1 = a6.n > 0 ? (double)a6.n : 1.0;
                    fprintf(stderr, "[hold60] climb cost parts, %d pictures judged: game thread %.1f ms a picture (less waits, the 60's step; of it the blend lane's wait %.1f), front end %.1f, back end %.1f, "
                            "GPU at the floor %.1f (max %.1f); over %.1f (%.0f %%; by game %d, of them only by the lane's wait %d, front end %d, back end %d, GPU %d) | at 60, %d pictures: work %.1f ms a picture "
                            "(of it the lane's wait %.1f), %d over %.1f ms, %d held 2+ refreshes; catch-up spread (MSC_LOCK60_SPREAD) %u pictures at one step while owing, %u late pictures\n",
                            hp.n, hp.game / n1, hp.lane / n1, hp.fe / n1, hp.be / n1, hp.gpu / n1, hp.gpuMax, hp.over, 100.0 * hp.over / n1, hp.by[0] + hp.by[1], hp.by[1], hp.by[2], hp.by[3], hp.by[4],
                            a6.n, a6.work / m1, a6.lane / m1, a6.over, 0.97 * R * 1000.0, a6.late, g45.spreadPics, g45.spreadFrom);
                    g45.spreadPics = g45.spreadFrom = 0;
                    {   // pace24 (FIX-02's check): what the climb cost's clock mix could hold, per picture since the last line: wall ms lost inside the steps beyond their CPU ms, the frame-send join's wait
                        static double s_joinSeen = -1.0;
                        const double jw = &NpFsJoinWaitedSeconds != 0 ? NpFsJoinWaitedSeconds() : 0.0;
                        const double jms = s_joinSeen >= 0.0 && jw > s_joinSeen ? (jw - s_joinSeen) * 1000.0 : 0.0;
                        s_joinSeen = jw;
                        const double nP = g45.stallN > 0 ? (double)g45.stallN : 1.0;
                        fprintf(stderr, "[hold60] clock check, %u pictures: wall ms lost inside the steps beyond their CPU ms %.2f a picture (in the climb cost's base unless MSC_HOLD60_WALLSIM=1), the frame-send join's wait %.2f a picture "
                                "(real time, kept in the cost), the blend lane's wait %.1f (kept)\n", g45.stallN, g45.stallInSteps / nP, jms / nP, hp.lane / n1);
                        g45.stallInSteps = 0.0; g45.stallN = 0;
                    }
                }
                if (g45.p98 && g.ctl)
                {
                    const npfps::Auto::P98Stats ps = g.ctl->P98Take(now);
                    fprintf(stderr, "[p98] %u live pictures: on slot %.2f %% over the last %d s (repeats %.2f %%, visible hitches %d); 60->45 trials %u kept %u failed %u, GPU-bound cuts skipped %u, backoff %.0f s%s; "
                            "30->45 trials %u kept %u failed %u backoff %.0f s; falls %u; repeats by class: play %u, cut-in %u, 3 pictures after a transition %u; hitch clusters play %u, cut-in %u; pictures play %u, cut-in %u; "
                            "transitions %u (tier %u, size %u), transition hitches %u\n", g45.p98Pics, ps.onSlot, ps.window, ps.repeatPct, ps.hitches, ps.trials, ps.ok, ps.fail, g.ctl->P98GpuSkips(), ps.backoff, ps.trialOn ? " (a trial is on)" : "",
                            g45.fall.Trials(), g45.fall.TrialsOk(), g45.fall.TrialsFailed(), g45.fall.TrialBackoff(), ps.falls, g45.p98Rep[0], g45.p98Rep[1], g45.p98Rep[2], g45.p98Hitch[0], g45.p98Hitch[1],
                            g45.p98Pict[0], g45.p98Pict[1], g45.p98Trans, g45.p98TransTier, g45.p98TransSize, g45.p98TransHitch);
                    g45.p98Rep[0] = g45.p98Rep[1] = g45.p98Hitch[0] = g45.p98Hitch[1] = g45.p98Pict[0] = g45.p98Pict[1] = 0;   // (per window)
                }
                g45.ts.Window();
                ++g45.tierLines;
            }
        }
        if (g.policy == kPolicy60)
        {
            // the 60 tier: 60's own pacing; only the game time is the show clock's (and the display holds the picture to the vblank it stands for)
            g45.fall.Pause();
            g45.lead.Pause();
            const bool seed50 = g45.use50 && g45.live && g45.clock.NewestTarget(&seedG);   // (f50) the 50's newest frame, for the 60's first
            g45.live = false;
            g45Live.store(0, std::memory_order_relaxed);
            g45.clock.Begin(false, 0, 0, false, 0, 0, -1);
            const bool display = &NpLatHandedOverTag != 0 && &NpLatShownTag != 0;
            g45.live60 = g45.clock60On && single && g_bootFast != 1 && !g_loadFast && display && haveVc;
            if (!g45.live60) { g45.clock60.Begin(false, 0, 0, false, 0, 0, -1); return -1.0; }
            g45.stamp = vc;
            const uint64_t fb = g45Fb.load(std::memory_order_acquire);
            const uint32_t hi = (uint32_t)(fb >> 32);
            const bool fb60 = fb != 0 && (hi & 7u) == npfps::Pattern45::kEvery;
            int L = g60L.load(std::memory_order_relaxed);
#if defined(__vita__)
            if (L < 0) L = 3;
#else
            if (L < 0) L = 0;
#endif
            if (seed50 && g45.clock60.Empty()) g45.clock60.Seed(seedStamp, seedG, (int)npfps::Pattern45::kEvery);
            const int adv = g45.clock60.Begin(true, vc, L, fb60, (uint64_t)(hi >> 3), (uint32_t)(fb & 0xFFFFFFFFu), fb60 ? (int)npfps::Pattern45::kEvery : -1);
            return adv < 0 ? -1.0 : (double)adv * R;
        }
        seed60 = g45.use50 && g45.live60 && g45.clock60.NewestTarget(&seedG);   // (f50) the 60's newest frame, for the 50's first
        g45.live60 = false;
        g45.clock60.Begin(false, 0, 0, false, 0, 0, -1);
        // the controller's relief: in live play it is the 45 below; while the 45 is itself on ITS relief (the 30), 60 is not tried (30 -> 45 -> 60)
        if (single && F45Allowed() && !g45.fall.At45() && g.ctl) g.ctl->DeferUp(now, 2.0);
    }
    const bool mega = &VgxMegaActive != 0 && VgxMegaActive() != 0;
    // the display's verdicts on the 45 pictures shown since the last frame (a size change's own late pictures are not held against the rate: VgxDynResGrace)
    const uint32_t sh = g45d[kD45Shown].load(std::memory_order_relaxed), sl = g45d[kD45Slips].load(std::memory_order_relaxed);
    const uint32_t dn = sh - g45.seenShown, dl = sl - g45.seenSlips;
    g45.seenShown = sh;
    g45.seenSlips = sl;
    const bool grace = &VgxDynResGrace != 0 && VgxDynResGrace() != 0;
    {   // (Fable) the lead: the display's evidence on the same pictures
        const uint32_t la = g45d[kD45Late].load(std::memory_order_relaxed), ju = g45d[kD45Judged].load(std::memory_order_relaxed), ro = g45d[kD45Roomy].load(std::memory_order_relaxed);
        const uint32_t dLate = la - g45.seenLate, dJudged = ju - g45.seenJudged, dRoomy = ro - g45.seenRoomy;
        if (dn) { g45.seenLate = la; g45.seenJudged = ju; g45.seenRoomy = ro; }   // (a count that ran ahead of the shown count is taken with its picture, on a later frame)
        for (uint32_t i = 0; i < dn && i < 16; ++i)
        {
            npfps::Lead45Verdict v;
            v.late = !grace && i < dLate;
            v.judged = !grace && !v.late && i < dJudged;
            v.roomy = v.judged && i < dRoomy;
            const int was = g45.lead.Lead();
            const int ch = g45.lead.Picture(v);
            if (ch) fprintf(stderr, "[f45] lead %d -> %d refreshes at t=%.3f s, frame %llu (%s); raised %u, lowered %u, lowerings that failed %u\n", was, g45.lead.Lead(), now, (unsigned long long)g.frames,
                            ch > 0 ? "two pictures came ready after their slot" : "8 s of the heaviest pictures ready more than a refresh + 6 ms early", g45.lead.raises_, g45.lead.lowers_, g45.lead.failed_);
        }
    }
    uint32_t dlFb = dl, dHold = 0;
    if (g45.lateOnly)
    {   // pace24: the true late pictures (the display's late count: ready after the slot it was planned for) and the refreshes the pictures were held, not every pattern slip
        const uint32_t lt = g45d[kD45Late].load(std::memory_order_relaxed), hs = g45d[kD45HoldSum].load(std::memory_order_relaxed);
        dlFb = lt - g45.seenLateFb;
        dHold = hs - g45.seenHoldSum;
        if (dn) { g45.seenLateFb = lt; g45.seenHoldSum = hs; }
    }
    for (uint32_t i = 0; i < dn && i < 16; ++i)
    {
        unsigned hold = 0;
        if (g45.lateOnly) hold = grace ? 1u : (dn ? dHold / dn + (i < dHold % dn ? 1u : 0u) : 1u);
        const bool p98Fell = F45Allowed() && g45.fall.Picture45(!grace && i < (g45.lateOnly ? dlFb : dl), now, hold);
        if (g45.fall.LastTrialOk()) fprintf(stderr, "[p98] 45 trial kept at t=%.3f s (trials %u kept %u failed %u)\n", now, g45.fall.Trials(), g45.fall.TrialsOk(), g45.fall.TrialsFailed());
        if (p98Fell && g45.fall.LastTrialFail()) fprintf(stderr, "[p98] 45 trial failed at t=%.3f s: back to 30, next trial in %.0f s (trials %u kept %u failed %u)\n", now, g45.fall.TrialBackoff() / 2.0 > 4.0 ? g45.fall.TrialBackoff() / 2.0 : 4.0, g45.fall.Trials(), g45.fall.TrialsOk(), g45.fall.TrialsFailed());
        else if (p98Fell)
            fprintf(stderr, "[f45] FALLBACK to the locked 30 at full size at t=%.3f s, frame %llu: %s (%u of 90, %u of %u); held at least %.0f s%s\n", now, (unsigned long long)g.frames,
                    g45.fall.LastRate() ? "the pictures were delivered under the rate the 45 needs (MSC_F45_LATEONLY)" : (g45.fall.LastSlow() ? "more than 4 % of the last 360 pictures were off their slot" : "more than 8 % of the last 90 pictures were off their slot"), g45.fall.WindowLate(),
                    g45.fall.SlowLate(), g45.fall.SlowN(), g45.fall.LastWait(), g45.fall.Latched() ? ", LATCHED for this match (third fallback)" : ", then 12 s of proof");
    }
    // the fallback 30 in live play: the proof that 45 would fit (the game thread's picture with 45's 1.11 steps instead of the steps it ran, and the GPU at the ladder's floor)
    if (F45Allowed() && play && !lan && !g45.fall.At45() && realSeconds > 0.0f)
    {
        if (g.simSteps > 0 && g.simMsAcc > 0.0) { const double st = g.simMsAcc / (double)g.simSteps; if (st < 40.0) g45.stepMs += (st - g45.stepMs) / 16.0; }
        const double busy = (double)realSeconds * 1000.0 - (double)g.waitNs * 1e-6;
        const double est = (busy - g.simMsAcc) + g45.stepMs * (50.0 * (double)g45.pat.m / (double)g45.pat.n * R);
        const double gpu = &VgxGpuMinMs != 0 ? (double)VgxGpuMinMs() : 0.0;
        double lanes = 0.0;
        { const double fe = g_lc.lane[0].Recent(), be = g_lc.lane[1].Recent(); lanes = fe > be ? fe : be; }
        if (g45.hold) lanes = g45.feMean > g45.beMean ? g45.feMean : g45.beMean;   // hold60: a pipelined lane by its mean, not its largest of the last three   // (the front end and back end lanes when [late] is on; the GPU lane is at the 30's full size)
        const bool fits = busy > 0.0 && est <= g45.fitMs && (gpu <= 0.0 || gpu <= g45.fitMs) && (lanes <= 0.0 || lanes <= g45.fitMs);
        if (fits) ++g45.wFit; else ++g45.wNoFit;
        if (!fits) { if (!(busy > 0.0 && est <= g45.fitMs)) ++g45.wNoFitGame; else if (!(gpu <= 0.0 || gpu <= g45.fitMs)) ++g45.wNoFitGpu; else ++g45.wNoFitLane; }   // (hold60 diagnosis: which input)
        const bool hwOk = (gpu <= 0.0 || gpu <= g45.fitMs) && (lanes <= 0.0 || lanes <= g45.fitMs);
        const unsigned relaxedBefore = g45.fall.Relaxed();
        const double proofMean = g45.fall.ProofMean();
        const bool p98Back = g45.fall.Picture30(fits, now, busy > 0.0 ? est : -1.0, hwOk);
        if (p98Back && g45.fall.LastTrialStart()) fprintf(stderr, "[p98] 30 -> 45 trial %u at t=%.3f s, frame %llu (the wait / the cut is over; the GPU and lanes fit): kept at 98 %% of the pictures on their slot over 2 s\n", g45.fall.Trials(), now, (unsigned long long)g.frames);
        else if (p98Back)
            fprintf(stderr, "[f45] back to 45 at t=%.3f s, frame %llu: held %.0f s and 12 s of pictures that fit (the last: %.1f ms at 45, GPU at the floor %.1f ms)%s\n", now, (unsigned long long)g.frames,
                    g45.fall.LastWait(), est, gpu, g45.fall.Relaxed() != relaxedBefore ? " [MSC_F45_RETRY: the proof was the mean cost of the last 120 pictures, not each picture]" : "");
        if (g45.retry && ++g45.retryPics % 300 == 0) fprintf(stderr, "[f45] retry: at the fallback 30 now; mean estimated cost at 45 over the last proof pictures %.1f ms (retry at most %.1f ms), %u returns by the mean proof so far\n", proofMean, g45.fall.RelaxMean(), g45.fall.Relaxed());
    }
    if (!inMatch) { if (++g45.matchOut == 120) { TierMemMatchEnd(); g45.fall.MatchEnd(); g45.lead.MatchEnd(); if (g45.p98 && g.ctl) g.ctl->P98NewMatch(); } }   // (p98: a new match starts the trial backoff again) }
    else g45.matchOut = 0;
    if (!play) { g45.fall.Pause(); g45.lead.Pause(); }
    const bool display = &NpLatHandedOverTag != 0 && &NpLatShownTag != 0;
    const bool live = npfps::Tier45Live(play && !lan && !NpFpsNetOrRecorded() && !mega && g_bootFast != 1 && !g_loadFast && display && haveVc, F45Allowed(), g45.fall.At45());
    g45.live = live;
    g45Live.store(live ? (g45.use50 ? 2 : 1) : 0, std::memory_order_relaxed);
    ++g45.win;
    if (live) ++g45.w45; else if (play && F45Allowed() && !g45.fall.At45()) ++g45.wFall; else if (inMatch && !play) ++g45.wHeld; else ++g45.wOther;
    if (g45.win >= 300) F45Line(taskState);
    if (!live) { g45.clock.Begin(false, 0, 0, false, 0, 0, -1); return -1.0; }
    g45.stamp = vc;
    const uint64_t fb = g45Fb.load(std::memory_order_acquire);
    int L = g45L.load(std::memory_order_relaxed);
#if defined(__vita__)
    if (L < 0) L = 3;
#else
    if (L < 0) L = 0;   // (host pacing: the picture is "shown" on the slot after its own frame)
#endif
    const uint32_t hi = (uint32_t)(fb >> 32);
    const bool fb45 = fb != 0 && (hi & 7u) < npfps::Pattern45::kEvery;   // (a report of a 60-tier picture is not this clock's)
    if (seed60 && g45.clock.Empty()) g45.clock.Seed(seedStamp, seedG);
    const int adv = g45.clock.Begin(true, vc, L, fb45, (uint64_t)(hi >> 3), (uint32_t)(fb & 0xFFFFFFFFu), fb45 ? (int)(hi & 7u) : -1);
    if (adv < 0) { ++g45.wOwn; return -1.0; }
    ++g45.wPred;
    return (double)adv * R;
}

static bool ProbeSkipThisPicture(unsigned taskState, double interval, double R)
{
    if (g.probeEvery <= 1) return false;
    ++g.probeAge;
    const bool net = (&NpLanActive != 0 && NpLanActive()) || NpFpsNetOrRecorded();
    const double d = interval - g.probeIv;
    g.probeIv = interval;
    if (NP_PROBE_EVERY_BREAK == 2) return g.probeAge < g.probeEvery;   // (mutant: no forcing rule at all)
    const bool force = net || !g.haveProbe || !g.probeCacheOk || g.probeForceNext || g.lockedThisFrame || taskState != g.lastScene || (d < 0 ? -d : d) > 0.5 * R
        || g.probeAge >= g.probeEvery || NP_PROBE_EVERY_BREAK == 1;
    g.probeForceNext = false;
    return !force;
}

void NpFpsBeginFrame(float realSeconds, unsigned taskState)
{
    Init();
    {   // wave 9 (MSC_SMOOTH_CLOCK, np_smooth.cpp): under a held cadence in live single-player play the frame advances by its picture's predicted show-to-show time
        uint32_t vc = 0;
        int haveVc = 0;
        if (&sceDisplayGetVcount != 0) { vc = (uint32_t)sceDisplayGetVcount(); haveVc = 1; }
        else if (&NpVI_GetRetraceCount != 0) { vc = NpVI_GetRetraceCount(); haveVc = 1; }
        const double s45 = F45Frame(realSeconds, taskState, vc, haveVc);   // helper f45 (MSC_FPS=45): decides g45.live; at 45 the game time is the 45 clock's, never the smooth clock's
        const int interval = (!g.paced && g.policy != kPolicyGame && !g45.live && !g45.live60) ? NpFpsDisplayInterval() : 0;
        g_smBasis = &NpSmoothFrameBegin != 0 ? NpSmoothFrameBegin(realSeconds, interval, haveVc, vc, taskState == (unsigned)kTaskGameplay ? 1 : 0) : -1.0;
        if (g45.live || (g45.live60 && s45 >= 0.0)) g_smBasis = s45;   // (60 tier: the frames the show clock has no slot for keep the smooth clock's / their own time)
    }
    SlipFrame(realSeconds, taskState);   // wave 9 (MSC_SLIP_BUDGET_MS): this frame's slip, decided once for every task of the frame
    SpeedFrame(taskState);               // wave 9 (MSC_SPEED_LOG): game speed against real time, from the steps the simulation ran
    if (&sceDisplayGetVcount != 0 && g.presentSync != 0)
    {
        g.turnVcount = (uint32_t)sceDisplayGetVcount();
        g.haveTurn = true;
    }
    const double now = NowSeconds();
    const double R = Refresh();
    ++g.frames;
    if (g.frames == 1)
    {
        g.firstNow = now;
        g.lastScene = taskState;
        g.waitNs = 0;
        g.simMsAcc = 0;
        g.simWallAccMs = 0;
        g.simSteps = 0;
        g.simWinMsAcc = g.simJoinMsAcc = g.simWallMsAcc = 0;
        g.lockedThisFrame = false;
        if (g.probe)
        {
            NpFpsProbeOut o;
            memset(&o, 0, sizeof o);
            NP_GS(NPGS_BF_PROBE);
            if (g.probe(&o)) { g.probePrev = o; g.haveProbe = true; }
        }
        return;   // no previous frame yet
    }
    const double interval = realSeconds > 0.0f ? (double)realSeconds : 0.0;
    if (PairControlled())
    {
        // wave 10 (bg30b): the pair controller (np_pair_rate.h) hears the picture just finished: its phase (np_bg30), its game-thread ms (frame length less the waits), its GPU ms by phase
        // (the renderer), how many refreshes it took. live: live play only, never LAN. It answers whether the pair cadence is wanted from the next picture.
        const int phase = &NpBg30Phase != 0 ? NpBg30Phase() : 0;
        const double gameMs = interval * 1000.0 - (double)g.waitNs * 1e-6;
        const float gpuMs = (phase != 0 && &VgxBg30GpuMs != 0) ? VgxBg30GpuMs(phase) : 0.0f;
        const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
        const bool live = masked == (unsigned)kTaskGameplay && !(&NpLanActive != 0 && NpLanActive());
        const int refreshes = interval > 0.0 ? (int)(interval / R + 0.5) : 0;
        const int want = NpFpsPairReport(phase, gameMs > 0.0 ? (float)gameMs : 0.0f, gpuMs, refreshes, live ? 1 : 0, now);
        static int last = 0;
        if (want != last)
        {
            last = want;
            fprintf(stderr, "[pair] frame %llu: the pair cadence is %s (phase of the last picture %d, game %.1f ms, GPU %.1f ms)\n", (unsigned long long)g.frames, want ? "WANTED" : "left for the even 30", phase, gameMs, (double)gpuMs);
        }
        // lessons17 (Galaxy's Game Over strip): the pair cadence (a picture every refresh) must not run on a pause or home-menu screen. Without MSC_PAIR_SCREENS=1 it does (the controller keeps `live` over a pause so that a pause does not
        // reset what it learned): the count of such pictures is logged either way. With it the DISPLAY interval falls to the even 30 while the pause bits are set; the controller's own state is untouched.
        static const bool screens = [] { const char* e = getenv("MSC_PAIR_SCREENS"); return e && *e && atoi(e) > 0; }();
        static unsigned long pausedPair = 0;
        const bool menuBits = (taskState & (kTaskPaused | kTaskHomeMenu)) != 0;
        if (want && live && menuBits)
        {
            ++pausedPair;
            if (pausedPair == 1 || pausedPair % 600 == 0)
                fprintf(stderr, "[pair] %lu pictures so far were at the pair cadence on a pause / home-menu screen (task state 0x%X): %s (MSC_PAIR_SCREENS=%d)\n", pausedPair, taskState,
                        screens ? "the display interval was held at the even 30 there" : "no light picture is drawn there (bg30 needs the gameplay state) but every refresh gets a picture", screens ? 1 : 0);
        }
        g_pairLive.store(want && live && !(screens && menuBits) ? 1 : 0, std::memory_order_relaxed);
    }
    if (LateCauseOn() && interval > 0.0)
    {
        const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
        const int di = (!g.paced && g.policy != kPolicyGame) ? NpFpsDisplayInterval() : 0;
        if (masked == (unsigned)kTaskGameplay && (taskState & (kTaskPaused | kTaskHomeMenu)) == 0 && di > 0)
        {
            const int expected = di == 2 ? HeldPeriod() : di;
            const double waitMs = (double)g.waitNs * 1e-6;
            const double ownMs = (double)g_padSleepUs * 1e-3;
            if (g_lc.Picture((int)(interval / R + 0.5), expected, interval * 1000.0 - waitMs, waitMs > ownMs ? waitMs - ownMs : 0.0, interval * 1000.0))
            {
                double us = -1.0;
                unsigned long reads = 0;
                if (&VgxLateCostTake != 0 && !VgxLateCostTake(&us, &reads)) us = -1.0;
                char line[700];
                size_t ll = g_lc.Line(line, sizeof line, us, reads);
                { unsigned dn = 0, o8 = 0, o33 = 0; float mean = 0, mx = 0;   // diag13 (MSC_DQ_TIMING): the render thread's display-queue call this window, on the same line
                  if (&vgx_host_dq_window != 0 && ll < sizeof line && vgx_host_dq_window(1, &dn, &mean, &mx, &o8, &o33))
                      snprintf(line + ll, sizeof line - ll, " | display queue %u swaps mean %.2f max %.1f ms, %u at 8 ms or more, %u at 33 ms or more", dn, mean, mx, o8, o33); }
                fprintf(stderr, "%s\n", line);
            }
        }
    }
    g_padSleepUs = 0;   // (the next frame's sleep, if any, is set after this point)
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
    CrowdGovPicture(now, interval, R, taskState);   // crowd25 (MSC_CROWD_GOV)
    if (g_headOn != 0) Head17Picture(now, interval, R, taskState);   // wave 17 (MSC_HEAD_STAT=1): the headroom line

    if (&VgxWarmScene != 0)
    {
        // wave 7 (MSC_DL_WARM): the scene class at its cut, in every mode (what the front end learns of a replay and a celebration)
        static unsigned s_lastMasked = 0xFFFFFFFFu;
        const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
        if (masked != s_lastMasked)
        {
            s_lastMasked = masked;
            VgxWarmScene((taskState & (kTaskReplay | kTaskDebugReplay)) ? 2 : ((taskState & kTaskNis) ? 3 : (masked == (unsigned)kTaskGameplay ? 1 : 0)));
        }
    }
    // (only AUTO: in 30 FPS mode the picture size stays as 1.1 had it; REPLAYS & CUT-SCENES says "no effect with 30 FPS")
    const bool replayClass = (taskState & (kTaskReplay | kTaskNis | kTaskDebugReplay)) != 0;
    g_bpHeldNow = replayClass && BpHeldOn();
    if (&VgxPacedSceneHint != 0 && g.mode == kModeAuto) VgxPacedSceneHint((replayClass && g.replayMode != 2) || g.cadMenuHeld ? 1 : 0);
    if (g.mode == kModeAuto)
    {
        npfps::FrameIn in;
        memset(&in, 0, sizeof in);
        in.now = now;
        in.interval = interval;
        // (1.2) the scene is the class of the task state without the pause bits: pausing (or the home menu) must not reset the retry back-off
        // or the hold of a replay
        in.scene = g.sceneNoPause ? (taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu)) : taskState;
        in.locked = g.lockedThisFrame;
        in.steps = g.simSteps;
        in.simMs = g.simMsAcc;
        in.simMode = SimModeNow();   // pace18: 0 unless MSC_SIMT 1|2 is in force
        in.windowMs = g.simWinMsAcc;
        in.joinMs = g.simJoinMsAcc;
        in.simWallMs = g.simWallMsAcc;
        in.gameCore = 0;
        in.gpuMinMs = (&VgxGpuMinMs != 0) ? (double)VgxGpuMinMs() : 0.0;
        in.dynGrace = &VgxDynResGrace != 0 && VgxDynResGrace() != 0;
        in.p98 = g45.p98 && g45.hold && taskState == (unsigned)kTaskGameplay && !(&NpLanActive != 0 && NpLanActive()) && !NpFpsNetOrRecorded();   // (LAN / online / recorded: the controller as before)
        in.ladderRoom = &VgxDynResRoom != 0 && VgxDynResRoom() != 0;
        if (g45.hold)
        {   // hold60: the picture's measured work for the climb (the frame length less the counted waits, as [head]; the renderer's lanes by their means, when MSC_LATE_CAUSE feeds them)
            const double busy = interval * 1000.0 - (double)g.waitNs * 1e-6;
            in.workMs = busy > 0.0 ? busy * g.costScale : 0.0;
            in.stepWallMs = g.simWallAccMs * g.costScale;   // pace24 (Config::holdWallSim): the steps in the frame's own clock
            g45.stallInSteps += g.simWallAccMs > g.simMsAcc ? g.simWallAccMs - g.simMsAcc : 0.0; ++g45.stallN;
            in.feMs = g45.feMean;
            in.beMs = g45.beMean;
            // hold60b: the part of that the game thread was blocked on the helper lane (the async snapshot blend's join): kept in the climb cost, shown apart in [hold60]
            static double s_laneSeen = -1.0;
            const double lw = &NpLaneWaitedSeconds != 0 ? NpLaneWaitedSeconds() : 0.0;
            double laneMs = s_laneSeen >= 0.0 && lw > s_laneSeen ? (lw - s_laneSeen) * 1000.0 * g.costScale : 0.0;
            s_laneSeen = lw;
            in.laneWaitMs = laneMs < in.workMs ? laneMs : in.workMs;
        }
        if (g_model.on)
        {
            in.costsValid = g.haveModel;
            in.steps = g.modelSteps;
            in.simMs = g.modelSimMs;
            in.simMode = 0;
            for (int c = 0; c < npfps::kMaxCores; ++c) { in.cpuMs[c] = g.modelCpu[c]; in.timeMs[c] = g.modelTime[c]; }
        }
        else if (g.probe && ProbeSkipThisPicture(taskState, interval, R))
        {
            // wave 18 (MSC_FPS_PROBE_EVERY > 1): no read this picture. The controller hears the per-picture mean of the last window (the frame counters of its rules step as before);
            // ProbeSkipThisPicture lists every case that forces a real read.
            in.costsValid = true;
            in.gameCore = g.probeAvgCore;
            for (int c = 0; c < npfps::kMaxCores; ++c)
            {
                in.cpuMs[c] = g.probeAvgCpu[c] * g.costScale;
                in.timeMs[c] = g.probeAvgTime[c] * g.costScale;
            }
            in.simMs *= g.costScale;
            in.windowMs *= g.costScale;
            ++g.winProbeOk;
            ++g.probeSkips;
            for (int c = 0; c < npfps::kMaxCores; ++c) g.winCpu[c] += in.cpuMs[c];
        }
        else if (g.probe)
        {
            NpFpsProbeOut o;
            memset(&o, 0, sizeof o);
            bool sampled;
            { NP_GS(NPGS_BF_PROBE); sampled = g.probe(&o) != 0; }   // gt27 (MSC_GSPLIT)
            const int window = g.probeEvery > 1 ? (g.probeAge > 1 ? g.probeAge : 1) : 1;   // pictures this read covers (1 = today)
            const bool regular = g.probeEvery > 1 && g.probeAge >= g.probeEvery;
            if (g.probeEvery > 1) { g.probeAge = 0; g.probeCacheOk = false; ++g.probeReads; }
            if (sampled && g.haveProbe)
            {
                in.costsValid = true;
                in.gameCore = o.gameCore;
                for (int c = 0; c < npfps::kMaxCores; ++c)
                {
                    double dc = o.cpuMs[c] - g.probePrev.cpuMs[c];
                    double dt = o.timeMs[c] - g.probePrev.timeMs[c];
                    if (dc < 0) dc = 0;
                    if (dt < 0) dt = 0;
                    if (window > 1)
                    {
                        // the window's mean is what the cache keeps; a read forced before the window is full (a long picture, a scene cut, a policy change) hears the
                        // picture just finished as what the window holds beyond the cached mean of the pictures before it (so a spike is not diluted by the skipped ones)
                        const double meanC = dc / window, meanT = dt / window;
                        double useC = meanC, useT = meanT;
                        if (!regular && g.probeCacheOk)
                        {
                            useC = dc - g.probeAvgCpu[c] * (window - 1);
                            useT = dt - g.probeAvgTime[c] * (window - 1);
                            if (useC < meanC) useC = meanC;   // (never less than the mean: a wrong cache must not hide a spike)
                            if (useT < meanT) useT = meanT;
                        }
                        g.probeAvgCpu[c] = meanC;
                        g.probeAvgTime[c] = meanT;
                        dc = useC;
                        dt = useT;
                    }
                    else if (g.probeEvery > 1) { g.probeAvgCpu[c] = dc; g.probeAvgTime[c] = dt; }
                    in.cpuMs[c] = dc * g.costScale;
                    in.timeMs[c] = dt * g.costScale;
                }
                g.probeAvgCore = o.gameCore;
                in.simMs *= g.costScale;
                in.windowMs *= g.costScale;
                g.probePrev = o;
                g.haveProbe = true;
                // a probe that reports no CPU time at all for a whole frame is not delivering (a real frame costs something somewhere):
                // fall back to the game thread's own busy time rather than read "free" as "can hold 60"
                double total = 0;
                for (int c = 0; c < npfps::kMaxCores; ++c) total += in.cpuMs[c];
                if (total < 0.1) in.costsValid = false;
                if (g.probeEvery > 1) g.probeCacheOk = in.costsValid;
                if (in.costsValid)
                {
                    ++g.winProbeOk;
                    for (int c = 0; c < npfps::kMaxCores; ++c) g.winCpu[c] += in.cpuMs[c];
                }
                else ++g.winProbeZero;
            }
            else if (sampled)
            {
                g.probePrev = o;
                g.haveProbe = true;
            }
            else
            {
                g.haveProbe = false;
                ++g.winNoProbe;
            }
        }
        else if (!g_model.on) ++g.winNoProbe;
        g.lastCostsValid = in.costsValid;
        if (in.simMode != 0 && &NpSimtCoreMask != 0)
        {   // pace18: where np_sim sits (one bit a core; 0 = not known: treated as the game core, the default of MSC_SIMT=1)
            const unsigned sm = (unsigned)NpSimtCoreMask();
            in.simOnGameCore = sm == 0 || (in.gameCore >= 0 && in.gameCore < 4 && sm == (1u << in.gameCore));
            in.simCore = (sm != 0 && (sm & (sm - 1)) == 0) ? (int)__builtin_ctz(sm) : -1;
        }
        if (!in.costsValid)
        {
            const double busy = interval * 1000.0 - (double)g.waitNs * 1e-6;
            in.gameBusyMs = (busy > 0 ? busy : 0) * g.costScale;
            in.simMs *= 1.0;
        }
        // 1.3 MSC_BG30=2 (np/bg30.h): the per-picture cost by kind; held at 30 in live play that pairs could serve, the cost a pair would have
        if (&NpBg30NoteCost != 0) NpBg30NoteCost(in.gameBusyMs - in.simMs);
        if (&NpBg30HeldEligible != 0 && NpBg30HeldEligible())
        {
            const double save = NpBg30PairSavingMs();
            in.gameBusyMs = in.gameBusyMs > save ? in.gameBusyMs - save : 0.0;
            ++g.bg30Discounted;
        }
        // held30 (23 Nov): under SMOOTH 60 with the hold a Mega Strike is held at 30 from its first picture (it fell there after the 2 s fast window before), and live
        // play gets its 60 back at once when the Mega Strike ends (below); MSC_LOCK60_HELD30=0 or MSC_FPS_MEGA=auto: the controller decides, as before
        static const bool megaHeld30 = [] { const char* e = getenv("MSC_LOCK60_HELD30"); const char* m = getenv("MSC_FPS_MEGA"); return g.lock60 && g45.hold && !(e && *e && atoi(e) == 0) && !(m && IEqual(m, "auto")); }();
        static const bool megaAuto = [] { const char* e = getenv("MSC_FPS_MEGA"); return (e && IEqual(e, "auto")) || (g.lock60 && !megaHeld30); }();
        const bool holdMega = !megaAuto && &VgxMegaActive != 0 && VgxMegaActive() != 0;
        static bool sMegaWasHeld = false;
        const bool megaEnded = megaHeld30 && sMegaWasHeld && !holdMega && (taskState & kTaskGameplay) != 0;
        sMegaWasHeld = holdMega;
        // 1.2 (STR-010): replays, goal celebrations and cut-scenes (task states 8, 16, 131072) were shown at an uneven 20-36 fps under auto. MSC_FPS_REPLAY:
        // fit (default) = the controller decides, but a replay that did not hold a tried 60 stays at 30 until it ends, with the renderer free to go
        // deeper in picture size to keep the 30 even; 30 = always 30 there (like a Mega Strike); auto = as in 1.1.
        in.sceneClass = (replayClass && g.replayMode != 2) ? 1 : 0;
        if (g.lock60) in.sceneClass = (taskState & kTaskGameplay) ? 0 : 1;   // LOCKED 60: a relief outside the match proper lasts until that scene ends (np_fps_ctl.h Lock60)
        const bool holdReplay = replayClass && g.replayMode == 1;
        // wave 7 (MSC_CADENCE, np_fps_cadence.h): the scene class decides at its cut. Open play is never held by it; a replay and a cut-scene start held at a locked 30 unless the class
        // has earned 60 over visits; the cut back to open play puts the earlier policy back at once; a goal makes the controller wait for its cut.
        npfps::CadenceOut co;
        if (g.cad)
        {
            npfps::CadenceIn ci;
            memset(&ci, 0, sizeof ci);
            const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
            ci.now = now;
            ci.cls = npfps::ClassOfTaskState(taskState, g.cadMenus);   // (menus: MSC_CADENCE_MENUS)
            ci.key = 0;
            ci.scene = masked;
            ci.refreshMs = R * 1000.0;
            ci.intervalMs = interval * 1000.0;
            ci.enabled = g.replayMode == 0 && !(&NpLanActive != 0 && NpLanActive());
            ci.policy = g.policy == kPolicy30 ? 1 : (g.policy == kPolicy60 ? 2 : 0);
            ci.paced = g.paced;
            if (ci.cls == npfps::kCadReplay || ci.cls == npfps::kCadCut || ci.cls == npfps::kCadMenu)
            {
                // (the prediction of this frame: the controller is not asked while a scene is held, so ask for it here; a frame it does not judge needs none)
                const npfps::Prediction pr = g.ctl->Predict(in);
                ci.valid = pr.valid;
                ci.costMs = pr.max60 > in.gpuMinMs ? pr.max60 : in.gpuMinMs;
            }
            co = g.cad->Frame(ci);
            if (co.cut)
            {
                ++g.cadCuts;
                if (++g.cadLogged <= 80)
                    fprintf(stderr, "[cadence] cut at t=%.3f s, frame %llu: task state 0x%X (class %d) -> %s%s%s; policy %s%s\n", now, (unsigned long long)g.frames, taskState, ci.cls,
                            co.hold30 ? "held at 30" : "left to the controller", co.go60 ? ", released class: 60 from the cut" : "",
                            co.resume ? (co.resumePolicy == 2 ? ", open play resumes at 60" : (co.resumePaced ? ", open play resumes free-running" : ", open play resumes at 30")) : "",
                            kPolicyName[g.policy], g.paced ? " (paced)" : "");
            }
            if (co.resume) { ++g.cadResumes; g.cadPacedBack = co.resumePaced; }
            else g.cadPacedBack = false;
            if (co.go60) ++g.cadGo60;
        }
        npfps::Decision d;
        if (holdMega) d = g.ctl->Hold30(now);
        else if (megaEnded) d = g.ctl->Resume(kPolicy60, now, "LOCKED 60: the Mega Strike is over, live play is back at 60 (MSC_LOCK60_HELD30)");
        else if (holdReplay) d = g.ctl->Hold30(now, "a replay, celebration or cut-scene stays at 30 (MSC_FPS_REPLAY)");
        else if (co.resume) d = g.ctl->Resume(co.resumePolicy == 2 ? kPolicy60 : kPolicy30, now, "MSC_CADENCE: open play is back, the policy of before the scene is put back at once");
        else if (co.go60) d = g.ctl->Resume(kPolicy60, now, "MSC_CADENCE: this class has earned 60: from its cut");
        else if (co.hold30) { d = g.ctl->Hold30(now, "MSC_CADENCE: a replay / cut-scene is held at a locked 30 from its cut"); ++g.cadHoldFrames; }
        else if (co.deferDown) { d.policy = g.policy; d.switched = false; d.reason = ""; ++g.cadDefers; }   // (a goal waits for its cut: no decision)
        else d = g.ctl->Frame(in);
        g.cadMenuHeld = g.cadMenus && co.hold30 && !replayClass && !holdMega;
        // bit 0: the cadence holds this scene at 30 (MSC_CADENCE_FULL acts on it); bit 1: MSC_CADENCE_FULL is on; bit 2: the scene is held at 30 by any rule (cadence or MSC_FPS_REPLAY),
        // which the FULL picture hold reads whatever MSC_CADENCE_FULL says: such a scene is drawn at the held size from its cut and no size is rehearsed in it (vgx_backend_gl.cpp HoldTick)
        if (&VgxCadenceFull != 0) VgxCadenceFull((g.cadFull && co.hold30 && !holdMega ? 1 : 0) | (g.cadFull ? 2 : 0) | ((co.hold30 || holdReplay) && !holdMega ? 4 : 0));
        g.lastPred60 = g.ctl->LastPrediction().max60;
        g.lastPredNow = g.ctl->LastPrediction().maxNow;
        g.lastPredFree = g.ctl->LastPrediction().maxFree;
        g.lastPredCore = g.ctl->LastPrediction().core;
        if (d.switched)
        {
            ++g.switches;
            g.probeForceNext = true;
            fprintf(stderr, "[fps] auto: %s -> %s at t=%.3f s, frame %llu, task state 0x%X (%s; predicted 60 Hz cost of the busiest core %.1f ms (core %d), measured %.1f ms, "
                    "refresh %.1f ms; penalty %d, cool-down until t=%.1f)\n",
                    kPolicyName[g.policy], kPolicyName[d.policy], now, (unsigned long long)g.frames, taskState, d.reason, g.lastPred60, g.lastPredCore, g.lastPredNow, R * 1000.0,
                    g.ctl->Penalty(), g.ctl->CooldownUntil());
            if (g45.p98 && g.ctl)
            {
                const npfps::Auto::P98Stats ps = g.ctl->P98Take(now);
                fprintf(stderr, "[p98] %s at t=%.3f s, frame %llu: %s (on slot %.2f %% over %d s, hitches %d; 60 trials %u kept %u failed %u, backoff %.0f s)\n", d.policy == kPolicy60 ? "return to 60" : "fall from 60", now,
                        (unsigned long long)g.frames, d.reason, ps.onSlot, ps.window, ps.hitches, ps.trials, ps.ok, ps.fail, ps.backoff);
            }
            g.policy = d.policy;
        }
        // wave 11 (cadence16, MSC_HELD_CADENCE=auto, np_fps_cad3.h): in a held scene that cannot make 33 ms, hold a steady 3 refreshes. Presentation only; the period is read by the late wait
        // and the back-pressure gate below. Inert (period 2) in play, menus, loads, LAN/online, a Mega Strike, a locked cut-scene, a free run, and without the late wait.
        if (g.cad3)
        {
            npfps::Cad3In c3;
            memset(&c3, 0, sizeof c3);
            const unsigned masked3 = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
            c3.enabled = replayClass && g.policy == kPolicy30 && !g.paced && !holdMega && !g.lockedThisFrame && g_bootFast != 1 && !g_loadFast && LateOn() && !(&NpLanActive != 0 && NpLanActive()) && !NpFpsNetOrRecorded() && !(taskState & (unsigned)(kTaskPaused | kTaskHomeMenu));   // (Fable verify5: a pause menu over a held scene is not paced at 20)
            c3.visit = masked3 + 0x9E3779B1u * (unsigned)g.cadCuts;
            c3.cls = (taskState & (kTaskReplay | kTaskDebugReplay)) ? 0 : 1;
            c3.pictureMs = interval * 1000.0;
            {
                const double busy = interval * 1000.0 - (double)g.waitNs * 1e-6;   // (the game thread's own time; the GPU's is the renderer's lower bound for its smallest picture)
                double w = busy > g.lastPredNow ? busy : g.lastPredNow;
                if (in.gpuMinMs > w) w = in.gpuMinMs;
                { const double lane = HeldLaneMaxMs(); if (lane > w) w = lane; }   // the front end, back end and GPU lanes ([late]'s NpLateLaneNote values): without them 59 ms pictures read as 15 ms of work
                c3.workMs = w;
            }
            const npfps::Cad3Out o3 = g.cad3->Frame(c3);
            if (o3.event && ++g.cad3Logged <= 60)
                fprintf(stderr, "[cad3] t=%.3f s, frame %llu, task state 0x%X: %s (period %d refreshes; picture %.1f ms, work %.1f ms%s)\n", now, (unsigned long long)g.frames, taskState,
                        o3.event == 1 ? "more than a third of the last 20 pictures missed 33 ms: steady 3 refreshes" : o3.event == 2 ? "work under 28 ms for 40 pictures: back to 2 refreshes" :
                        o3.event == 3 ? "this class ended its last visit at 3: steady 3 refreshes from the cut" : "held scene over: 2 refreshes", o3.period, c3.pictureMs, c3.workMs, o3.event == 1 ? ", misses counted in the window" : "");
            g.cad3Period = o3.period;
            if (o3.period == 3) ++g.cad3Frames;
        }
    }
    g_loadFast = LoadPace(taskState, &NpLanActive != 0 && NpLanActive() != 0);
    {
        unsigned ls = 0; double lms = 0.0; unsigned long long lp = 0;
        if (LoadClockStep(g_loadClock, taskState, now, ls, lms, lp))
            fprintf(stderr, "[load] task state 0x%X over: %llu pictures in %.0f ms (%.1f ms a picture); MSC_LOAD_FPS=%s (a picture per refresh in a load: %s)\n", ls, lp, lms, lp ? lms / (double)lp : 0.0,
                    g_loadMode > 0 ? "60" : "off", g_loadMode > 0 ? "on, solo only" : "off");
    }
    if (g.betweenMode)
    {
        // (MSC_FPS_BETWEEN=pace, see NpFpsPaced. &VgxMegaActive: the controller holds those at 30 itself, MSC_FPS_MEGA=auto included: a Mega Strike keeps its even picture)
        const bool mega = &VgxMegaActive != 0 && VgxMegaActive() != 0;
        const bool lan = &NpLanActive != 0 && NpLanActive();
        const bool eligible = g.mode == kModeAuto && g.policy == kPolicy30 && (taskState & kTaskGameplay) != 0 && !replayClass && !mega && !lan && g_bootFast != 1;
        const double gpuMin = (&VgxGpuMinMs != 0) ? (double)VgxGpuMinMs() : 0.0;
        // wave 10 (pacing11, MSC_FPS_BETWEEN_COST=free, the default; =measured is the wave 9 rule): the cost the busiest core would have AT the free-run rate, not the one measured at 30, where a picture carries
        // 1.67 simulation steps (Vice, console: 21.7 ms at 30 against 18.4 ms free: the gate judged 21.7 against its 20 ms and never opened)
        static const bool freeCost = [] { const char* e = getenv("MSC_FPS_BETWEEN_COST"); return !(e && IEqual(e, "measured")); }();
        const double coreMs = (freeCost && g.lastPredFree > 0.0) ? g.lastPredFree : g.lastPredNow;
        double busy = coreMs > gpuMin ? coreMs : gpuMin;
        if (NpFpsGameOverload()) busy = busy > 100.0 ? busy : 100.0;   // pacing12: a game thread that cannot run real time does not free-run (and leaves a free run)
        if (g.cadPacedBack && g.policy == kPolicy30) { g.between->Resume(now, true); g.cadPacedBack = false; }   // (MSC_CADENCE: the free run that was in force before the held scene)
        const bool want = g.between->Frame(now, taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu), eligible, g.lastCostsValid ? busy : 0.0, interval * 1000.0);
        if (want != g.paced)
        {
            g.paced = want;
            g.probeForceNext = true;
            LateForgetAnchor();
            if (++g.pacedChanges <= 40) fprintf(stderr, "[fps] between: %s at t=%.3f s, frame %llu, task state 0x%X (busiest stage %.1f ms judged %s: at the free-run rate %.1f, measured now %.1f, GPU at its smallest picture %.1f)\n", want ? "paced (free-running, up to 60)" : "held", now, (unsigned long long)g.frames, taskState, busy, freeCost ? "at the free-run rate" : "as measured", g.lastPredFree, g.lastPredNow, gpuMin);
        }
        if (g.paced) ++g.framesPaced;
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
        char probeText[260];
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
        if (g.mode == kModeAuto && g.probeEvery > 1 && !g_model.on)
        {
            const size_t l = strlen(probeText);
            snprintf(probeText + l, sizeof probeText - l, "; MSC_FPS_PROBE_EVERY=%d: read in %llu pictures, skipped in %llu", g.probeEvery, (unsigned long long)g.probeReads, (unsigned long long)g.probeSkips);
        }
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
    g.simWallAccMs = 0;
    g.simSteps = 0;
    g.simWinMsAcc = g.simJoinMsAcc = g.simWallMsAcc = 0;
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
int g_lateAnchorMode = -1;   // MSC_LAT_ANCHOR
bool LateAnchorBeforeGate()
{
    if (g_lateAnchorMode < 0)
    {
        const char* e = getenv("MSC_LAT_ANCHOR");
        g_lateAnchorMode = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return g_lateAnchorMode == 1;
}
void LateForgetAnchor()
{
    g_late.anchored = false;
    g_fifHaveCheck = false;
}
} // namespace

extern "C" {

int NpFpsLateWait(int swapMode, unsigned long refRetrace)
{
    // (a frame that does not take the 30 fps wait, a 60 fps frame or the game's own cut-scene lock, breaks the gate's entry-to-entry cadence record)
    if (swapMode != 4) g_fifHaveCheck = false;
    if (swapMode != 4 || !LateOn()) return 0;
    if (g_late.on == 1)
    {
        // two retraces after the previous frame's wait ended; refRetrace (two after the previous hand-over, the wait swap_Post makes) when the
        // previous frame made none
        g_late.sendOwed = true;
        g_late.sendRef = (g_late.anchored ? g_late.anchor + 2 : refRetrace) + (unsigned long)(HeldPeriod() - 2);   // (wave 11: 3 under MSC_HELD_CADENCE)
        g_late.anchored = false;
        return 1;
    }
    g_late.armNext = true;
    return 1;
}

// helper f45 / Fable: the gate before a 45 frame (and its pad read). The frame starts `lead` refreshes before the slot its picture will be planned for (Clock45::NextSlot: the slot after
// the newest frame's, on the display's newest report), so every picture has the same start-to-screen time and the start does not hang on which thread wakes first on a vblank (a count of
// pictures does: the display's report of a picture and VIWaitForRetrace's return come on the same vblank edge). The count of unshown pictures stays as a cap, and is the whole rule while no
// slot is known (the first frames of a stretch). The gate is the 45's pace (the swap does not wait), so MSC_LAT_BP=0 does not turn it off.
static bool F45Vcount(uint32_t* vc)
{
    if (&sceDisplayGetVcount != 0) { *vc = (uint32_t)sceDisplayGetVcount(); return true; }
    if (&NpVI_GetRetraceCount != 0) { *vc = NpVI_GetRetraceCount(); return true; }
    return false;
}
static void F45Gate()
{
    g_bpHaveEntry = false;   // (the 30 / 60 gate's entry-to-entry record does not span a 45 stretch)
    if (&NpLatHandedOverTag == 0 || &NpLatShownTag == 0 || &VIWaitForRetrace == 0) return;
    const uint64_t sent = NpLatHandedOverTag();
    int L = g45L.load(std::memory_order_relaxed);
#if defined(__vita__)
    if (L < 0) L = 3;
#else
    if (L < 0) L = 0;
#endif
    const uint32_t lead = (uint32_t)g45.lead.Lead();
    unsigned waited = 0;
    bool bySlot = false;
    for (;;)
    {
        const uint64_t shown = NpLatShownTag();
        if (shown == 0) break;   // (the display reports nothing: no gate)
        const uint64_t fb = g45Fb.load(std::memory_order_acquire);
        const uint32_t hi = (uint32_t)(fb >> 32);
        uint32_t slot = 0, vc = 0;
        const bool haveSlot = g45.clock.NextSlot(L, fb != 0 && (hi & 7u) < npfps::Pattern45::kEvery, (uint64_t)(hi >> 3), (uint32_t)(fb & 0xFFFFFFFFu), fb != 0 && (hi & 7u) < npfps::Pattern45::kEvery ? (int)(hi & 7u) : -1, &slot) && F45Vcount(&vc);
        const int32_t early = haveSlot ? (int32_t)(slot - lead - vc) : 0;   // refreshes before the frame's start vblank
        const bool timeWait = early > 0 && early <= 6;                      // (more is not a prediction to wait on)
        const bool countWait = sent > shown + (uint64_t)(g45.depth + (haveSlot ? 1 : 0));
        if (haveSlot) bySlot = true;
        if (!timeWait && !countWait) break;
        if (waited >= 8) { ++g45.wGateCap; break; }
        VIWaitForRetrace();
        ++waited;
    }
    if (bySlot) ++g45.wGateSlot; else ++g45.wGateCount;
    if (waited) { ++g45.wBpWait; g45.wBpRefresh += waited; }
}

// helper f45, host builds only (no display thread): the frame's picture is "shown" by the 45 gate on its slot, and the game thread waits for that vblank itself, so the 45 path (gate, clock,
// counters) runs in host proofs with the pattern's own timing. The Vita's display callback does this instead (vgx_host_vita.cpp GateDisplay).
static void F45HostPace()
{
#if !defined(__vita__)
    if (&VIGetRetraceCount == 0 || &VIWaitForRetrace == 0 || &NpLatHandedOverTag == 0) return;
    const uint64_t tag = NpLatHandedOverTag();
    const uint32_t earliest = (uint32_t)VIGetRetraceCount() + 1u;
    uint32_t due = 0;
    if (!NpFps45Due(tag, earliest, g45HostLast, g45HostHave ? 1 : 0, &due)) return;
    int guard = 0;
    while ((int32_t)((uint32_t)VIGetRetraceCount() - due) < 0 && guard++ < 12) VIWaitForRetrace();
    NpFps45Shown(tag, due);
    g45HostLast = due;
    g45HostHave = true;
#endif
}

void NpFpsLateWaitAfterSend(void)
{
    if (!g_late.sendOwed)
    {
        if (g.paced)
        {
            // MSC_FPS_BETWEEN=pace: no 30 fps wait here, but the frames on their way to the display stay as few as under the wait
            const int64_t t = NowNs();
            FramesInFlightGate();
            const int64_t w = NowNs() - t;
            if (w > 0) g.waitNs += w;
        }
        else if (!g.paced && g.policy != kPolicyGame)
        {
            // wave 11: a held cadence with no late wait of its own (60 fps, or MSC_LATE_WAIT=0): the back-pressure is still made before the next pad read
            const int64_t t = NowNs();
            if (g45.live) { F45HostPace(); F45Gate(); }   // helper f45: its own gate; a host build has no display thread: the game thread waits for its slot itself first
            else BackPressureGate();
            const int64_t w = NowNs() - t;
            if (w > 0 && g.policy != kPolicyGame) g.waitNs += w;
        }
        return;
    }
    g_late.sendOwed = false;
    const bool count = g.policy != kPolicyGame;
    const int64_t t0 = NowNs();
    while ((long)(VIGetRetraceCount() - g_late.sendRef) < 0) VIWaitForRetrace();
    // The next frame's pads are read after the gate. With MSC_LAT_ANCHOR=1 (default) its cadence is measured from the end of the late wait, BEFORE the
    // gate's own refresh: the gate shifts the pad read inside the two-refresh period instead of lengthening the period (an anchor taken after the
    // gate made the next send target slide by a refresh per gated frame: 24 / 20 fps sustained in tools/fps/fps_gate_unit.cpp). MSC_LAT_ANCHOR=0:
    // the 1.1 behaviour (anchor after the gate; STR-022's lateness back-off alone keeps the rate).
    const unsigned long preGate = VIGetRetraceCount();
    FramesInFlightGate();
    g_late.anchor = LateAnchorBeforeGate() ? preGate : VIGetRetraceCount();
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
    g_late.ref = (g_late.anchored ? g_late.anchor + 2 : refRetrace) + (unsigned long)(HeldPeriod() - 2);
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

} // extern "C"

// ---- wave 6: the late latch (MSC_PAD_LATE=1, default off; np_feel.h PadLate) ------------------------------------------------------------------------
// Under a held cadence (30, or 60) the pad is read at the start of the period, the frame is ready some time before its refresh, and the picture then
// waits for it twice: on the game thread (the late wait / the retrace wait after the send) and finished on the GPU (handed -> shown). Both waits are
// pure button delay. The step that reads the pad is put off by `delay` microseconds, which is the smaller of the two slacks seen with the previous
// delay in force minus a 3 ms margin (np_feel.h); a picture that comes up a period late sets it to zero for 120 pictures. The simulation's clock
// is not touched (the step's dt and the picture's blend were fixed at the frame's start; the sleep is added to the waits the controller counts, so
// AUTO does not take it for work). Single player only; free-running paces (MSC_FPS_BETWEEN=pace, interval 0) and LAN or online sessions never delay.
// Estimate, not measured: up to the slack, about 10 ms at 30 when the pipeline needs 20 ms of the 33 (docs/TECHNICAL.md "Pad to picture delay").
extern "C" int NpLatSlackTake(uint32_t* minSlackUs, uint32_t* maxGapUs) __attribute__((weak));
extern "C" void np_sleep_ns(uint64_t ns) __attribute__((weak));
extern "C" int NpLatSlackLbTake(uint32_t* minLbUs) __attribute__((weak));   // wave 11 (press16): np_latency.cpp
extern "C" uint64_t NpVitaCtrlStampUs(void) __attribute__((weak));   // wave 11 (press16): the stamp of the newest controller sample (np_vita_input.cpp); absent on the host
namespace {
// wave 11 (press16, np_lat16.h, MSC_LAT_SAMPLE=1, default off): sleep up to just after the next controller sample when the picture has the room, else read at once.
// Refused (no sleep, nothing learned from the slack) in LAN or online sessions, free runs and the game's own locked scenes, like the late latch.
void Lat16Wait()
{
    npl16::Latch& l = npl16::L();
    const int interval = NpFpsDisplayInterval();
    if ((&NpLanActive != 0 && NpLanActive()) || interval <= 0 || g.policy == kPolicyGame || g.paced) { l.lastWaitUs = 0; return; }
    const bool sim = npl16::Sim();
    uint32_t minSlack = 0xFFFFFFFFu, maxGap = 0;
    int n = NpLatSlackTake(&minSlack, &maxGap);
    if (&NpLatSlackLbTake != 0)   // the true room: from the callback's entry (the gate's own waits are idle time of a finished picture); the handed -> shown figure never exceeds one refresh
    {
        uint32_t lb = 0xFFFFFFFFu;
        if (NpLatSlackLbTake(&lb) > 0 && n > 0 && lb > minSlack) minSlack = lb;
    }
    if (sim) { n = 1; minSlack = (uint32_t)npl16::EnvInt("MSC_LAT_SAMPLE_SIM_SLACK_US", 20000); maxGap = 0; }
    const uint32_t period = (uint32_t)(interval == 2 ? HeldPeriod() : interval) * 16683u;   // (Fable verify5: 3 refreshes under MSC_HELD_CADENCE, else the late verdict flaps at a 50 ms gap)
    const bool late = n > 0 && maxGap > period + period / 2;
    const int64_t prevUs = g.waitNsPrev / 1000;
    l.Observe(n > 0, minSlack, (uint32_t)(prevUs > 0 ? (prevUs > 1000000 ? 1000000 : prevUs) : 0), late);
    const uint32_t w = npl16::DoWait(l,
        [] { return (uint64_t)NowNs() / 1000u; },
        [sim]() -> uint64_t { return sim ? npl16::SimStampUs((uint64_t)NowNs() / 1000u) : (&NpVitaCtrlStampUs != 0 ? NpVitaCtrlStampUs() : 0u); },
        [](uint32_t us) { NP_GS(NPGS_WAIT_PADLATE); if (&np_sleep_ns != 0) np_sleep_ns((uint64_t)us * 1000u); else { timespec tv; tv.tv_sec = 0; tv.tv_nsec = (long)us * 1000L; nanosleep(&tv, 0); } });
    if (w) g.waitNs += (int64_t)w * 1000;
    g_padSleepUs = w;
    if (sim)   // (no input layer on the host: the read the step makes next is made here, from the simulated stamps)
    {
        const uint64_t now = (uint64_t)NowNs() / 1000u;
        npl16::GameRead(npl16::SimStampUs(now), now);
    }
}
}
namespace {
struct PadLateState {
    int on = -1;
    int phase = 0;                  // MSC_PHASE_SYNC=1 (pace10): np_fps_phase.h PhaseSync decides the delay instead of NpFeel::PadLate
    NpFeel::PadLate ctl;
    npfps::PhaseSync ps;
    uint32_t lastDelayUs = 0;       // the delay slept in the previous call (PhaseSync takes it out of the thread's waits)
    uint32_t calls = 0, delayed = 0, lates = 0;
    uint64_t sumUs = 0;
    uint32_t maxUs = 0;
} g_pl;
float g_phaseBreakScale = 1.0f;   // MSC_PHASE_SYNC_BREAK=1 (a proof mutant, never set outside the host determinism check): the sleep leaks into the game clock
}
extern "C" void NpFpsPadLateWait(void)
{
    PadLateState& p = g_pl;
    if (p.on < 0)
    {
        const char* e = getenv("MSC_PAD_LATE");
        const char* ph = getenv("MSC_PHASE_SYNC");
        p.phase = (ph && *ph && atoi(ph) > 0 && &NpLatSlackTake != 0) ? 1 : 0;
        p.on = ((e && *e && atoi(e) > 0) || p.phase) && &NpLatSlackTake != 0 ? 1 : 0;
        if (const char* m = getenv("MSC_PAD_LATE_MAX_US")) { const long v = atol(m); if (v >= 0 && v <= 20000) p.ctl.maxUs = (uint32_t)v; }
        if (p.phase)
        {
            npfps::PhaseConfig c;
            c.maxUs = p.ctl.maxUs;   // (MSC_PAD_LATE_MAX_US also caps it)
            if (const char* m = getenv("MSC_PHASE_SYNC_MAX_US")) { const long v = atol(m); if (v >= 0 && v <= 20000) c.maxUs = (uint32_t)v; }
            if (const char* m = getenv("MSC_PHASE_SYNC_MARGIN_US")) { const long v = atol(m); if (v >= 0 && v <= 20000) c.marginUs = (uint32_t)v; }
            if (const char* m = getenv("MSC_PHASE_SYNC_WINDOW")) { const long v = atol(m); if (v >= 8 && v <= 128) c.window = (unsigned)v; }
            p.ps.Configure(c);
            fprintf(stderr, "[fps] phase sync on (MSC_PHASE_SYNC=1): the pad is read up to %u us later under a held cadence, single player; window %u pictures, margin %u us, a late picture with no delay in force is not charged to it\n",
                    (unsigned)c.maxUs, c.window, (unsigned)c.marginUs);
        }
        else if (p.on) fprintf(stderr, "[fps] late latch on (MSC_PAD_LATE=1): the pad is read up to %u us later under a held cadence, single player\n", (unsigned)p.ctl.maxUs);
    }
    if (g45.live) { p.ctl.delay = 0; p.ps.Reset(); p.lastDelayUs = 0; return; }   // helper f45: no late pad read at 45 (the room is the 1, 1, 2 pattern's, not a 30 period's)
    if (npl16::Wanted() && &NpLatSlackTake != 0) { Lat16Wait(); return; }   // wave 11 (press16): replaces the continuous delay of the two controllers below
    if (!p.on) return;
    if (&NpLanActive != 0 && NpLanActive()) { p.ctl.delay = 0; p.ps.Reset(); p.lastDelayUs = 0; return; }
    const int interval = NpFpsDisplayInterval();
    // int29: late-latch sleep is for held 30 Hz (interval >= 2) only; at 60 fps (interval == 1), sleeping on Core 0 before pad read risks sceKernelDelayThread overshoot past the 16.68 ms budget.
    if (interval < 2 || g.policy == kPolicyGame || g.paced) { p.ctl.delay = 0; p.ps.Reset(); p.lastDelayUs = 0; return; }
    uint32_t minSlack = 0xFFFFFFFFu, maxGap = 0;
    const int n = NpLatSlackTake(&minSlack, &maxGap);
    if ((npl16::LbWanted() || npl16::AgeLog()) && &NpLatSlackLbTake != 0)   // wave 11 (press16, MSC_LAT_SLACK_LB=1): the room of a finished picture from the callback's entry, not the gate's exit (MSC_LAT_PADAGE=1: only logged)
    {
        uint32_t lb = 0xFFFFFFFFu;
        if (NpLatSlackLbTake(&lb) > 0 && n > 0 && lb != 0xFFFFFFFFu)
        {
            if (npl16::AgeLog()) npl16::L().NoteRoom(lb);
            if (npl16::LbWanted() && lb > minSlack) minSlack = lb;
        }
    }
    const uint32_t period = (uint32_t)(interval == 2 ? HeldPeriod() : interval) * 16683u;   // (Fable verify5: 3 refreshes under MSC_HELD_CADENCE, else the late verdict flaps at a 50 ms gap)
    const bool late = n > 0 && maxGap > period + period / 2;
    uint32_t d;
    if (p.phase)
    {
        // the thread's idle time of the previous frame WITHOUT the sleep we put into it
        const int64_t waitUs = g.waitNsPrev / 1000 - (int64_t)p.lastDelayUs;
        const uint32_t cpu = waitUs > 0 ? (uint32_t)waitUs : 0u;
        if (late) ++p.lates;
        d = p.ps.Step(n > 0 ? minSlack : npfps::PhaseSync::kNone, cpu, late);
    }
    else
    {
        if (n > 0)
        {
            // the CPU slack: the previous frame's waits less the delay we put into them
            const int64_t waitUs = g.waitNsPrev / 1000 - (int64_t)p.ctl.delay;
            const uint32_t cpu = waitUs > 0 ? (uint32_t)waitUs : 0u;
            p.ctl.Observe(minSlack < cpu ? minSlack : cpu, late);
            if (late) ++p.lates;
        }
        d = p.ctl.DelayUs();
    }
    p.lastDelayUs = d;
    if (p.phase)
    {
        static const bool brk = [] { const char* e = getenv("MSC_PHASE_SYNC_BREAK"); return e && *e && atoi(e) > 0; }();
        g_phaseBreakScale = (brk && d) ? 0.97f : 1.0f;
    }
    ++p.calls;
    if (d)
    {
        ++p.delayed;
        p.sumUs += d;
        if (d > p.maxUs) p.maxUs = d;
        NP_GS(NPGS_WAIT_PADLATE);
        const int64_t t0 = NowNs();
        if (&np_sleep_ns != 0) np_sleep_ns((uint64_t)d * 1000u);
        else { timespec ts; ts.tv_sec = 0; ts.tv_nsec = (long)d * 1000L; nanosleep(&ts, 0); }
        const int64_t w = NowNs() - t0;
        if (w > 0) { g.waitNs += w; g_padSleepUs = (uint32_t)(w / 1000); }
    }
    if (p.phase && p.calls % 1200 == 0)
    {
        const npfps::PhaseStats& st = p.ps.Stats();
        fprintf(stderr, "[fps] phase sync: %u steps, %u delayed (%.0f %%, mean %.1f ms, max %.1f ms), %u late pictures (%u with a delay in force: backed off, %u not charged to it), margin now %.1f ms (%u widenings), delay now %.2f ms\n", p.calls, p.delayed,
                p.calls ? 100.0 * p.delayed / p.calls : 0.0, p.delayed ? (double)p.sumUs / p.delayed / 1000.0 : 0.0, p.maxUs / 1000.0, p.lates, st.ours, st.notOurs, p.ps.MarginNowUs() / 1000.0, st.widens, d / 1000.0);
    }
    else if (p.calls % 1200 == 0)
        fprintf(stderr, "[fps] late latch: %u steps, %u delayed (mean %.1f ms, max %.1f ms), %u late pictures, delay now %.2f ms\n", p.calls, p.delayed,
                p.delayed ? (double)p.sumUs / p.delayed / 1000.0 : 0.0, p.maxUs / 1000.0, p.lates, d / 1000.0);
}

extern "C" {

void NpFpsLateWaitStats(uint32_t* early, uint32_t* fallback, double* waitedMs)
{
    *early = g_late.early;
    *fallback = g_late.fallback;
    *waitedMs = g_late.waitedMs;
    g_late.early = g_late.fallback = 0;
    g_late.waitedMs = 0;
}

} // extern "C"

// ---- 1.2: bounded slip (MSC_SLIP=<percent>, default 0 = off; from the Galaxy port) -------------------------------------------------------------
// The simulation takes the task manager's frame time (nlTask.cpp) and runs fixed 20 ms steps to catch up with it. A frame that was late (say 40 ms
// where the picture policy wants 33.4) therefore asks the next frame for two steps instead of 1.67, and the extra step is more work for a machine that
// is already behind: the shortfall grows until a picture is held a whole refresh longer (30 -> 20 -> 15 fps). With slip on, the time a LATE frame adds
// to the simulation clock is cut: a frame longer than 1.05 periods advances the game by max(1.05 periods, frame / (1 + MSC_SLIP/100)), so the game runs
// at most MSC_SLIP percent slower than real time (20 -> never below 83 %), and never faster; the steps themselves are the same fixed steps with the
// same inputs, so no simulation result depends on it, only how many of them are made per second of wall clock. Frames that are on time (up to
// 1.05 periods, the jitter of the ticker) are untouched.
// Only while a picture policy with a period is in force (30 / 60 / auto: NpFpsDisplayInterval), only in the match (nlTask.cpp passes the state),
// and never in a LAN session (lockstep: the machines must keep one pace). Cut-scenes, movies and menus read the real frame time and are not slipped.
// 1.3 (wave 8) MSC_SLIP_STEPS=<n> (default 0 = off; the Vita plan sets 2; 1..5): the catch-up cap, after the Galaxy console finding that the mix of picture holds matters more than the mean fps and that
// capping the catch-up at two steps removed the holds of three refreshes and more at no cost. Without it a frame the game takes 100 ms over (nlTask.cpp clamps one task delta at 0.1 s) asks the
// next call for up to five 20 ms steps in one frame, which makes that frame late too, and a run of long holds follows. With it a late frame advances the game by at most n steps (n x 20 ms, never
// less than the slip limit of one period): the rest of the lateness is not made up, the game clock simply runs behind real time by that much (as MSC_SLIP does for 20 percent). The steps
// themselves are the same fixed steps with the same inputs. Needs MSC_SLIP on (the same single-player, in-match, picture-policy conditions).
// 1.3 (wave 9) MSC_SLIP_BUDGET_MS=<ms> (default 0 = the rule above, stateless; the Vita plan sets 30), MSC_SLIP_REFILL_PCT=<percent> (default 2): the rule above has no memory, so a scene that is late EVERY frame
// (a 60 target the picture hold cannot meet while LADDERHOLD waits for it, a held 30 that takes 40-50 ms) ran the game at 83 percent (17 % slower at 33 ms against a 16.7 ms target) or 80 percent (the
// two-step cap at 50 ms against 33.4) for as long as it lasted, with an even-looking picture: slow motion as a way to look smooth. With a budget (np_slip.h) the cut is paid from a bucket of that many
// milliseconds that fills at the refill percent of real time: a single hitch is cut as before (up to the bucket), a run of late frames drains it and the game then advances in full, so sustained the
// game clock loses no more than the refill percent and in any window W at most refill x W + the bucket. The decision is made ONCE per frame (SlipFrame, from NpFpsBeginFrame's real frame length) and
// every task of the frame is advanced by the same fraction (NpSlipDelta): the tasks of a frame must agree on the game time or the camera, the world and the particles drift apart. Needs MSC_SLIP on.
// The ledger: "[slip] match #n so far / end" lines (game time lost to slip of the match's play time, the worst 2 s window, fixed steps run per second of play, the frames cut) and a line when the bucket first runs dry.
namespace {
int g_slipPct = -1;
int g_slipSteps = -1;
float g_slipBudgetMs = 0.0f, g_slipRefillPct = 2.0f;
unsigned long g_slipFrames = 0, g_slipCalls = 0, g_slipCapped = 0;
double g_slipLostMs = 0.0, g_slipCapLostMs = 0.0;
npfps::SlipBudget g_slipBudget;
// pacing12 (MSC_CLOCK=debt|slip, default debt; needs MSC_SLIP and MSC_SLIP_BUDGET_MS on, as the plan has them): the game clock of live play keeps a bounded DEBT instead of cutting a late frame's game time (np_game_clock.h)
npfps::GameClock g_clock;
int g_clockOn = -1;
npsimpace::Model g_clockSim;   // pace18: the overlap's smoothed window / join overhead for the clock's cost of a picture
double g_clockSeenMs = 33.4, g_clockPerPicMs = 11.0, g_clockStepMs = 8.0;   // (the pictures seen, the cost of a picture without its steps and of one step: moving averages; the r9g split as the start)
bool g_clockWasOver = false;
unsigned long g_clockFrames = 0;
float g_slipF = 1.0f;          // this frame's advance as a fraction of the real time (SlipFrame), for every task of the frame
bool g_slipFValid = false;     // ... decided for this frame (a gameplay frame, single player, a picture policy with a period)
struct SlipLedger
{
    unsigned match = 0;
    unsigned long frames = 0, late = 0, cut = 0, dry = 0, atInterval[3] = {0, 0, 0};
    double realMs = 0, advMs = 0, wantMs = 0, lostMs = 0;
    unsigned long long steps = 0, stepsBase = 0;
    double binReal = 0, binLost = 0, prevBinReal = 0, prevBinLost = 0, worstLostMs = 0, worstRealMs = 0;   // 1 s bins: the worst two adjacent are the worst 2 s window
    int outFrames = 0;
    bool announced = false;
    unsigned long long lastLogFrames = 0;
} g_sl;
unsigned long long g_slipStepsAll = 0;
void SlipLog(const char* what)
{
    const SlipLedger& L = g_sl;
    const double lostPct = L.realMs > 0 ? 100.0 * L.lostMs / L.realMs : 0.0, wantPct = L.realMs > 0 ? 100.0 * L.wantMs / L.realMs : 0.0;
    const double wPct = L.worstRealMs > 0 ? 100.0 * L.worstLostMs / L.worstRealMs : 0.0;
    fprintf(stderr, "[slip] match #%u %s: lost %.0f ms of %.0f ms of play to slip (%.2f %%; the stateless rule would have lost %.0f ms = %.2f %%), %lu of %lu frames cut (%lu with the bucket dry), "
            "worst 2 s window lost %.0f ms of %.0f (%.2f %%), %llu fixed steps = %.2f a second of play (50.00 is exact), frames at interval 1/2/other %lu/%lu/%lu, bucket %.0f of %.0f ms (MSC_SLIP_BUDGET_MS)\n",
            L.match, what, L.lostMs, L.realMs, lostPct, L.wantMs, wantPct, L.cut, L.frames, L.dry, L.worstLostMs, L.worstRealMs, wPct, L.steps, L.realMs > 0 ? (double)L.steps * 1000.0 / L.realMs : 0.0,
            L.atInterval[0], L.atInterval[1], L.atInterval[2], g_slipBudget.Bucket() * 1000.0, g_slipBudget.Capacity() * 1000.0);
    if (g_clockOn > 0)
        fprintf(stderr, "[clock] match #%u %s: game clock %.2f %% of the frame time it was given (the ledger above counts the task-clamped time); debt now %.0f ms, shed %.0f ms, forgiven %.0f ms (%u stalls over 250 ms), overload entered %u times\n",
                L.match, what, g_clock.TotalPercent(), g_clock.DebtMs(), g_clock.ShedMs(), g_clock.ForgivenMs(), g_clock.Stalls(), g_clock.OverloadEntries());
}
}
namespace {
void SlipInit()
{
    if (g_slipPct >= 0) return;
    const char* e = getenv("MSC_SLIP");
    int v = 0;
    if (e && *e)
    {
        v = IEqual(e, "on") ? 20 : atoi(e);
        if (v < 0) v = 0;
        if (v > 25) v = 25;
    }
    g_slipPct = v;
    if (v) fprintf(stderr, "[fps] bounded slip on (MSC_SLIP=%d): a late frame advances the game by at most %d %% less than it took (single-player only)\n", v, v);
    const char* st = getenv("MSC_SLIP_STEPS");
    int n = (st && *st) ? atoi(st) : 0;
    if (n < 0) n = 0;
    if (n > 5) n = 5;
    g_slipSteps = v ? n : 0;
    fprintf(stderr, "[fps] catch-up cap (MSC_SLIP_STEPS=%d%s): %s\n", n, v ? "" : ", MSC_SLIP is off so it is off",
            g_slipSteps ? "a late frame advances the game by at most that many 20 ms steps" : "off: one task delta is clamped at 100 ms (five steps), as before");
    const char* bm = getenv("MSC_SLIP_BUDGET_MS");
    float b = (bm && *bm) ? (float)atof(bm) : 0.0f;
    if (b < 0.0f) b = 0.0f;
    if (b > 200.0f) b = 200.0f;
    const char* rp = getenv("MSC_SLIP_REFILL_PCT");
    float r = (rp && *rp) ? (float)atof(rp) : 2.0f;
    if (r < 0.0f) r = 0.0f;
    if (r > 20.0f) r = 20.0f;
    g_slipBudgetMs = v ? b : 0.0f;
    g_slipRefillPct = r;
    g_slipBudget.Configure(g_slipBudgetMs * 0.001f, r * 0.01f);
    g_sl = SlipLedger();
    g_slipStepsAll = 0;
    fprintf(stderr, "[fps] slip budget (MSC_SLIP_BUDGET_MS=%.0f%s): %s\n", b, v ? "" : ", MSC_SLIP is off so it is off",
            g_slipBudgetMs > 0 ? "late frames cut the game clock only from a bucket of that size that fills at the refill share of real time (MSC_SLIP_REFILL_PCT): sustained, the game never runs slower than that share below real time"
                               : "off: the stateless rule above (a frame late every time runs the game at 83 % or 80 %)");
}
void SlipNoteStep() { ++g_slipStepsAll; }
void SlipFrame(float real, unsigned taskState)
{
    SlipInit();
    g_slipFValid = false;
    g_slipF = 1.0f;
    if (g_clockOn < 0)
    {
        const char* e = getenv("MSC_CLOCK");
        g_clockOn = (g_slipPct > 0 && g_slipBudgetMs > 0.0f && !(e && *e && IEqual(e, "slip"))) ? 1 : 0;
        fprintf(stderr, "[fps] game clock (MSC_CLOCK=%s): %s\n", g_clockOn ? "debt" : "slip",
                g_clockOn ? "a late frame advances the game by what the game thread can run in a picture (2 to 5 steps from the measured costs), the rest is owed (at most 100 ms) and paid back by the next pictures; a stall over 250 ms is forgiven; what does not fit is shed and signalled as an overload (NpFpsGameOverload)"
                          : "the slip budget (bounded cut with a refilling bucket)");
    }
    if (g_slipBudgetMs <= 0.0f && !g_clockOn) return;
    SlipLedger& L = g_sl;
    const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
    const bool inMatchScene = (masked & (kTaskGameplay | kTaskReplay | kTaskNis | kTaskDebugReplay)) != 0;
    if (taskState != (unsigned)kTaskGameplay)
    {
        g_clock.Reset();   // (pacing12: nothing is owed across a pause, a replay or a load)
        // outside play: nothing is slipped. A ledger with play in it ends two seconds after the match scenes are over (the front end, a load)
        if (!inMatchScene && L.frames >= 60 && ++L.outFrames >= 120)
        {
            SlipLog("end");
            const unsigned next = L.match + 1;
            g_sl = SlipLedger();
            g_sl.match = next;
            g_sl.stepsBase = g_slipStepsAll;
            g_slipBudget.Refill();
        }
        return;
    }
    L.outFrames = 0;
    if (&NpLanActive != 0 && NpLanActive()) return;
    const int interval = g.paced ? 2 : NpFpsDisplayInterval();   // (MSC_FPS_BETWEEN=pace: a frame under two refreshes is on time, as at 30)
    if (interval <= 0 || !(real > 0.0f)) return;
    const float use = g_smBasis >= 0.0 ? (float)g_smBasis : real;   // (wave 9 MSC_SMOOTH_CLOCK: the picture's show-to-show time)
    float dt = use > 0.1f ? 0.1f : use;   // (nlTask.cpp's g_fTaskTimeUpperBound)
    const float period = (float)interval * (1001.0f / 60000.0f);
    const float limit = period * 1.05f;
    float capLost = 0.0f;
    const float old = npfps::SlipAdvance(dt, limit, g_slipPct, g_slipSteps, &capLost);
    const float want = dt - old;
    float adv;
    if (g_clockOn)
    {
        const double realMs = (double)real * 1000.0;
        const double target = (double)interval * (1001.0 / 60.0);
        if (realMs <= 250.0) g_clockSeenMs += (realMs - g_clockSeenMs) / 8.0;
        // the costs of the previous frame (still in g, reset at the end of NpFpsBeginFrame): the step cost and the cost of a picture without its steps
        if (g.simSteps > 0 && g.simMsAcc > 0.0)
        {
            const int sm = SimModeNow();
            bool onGame = true;
            if (sm != 0 && &NpSimtCoreMask != 0) { const unsigned m = (unsigned)NpSimtCoreMask(); onGame = m == 0 || m == 1u; }   // (the game thread is on core 0)
            const double st = g.simMsAcc / (double)g.simSteps, pp = npsimpace::ClockPerPictureMs(sm, onGame, realMs, (double)g.waitNs * 1e-6, g.simMsAcc, g.simWinMsAcc, g.simJoinMsAcc, g_clockSim);
            if (sm == npsimpace::kOverlap) g_clockSim.Observe(g.simWinMsAcc, g.simJoinMsAcc, g.simWallMsAcc);
            if (st < 40.0) g_clockStepMs += (st - g_clockStepMs) / 16.0;
            if (pp > 1.0 && pp < 100.0) g_clockPerPicMs += (pp - g_clockPerPicMs) / 16.0;
        }
        const npfps::ClockConfig cc;
        unsigned cap = npfps::CapSteps(cc, target, g_clockSeenMs, g_clockPerPicMs, g_clockStepMs);
        const double debtBefore = g_clock.DebtMs();
        if (g45.spread && g45.live60 && g_smBasis >= 0.0) cap = npfps::Spread60Cap(cap, debtBefore, cc, g45.spreadCapMs);   // hold60b: at the 60 tier the catch-up is spread, one step a picture
        const double advMs = g_clock.Frame((double)use * 1000.0, cap);   // (the costs above keep the frame's own time)
        if (cap == 1u && g45.spread && g45.live60 && (double)use * 1000.0 + debtBefore > cc.stepMs + 1e-6) { ++g45.spreadPics; if (debtBefore <= 0.0) ++g45.spreadFrom; }
        adv = (float)(advMs * 0.001);
        // (the tasks clamp their own delta at dt: the fraction is taken against that)
        ++g_clockFrames;
        if (g_clock.Overload() != g_clockWasOver)
        {
            g_clockWasOver = g_clock.Overload();
            fprintf(stderr, "[clock] %s at frame %llu: %s (steps a picture at most %u from picture %.1f ms (seen %.1f), step %.1f ms; debt %.0f ms, shed %.0f ms so far, %.0f ms forgiven in %u stalls)\n",
                    g_clockWasOver ? "OVERLOAD" : "overload over", (unsigned long long)g.frames,
                    g_clockWasOver ? "more than 2 % of the real time of the last second could not be run: the game is slow by that much, the picture and frame-rate controllers should lower their cost" : "the game runs at real time again",
                    cap, g_clockPerPicMs, g_clockSeenMs, g_clockStepMs, g_clock.DebtMs(), g_clock.ShedMs(), g_clock.ForgivenMs(), g_clock.Stalls());
        }
    }
    else adv = g_slipBudget.Frame(dt, want);
    g_slipF = dt > 0.0f ? adv / dt : 1.0f;
    g_slipFValid = true;
    g_slipAdvAbs = adv;
    if (L.match == 0 && L.frames == 0) L.match = 1;
    ++L.frames;
    ++L.atInterval[interval == 1 ? 0 : (interval == 2 ? 1 : 2)];
    const double rMs = (double)dt * 1000.0, aMs = (double)adv * 1000.0, wMs = (double)want * 1000.0;
    L.realMs += rMs;
    L.advMs += aMs;
    L.wantMs += wMs;
    L.lostMs += rMs - aMs;
    if (want > 0.0f) ++L.late;
    if (adv < dt - 1e-7f) ++L.cut;
    const bool refused = want > 0.0f && (dt - adv) < want - 1e-7f;   // the bucket held less than the stateless rule would have cut
    if (refused) ++L.dry;
    if (refused && !L.announced)
    {
        L.announced = true;
        fprintf(stderr, "[slip] match #%u: the bucket ran dry at frame %llu (%.0f ms of play): from here the game advances in full while frames stay late (interval %d, frame %.1f ms)\n", L.match,
                (unsigned long long)g.frames, L.realMs, interval, rMs);
    }
    L.binReal += rMs;
    L.binLost += rMs - aMs;
    if (L.binReal >= 1000.0)
    {
        const double wr = L.prevBinReal + L.binReal, wl = L.prevBinLost + L.binLost;
        if (wr >= 1500.0 && (L.worstRealMs <= 0 || wl / wr > L.worstLostMs / L.worstRealMs)) { L.worstLostMs = wl; L.worstRealMs = wr; }
        L.prevBinReal = L.binReal; L.prevBinLost = L.binLost;
        L.binReal = L.binLost = 0;
    }
    L.steps = g_slipStepsAll - L.stepsBase;
    if (L.frames % 3600 == 0) SlipLog("so far");
}
} // namespace
extern "C" int NpFpsGameOverload(void) { return g_clockOn > 0 && g_clock.Overload() ? 1 : 0; }   // pacing12: the game clock could not run real time over the last second (np_game_clock.h)
namespace {
// pacing13: the pair controller (np_pair_rate.h). Off unless MSC_PAIR_RATE=1; a seam for the renderer (bg30 reports each picture, hold11 says whether the background has room).
npfps::PairRate g_pair;
int g_pairOn = -1;
int g_pairBgRoom = 1;
bool PairOn()
{
    if (g_pairOn < 0)
    {
        const char* e = getenv("MSC_PAIR_RATE");
        g_pairOn = (e && *e && atoi(e) > 0) ? 1 : 0;
        if (g_pairOn)
        {   // lessons17 (MSC_PAIR_FAST_LEAVE=<n> default 0: leave when n of the last 16 pairs cost over MSC_PAIR_FAST_MS (default the leave limit); MSC_PAIR_SCREENS=1: see NpFpsBeginFrame)
            npfps::PairConfig c;
            const char* f = getenv("MSC_PAIR_FAST_LEAVE");
            if (f && *f && atoi(f) > 0 && atoi(f) <= 16) c.fastN = (unsigned)atoi(f);
            if (const char* m = getenv("MSC_PAIR_FAST_MS")) { const double v = atof(m); if (v >= 20.0 && v <= 100.0) c.fastMs = v; }
            g_pair = npfps::PairRate(c);
            if (c.fastN) fprintf(stderr, "[pair] fast leave on (MSC_PAIR_FAST_LEAVE=%u): the pair is left when %u of the last 16 pairs cost more than %.1f ms\n", c.fastN, c.fastN, c.fastMs > 0.0 ? c.fastMs : c.leaveMs);
        }
    }
    return g_pairOn > 0;
}
} // namespace
static bool PairControlled() { return PairOn() && &NpBg30Mode != 0 && NpBg30Mode() == 2; }
extern "C" int NpFpsPairControlled(void) { return PairControlled() ? 1 : 0; }
extern "C" void NpFpsPairBackground(int room) { g_pairBgRoom = room ? 1 : 0; }
extern "C" int NpFpsPairWanted(void) { return PairOn() && g_pair.Wanted() ? 1 : 0; }
extern "C" int NpFpsPairReport(int phase, float gameMs, float gpuMs, int heldRefreshes, int live, double now)
{
    if (!PairOn()) return 0;
    npfps::PairPicture p; p.now = now; p.live = live != 0; p.bgRoom = g_pairBgRoom != 0; p.overload = NpFpsGameOverload() != 0;
    p.phase = phase; p.gameMs = gameMs; p.gpuMs = gpuMs; p.heldRefreshes = heldRefreshes;
    return g_pair.Picture(p) ? 1 : 0;
}
extern "C" float NpSlipDelta(float dt)
{
    SlipInit();
    if (g_phaseBreakScale != 1.0f) return dt * g_phaseBreakScale;   // (the MSC_PHASE_SYNC_BREAK proof mutant)
    if (g_smBasis >= 0.0)   // (wave 9 MSC_SMOOTH_CLOCK: every gameplay task of the frame takes the frame's one time, after the debt clock when it runs)
    {
        const float b = g_slipFValid ? g_slipAdvAbs : (float)(g_smBasis > 0.1 ? 0.1 : g_smBasis);
        return b;
    }
    if (g_slipPct == 0) return dt;
    if (&NpLanActive != 0 && NpLanActive()) return dt;
    if (g_slipBudgetMs > 0.0f || g_clockOn > 0) return g_slipFValid ? dt * g_slipF : dt;   // (wave 9: the frame's one decision; see SlipFrame)
    const int interval = g.paced ? 2 : NpFpsDisplayInterval();   // (MSC_FPS_BETWEEN=pace: a frame under two refreshes is on time, as at 30)
    if (interval <= 0) return dt;
    const float period = (float)interval * (1001.0f / 60000.0f);
    const float limit = period * 1.05f;
    ++g_slipCalls;
    if (dt <= limit) return dt;
    float capLost = 0.0f;
    const float adv = npfps::SlipAdvance(dt, limit, g_slipPct, g_slipSteps, &capLost);
    if (capLost > 0.0f)
    {
        g_slipCapLostMs += (double)capLost * 1000.0;
        ++g_slipCapped;
    }
    ++g_slipFrames;
    g_slipLostMs += (double)(dt - adv) * 1000.0;
    if (g_slipFrames % 3000 == 0)   // (counted per task run: the task manager asks once for each task of the frame)
        fprintf(stderr, "[fps] slip: %lu of %lu task runs in the match were late, %.0f ms of task time not advanced; catch-up cap (MSC_SLIP_STEPS=%d): %lu of them cut to %d steps, %.0f ms of that not advanced%s\n", g_slipFrames,
                g_slipCalls, g_slipLostMs, g_slipSteps, g_slipCapped, g_slipSteps, g_slipCapLostMs, g_slipSteps && !g_slipCapped ? " (the cap did nothing here)" : "");
    return adv;
}

// ---- wave 9 MSC_SPEED_LOG=0|1 (default 1; log only, nothing in the game reads it): the game speed, always on. One "[speed]" line per 5 s of live play and one when a match ends:
//   [speed] play 5.0 s: 98.4 % of real time (246 steps = 4.920 s of game in 5.000 s, 150 pictures; lost 80 ms, worst picture 52 ms); match #1 so far 98.1 % (lost 1310 ms of 70.2 s, 2 of 14 windows under 98 %)
// Steps are the fixed 20 ms steps the simulation ran (NpFpsSimStep), wall time is the clock between two frames of live play, unclamped (np_speed.h). The owner's rule is 98 % or better.
// Live play is the gameplay task state alone (a pause, replay, cut-scene or load is not counted). A game-time scale below 1 in play (the game's own slow-motion effects) shows as under 100 %.
namespace {
extern "C" float NpFpsGameTimeScale(void) __attribute__((weak));   // pacing13: the game's own time scale (FixedUpdateTask), 1 normally
npfps::SpeedMeter g_speed;
int g_speedOn = -1;
unsigned g_speedMatch = 1;
unsigned g_speedOut = 0;
unsigned long long g_speedWindows = 0, g_speedUnder = 0;
void SpeedLine(const char* what)
{
    const npfps::SpeedWindow& w = g_speed.win();
    const npfps::SpeedWindow& m = g_speed.match();
    fprintf(stderr, "[speed] %s %.1f s: %.1f %% of real time, %.1f %% of wanted (%u steps = %.3f s of game in %.3f s, %u pictures; lost %.0f ms, worst picture %.0f ms; slow motion %.0f %% of the window); match #%u so far %.1f %% of real, %.1f %% of wanted (lost %.0f ms of %.1f s, %u of %u windows under %.0f %% of wanted, %u of real; slow motion %.1f %% of the match)\n",
            what, w.wallMs / 1000.0, w.Percent(), w.WantedPercent(), w.steps, w.gameMs / 1000.0, w.wallMs / 1000.0, w.pictures, w.lostMs, w.worstPictureMs, w.SlowShare(), g_speedMatch, m.Percent(), m.WantedPercent(), m.wallMs - m.gameMs,
            m.wallMs / 1000.0, g_speed.underRule(), g_speed.windows(), npfps::SpeedMeter::kRuleMs, g_speed.underRuleReal(), m.SlowShare());
    ++g_speedWindows;
    if (w.WantedPercent() < npfps::SpeedMeter::kRuleMs) ++g_speedUnder;
}
void SpeedFrame(unsigned taskState)
{
    if (g_speedOn < 0)
    {
        const char* e = getenv("MSC_SPEED_LOG");
        g_speedOn = (e && *e == '0') ? 0 : 1;
    }
    if (!g_speedOn) return;
    if (taskState == (unsigned)kTaskGameplay)
    {
        g_speedOut = 0;
        if (g_speed.Frame(NowSeconds(), g_slipStepsAll, NpFpsGameTimeScale ? NpFpsGameTimeScale() : 1.0)) SpeedLine("play");
        return;
    }
    if (g_speed.Break()) SpeedLine("play (cut short)");
    const unsigned masked = taskState & ~(unsigned)(kTaskPaused | kTaskHomeMenu);
    const bool inMatchScene = (masked & (kTaskGameplay | kTaskReplay | kTaskNis | kTaskDebugReplay)) != 0;
    if (!inMatchScene && g_speed.match().wallMs > 0 && ++g_speedOut >= 120)
    {
        const npfps::SpeedWindow& m = g_speed.match();
        fprintf(stderr, "[speed] match #%u end: %.1f %% of real time, %.1f %% of wanted, over %.1f s of live play (lost %.0f ms; %u of %u windows under %.0f %% of wanted, %u of real; slow motion %.1f %% of the match)\n", g_speedMatch, m.Percent(), m.WantedPercent(), m.wallMs / 1000.0,
                m.wallMs - m.gameMs, g_speed.underRule(), g_speed.windows(), npfps::SpeedMeter::kRuleMs, g_speed.underRuleReal(), m.SlowShare());
        g_speed.NewMatch();
        ++g_speedMatch;
        g_speedOut = 0;
    }
}
}

// ---- 1.3 MSC_LOAD_TRACE=1 (default off; a diagnostic, one clock read per loading step and per chunk inflated): where a load's time goes. One line per step of the game's loader
// (AsyncLoadingManager::SetLoadingComment), with the figures since the previous line. A step's time that the disc, zlib and the byte-order conversion do not explain is either the
// game's own parsing or a frame waited out: the frames column tells which (33 ms a frame under the 30 pace; MSC_LOAD_FPS=60 is the A/B).
extern "C" uint64_t np_time_real_ns(void) __attribute__((weak));
extern "C" volatile uint32_t np_stat_dvd_reads __attribute__((weak)), np_stat_dvd_kib __attribute__((weak)), np_stat_sync_kib __attribute__((weak));
extern "C" void npa_hook_stats(unsigned long* files, unsigned long* bytes, unsigned long* failed, double* seconds) __attribute__((weak));
namespace {
bool LoadTraceOn()
{
    static const bool on = [] { const char* e = getenv("MSC_LOAD_TRACE"); return e && *e == '1'; }();
    return on;
}
uint64_t g_loadInflateNs = 0, g_loadInflateKib = 0;
}
extern "C" uint64_t NpLoadInflateBegin(void)
{
    return LoadTraceOn() && &np_time_real_ns != 0 ? np_time_real_ns() : 0;
}
extern "C" void NpLoadInflateEnd(uint64_t t0, unsigned size)
{
    if (!t0) return;
    g_loadInflateNs += np_time_real_ns() - t0;
    g_loadInflateKib += size / 1024u;
}
extern "C" void NpLoadStep(const char* comment)
{
    if (!LoadTraceOn() || &np_time_real_ns == 0) return;
    static uint64_t lastNs = 0, lastFrames = 0, lastInflate = 0, lastInflateKib = 0;
    static uint32_t lastReads = 0, lastKib = 0, lastSync = 0;
    static double lastConv = 0;
    const uint64_t now = np_time_real_ns();
    const uint32_t reads = &np_stat_dvd_reads ? np_stat_dvd_reads : 0, kib = &np_stat_dvd_kib ? np_stat_dvd_kib : 0, sync = &np_stat_sync_kib ? np_stat_sync_kib : 0;
    double conv = 0;
    if (&npa_hook_stats != 0) npa_hook_stats(0, 0, 0, &conv);
    if (lastNs)
        fprintf(stderr, "[load] %.0f ms, %llu frames, disc %u reads %u KiB (waited for %u KiB), inflate %.0f ms %llu KiB in, convert %.0f ms; next: %s\n", (double)(now - lastNs) * 1e-6,
                (unsigned long long)(g.frames - lastFrames), reads - lastReads, kib - lastKib, sync - lastSync, (double)(g_loadInflateNs - lastInflate) * 1e-6,
                (unsigned long long)(g_loadInflateKib - lastInflateKib), (conv - lastConv) * 1000.0, comment ? comment : "?");
    else
        fprintf(stderr, "[load] trace starts at %s\n", comment ? comment : "?");
    lastNs = now; lastFrames = g.frames; lastInflate = g_loadInflateNs; lastInflateKib = g_loadInflateKib; lastReads = reads; lastKib = kib; lastSync = sync; lastConv = conv;
}
// held24: the held gate's action counters, in the exit summary
namespace {
void NpFpsBpHeldSummary() { if (g_bpHeldPics || g_bpRelPics) fprintf(stderr, "[fps] held gate (MSC_LAT_BP_HELD_DEPTH %u): %u pictures in held scenes passed it, %u of them waited; release after a held scene: %u pictures, %u waited\n", g_bp.GateDepthHeld(), g_bpHeldPics, g_bpHeldWaited, g_bpRelPics, g_bpRelWaited); }
}
