// MSC_GSPLIT (np/gsplit.h): the game thread's picture split. Logging builds; default off.
#ifdef NP_NATIVE
#include "np/gsplit.h"
extern "C" int NpBg30Phase(void) __attribute__((weak));
extern "C" int NpBg30Mode(void) __attribute__((weak));

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if !defined(__vita__)
#include <sys/syscall.h>
#endif
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

extern unsigned int gNetworkRandomSeed;   // (the simulation seed in the determinism trace: touched only by the test-only MSC_GSPLIT_BREAK mutant below)
extern "C" {
int g_npGsOn = 0;
unsigned int g_npGsCnt[32];
int NpGsPhysCensus(char* out, int n) __attribute__((weak));   // (src/Game/Physics.cpp: geoms by class, static / dynamic, bodies enabled)
}

namespace {
const int kEvery = 300;
volatile int g_phase = NPGS_OUTSIDE;     // read by the sampler
pthread_t g_owner;
bool g_inited = false, g_haveOwner = false, g_inPass = false;
int g_cur = NPGS_OUTSIDE;
uint64_t g_last = 0;
uint64_t g_acc[NPGS_MAX];
// wave 10 (bg30b): the same phases, kept apart for the FULL and the LIGHT pictures of a background-at-30 pair (np/bg30.h NpBg30Phase): a snapshot of g_acc at the top of the pass, the picture's
// own share added to g_kind[kind] at its end. Printed with each line as [gsplit-bg30].
uint64_t g_snap[NPGS_MAX];
uint64_t g_kind[3][NPGS_MAX];
unsigned g_kindPics[3];

uint32_t g_cnt[NPGS_MAX];
volatile uint32_t g_samples[NPGS_MAX];
volatile uint32_t g_sampleTotal = 0;
const int kPair0 = NPGS_TASK0 + 32;          // dynamic slots: 32 task names, then class-pair / body-kind names
const char* g_taskName[NPGS_MAX - NPGS_TASK0];
int g_nTasks = 0;
char g_pairBuf[NPGS_MAX - kPair0][40];
int g_pairKey[NPGS_MAX - kPair0];
int g_nPairs = 0;
unsigned g_pics = 0;
uint64_t g_t0 = 0, g_cpu0 = 0;
int g_waitDepth = 0;
int g_waitPrev = -1;

// gt27: per picture, by the number of simulation steps it ran (0, 1, 2, 3+): work ms (the non-wait phases) for p50 / p95, and the means of the groups. A picture with exactly
// ONE step is what a 60 fps picture with a step is (0.83 steps a picture at 60); the held 30 has 1.67 steps a picture, mostly 2-step pictures.
const int kPicClasses = 4;
float g_picWork[kPicClasses][kEvery];
unsigned g_picN[kPicClasses];
double g_picSum[kPicClasses][6];     // sim, render task, particles, end frame, begin frame + fe + runner + other tasks, waits
uint32_t g_snapSteps = 0;
float g_picNet[kPicClasses][kEvery];   // work less the split's own clock reads (marks x the measured cost of one read)
uint64_t g_snapMarks = 0;
int g_detail = 1;                     // MSC_GSPLIT_DETAIL=0: the per-step detail phases (physics / AI / events parts, narrow-phase class pairs) are not marked: ~1000 fewer clock reads a picture
// [gsplit-p95]: per 1-step picture, the work of each group, for the comparison of the slowest 5 % with the mean
const int kGroups = 17;
const char* const kGroupName[kGroups] = {"physics", "ai", "events", "sim-other", "blend", "chars", "npcs", "stadium", "crowd", "render-other", "particles", "end-frame", "begin-frame", "front-end", "mem", "log", "other"};
float g_grp[kEvery][kGroups];
float g_grpNet[kEvery];
unsigned g_grpN = 0;
int g_bfTask = -2;
uint64_t g_memParent[NPGS_MAX];      // time of NPGS_MEM by the phase it was nested in
int g_memParentCur = -1;
uint64_t g_memT0 = 0;
uint64_t g_marks = 0;                // Enter + Leave calls of the window (clock reads)
double g_clockUs = 0.0;              // the cost of one clock read, measured at Init


const int kNamed = 121;   // fixed ids with a name (0 .. NPGS_AUDIO_MIX)
static_assert(NPGS_AUDIO_MIX + 1 == kNamed && kNamed <= NPGS_TASK0, "gsplit: the named ids and kName must agree");
const char* const kName[kNamed] = {
    "outside", "runner", "wait-swap", "wait-join", "wait-handover", "wait-pace", "sim-pre", "sim-clock", "ai", "npc", "prephys", "physics", "pose", "net",
    "events", "pad", "r-pre", "r-npcs", "r-stage", "r-replaysnap", "r-blend", "r-chars", "r-stadium", "r-hud", "r-shadow", "r-fx", "crowd-pos", "crowd-sprites",
    "crowd-draw", "r-numbers", "p-update", "p-render", "e-warble", "e-send", "e-skin", "audio", "dvd", "fe",
    "precollide", "broad", "near", "contact-cb", "joints", "net-ball", "preupdate", "islands", "qs-prep", "qs-jacobian", "qs-rhs", "qs-sor", "qs-integrate", "postupdate", "ball-body", "phys-events",
    "ai-teampre", "ai-misc", "ai-teamtimers", "ai-intercept", "ai-teamai", "ai-teamrest", "ai-gamectx", "ai-fielder-self", "ai-fielder-script", "ai-vmstate(all machines)", "ai-fielder-action",
    "ai-fielder-head", "ai-fielder-anim", "ai-fielder-ctrl", "ai-goalie", "ai-ball", "ai-world", "ev-dispatch", "ev-peach", "ev-snapshot",
    "bf-pace", "bf-files", "bf-presentation", "bf-glbegin", "bf-chars", "bf-camera", "bf-setup", "fe-input", "fe-presentation", "fe-draw", "blend-posecull", "blend-chars", "draw-chars", "draw-powerups",
    "wait-padlate", "wait-gate", "wait-other", "sp-input", "sp-edge", "sp-padctl", "sp-lan", "sp-tail", "sp-loop", "bf-fps", "bf-probe",
    "e-still", "e-joinwork", "e-snapviews", "e-atfork", "e-pktrefresh", "e-fork", "e-hook", "e-tail", "e-sendtail", "run-ticker", "run-hooks", "run-movie",
    "fe-light", "fe-update", "fe-res", "fe-scupd", "fe-screnderer", "fe-models", "fe-rest", "mem", "log", "audio-mix"};
const char* const kClass[] = {"sphere", "box", "capsule", "cylinder", "plane", "ray", "transform", "trimesh", "simplespace", "hashspace", "quadspace", "user0", "user1", "user2", "user3", "composite"};

#if !defined(__vita__)
// The harness's virtual clock (MSC_VIRTUAL_TIME) interposes clock_gettime and moves time on by one tick for every read on the game thread: a read here would change
// the simulation's time base. A raw system call is not interposed (platform/vita/src/plat_realclock.h).
uint64_t RawClock(int clock) {
#if defined(__arm__)
    long ts[2] = {0, 0};
    syscall(SYS_clock_gettime, clock, ts);
    return (uint64_t)ts[0] * 1000000u + (uint64_t)ts[1] / 1000u;
#else
    timespec ts{};
    syscall(SYS_clock_gettime, clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}
#endif
inline uint64_t Now() {
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    return RawClock(CLOCK_MONOTONIC);
#endif
}
uint64_t ThreadCpu() {
#if defined(__vita__)
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0) return 0;
    return info.runClocks;
#else
    return RawClock(CLOCK_THREAD_CPUTIME_ID);
#endif
}
bool IsDetail(int id);
bool IsWait(int id) { return (id >= NPGS_WAIT_SWAP && id <= NPGS_WAIT_PACE) || (id >= NPGS_WAIT_PADLATE && id <= NPGS_WAIT_OTHER); }
const char* Name(int id) {
    if (id < kNamed) return kName[id];
    if (id >= NPGS_TASK0 && id - NPGS_TASK0 < g_nTasks) return g_taskName[id - NPGS_TASK0];
    if (id >= kPair0 && id - kPair0 < g_nPairs) return g_pairBuf[id - kPair0];
    return "?";
}
bool IsDetail(int id) { return (id >= NPGS_PH_PRECOLLIDE && id <= NPGS_EV_SNAPSHOT) || id >= NPGS_TASK0 + 32; }
int GroupOf(int id) {
    if (id == NPGS_SIM_PHYSICS || (id >= NPGS_PH_PRECOLLIDE && id <= NPGS_PH_PHYEV) || id >= NPGS_TASK0 + 32) return 0;
    if (id == NPGS_SIM_AI || (id >= NPGS_AI_TEAMPRE && id <= NPGS_AI_WORLD)) return 1;
    if (id == NPGS_SIM_EVENTS || (id >= NPGS_EV_DISPATCH && id <= NPGS_EV_SNAPSHOT)) return 2;
    if ((id >= NPGS_SIM_PRE && id <= NPGS_SIM_PAD) || (id >= NPGS_SP_INPUT && id <= NPGS_SP_LOOP)) return 3;
    if (id == NPGS_R_BLEND || id == NPGS_RB_POSE || id == NPGS_RB_CHARS) return 4;
    if (id == NPGS_R_CHARS || id == NPGS_RC_CHARS || id == NPGS_RC_POWERUPS) return 5;
    if (id == NPGS_R_NPCS) return 6;
    if (id == NPGS_R_STADIUM) return 7;
    if (id >= NPGS_R_CROWDPOS && id <= NPGS_R_CROWDDRAW) return 8;
    if (id >= NPGS_R_PRE && id <= NPGS_R_NUMBERS) return 9;
    if (id == NPGS_P_UPDATE || id == NPGS_P_RENDER) return 10;
    if ((id >= NPGS_E_WARBLE && id <= NPGS_E_SKIN) || (id >= NPGS_E_STILL && id <= NPGS_E_SENDTAIL)) return 11;
    if ((id >= NPGS_BF_PACE && id <= NPGS_BF_SETUP) || id == NPGS_BF_FPS || id == NPGS_BF_PROBE || id == g_bfTask) return 12;
    if (id == NPGS_FE || (id >= NPGS_FE_INPUT && id <= NPGS_FE_DRAW) || (id >= NPGS_FE_LIGHT && id <= NPGS_FE_REST)) return 13;
    if (id == NPGS_MEM) return 14;
    if (id == NPGS_LOG) return 15;
    return 16;
}
void* Sampler(void*) {
    for (;;) {
        usleep(1000);
        const int id = g_phase;
        if (id >= 0 && id < NPGS_MAX) g_samples[id] = g_samples[id] + 1;
        g_sampleTotal = g_sampleTotal + 1;
    }
    return 0;
}
// Host / qemu: a phase is charged the thread's CPU time (a loaded machine preempts the thread inside phases; the line must sum to CPU), a wait its wall time. Vita: wall time
// for all (one cheap clock read per marker; the CPU figure is read from the thread's runClocks at the line): a preempted core-0 thread then shows as sum above CPU.
uint64_t g_lastCpu = 0;
void Charge(int id) {
    const uint64_t t = Now();
#if defined(__vita__)
    g_acc[id] += t - g_last;
#else
    const uint64_t c = ThreadCpu();
    g_acc[id] += IsWait(id) ? t - g_last : c - g_lastCpu;
    g_lastCpu = c;
#endif
    g_last = t;
}
void Init() {
    g_inited = true;
    const char* e = getenv("MSC_GSPLIT");
    const int v = e && *e ? atoi(e) : 0;
    if (v <= 0) return;
    g_owner = pthread_self();
    g_haveOwner = true;
    g_last = g_t0 = Now();
    g_cpu0 = g_lastCpu = ThreadCpu();
    if (v >= 2 || v == 1) {   // (the sampler is part of the line; MSC_GSPLIT=1 and 2 both run it, 3 turns it off)
        if (v != 3) {
            pthread_t th;
            if (pthread_create(&th, 0, Sampler, 0) == 0) pthread_detach(th);
        }
    }
    {   // gt27: the cost of one clock read (every Enter / Leave makes one): the markers' own share of the line
        const uint64_t a = Now();
        uint64_t x = 0;
        for (int i = 0; i < 2000; ++i) x += Now();
        const uint64_t b = Now();
        g_clockUs = (double)(b - a) / 2000.0;
        if (x == 1) g_clockUs += 0.0;
    }
    {
        const char* d = getenv("MSC_GSPLIT_DETAIL");
        g_detail = (d && *d == '0') ? 0 : 1;
        if (!g_detail) fprintf(stderr, "[gsplit] MSC_GSPLIT_DETAIL=0: the per-step detail phases are not marked ([gsplit-phys] / [gsplit-ai] parts read 0; their time stays in physics / ai / events)\n");
    }
    g_npGsOn = 1;
    fprintf(stderr, "[gsplit] on (MSC_GSPLIT=%d): one line per %d pictures; phases are ms per picture, self time (%s); sampler %s\n", v, kEvery,
#if defined(__vita__)
        "wall clock; sum may exceed CPU when the thread is preempted",
#else
        "thread CPU time, waits in wall time",
#endif
        v == 3 ? "off" : "1 ms");
}
void Print() {
    const uint64_t now = Now();
    const uint64_t cpu = ThreadCpu();
    const double pics = (double)kEvery;
    double sumWork = 0, sumWait = 0;
    for (int i = 0; i < NPGS_MAX; ++i) (IsWait(i) ? sumWait : sumWork) += (double)g_acc[i] / 1000.0 / pics;
    auto grp = [&](int a, int b) { double s = 0; for (int i = a; i <= b; ++i) s += (double)g_acc[i] / 1000.0 / pics; return s; };
    double pairSum = 0;                       // the narrow-phase / body-kind phases (dynamic ids): children of physics
    for (int i = kPair0; i < kPair0 + g_nPairs; ++i) pairSum += (double)g_acc[i] / 1000.0 / pics;
    const double phys = grp(NPGS_SIM_PHYSICS, NPGS_SIM_PHYSICS) + grp(NPGS_PH_PRECOLLIDE, NPGS_PH_PHYEV) + pairSum;
    const double ai = grp(NPGS_SIM_AI, NPGS_SIM_AI) + grp(NPGS_AI_TEAMPRE, NPGS_AI_WORLD);
    const double ev = grp(NPGS_SIM_EVENTS, NPGS_SIM_EVENTS) + grp(NPGS_EV_DISPATCH, NPGS_EV_SNAPSHOT);
    const double sim = grp(NPGS_SIM_PRE, NPGS_SIM_PAD) + grp(NPGS_PH_PRECOLLIDE, NPGS_AI_WORLD) + grp(NPGS_EV_DISPATCH, NPGS_EV_SNAPSHOT) + pairSum + grp(NPGS_SP_INPUT, NPGS_SP_LOOP);
    const double simPre = grp(NPGS_SIM_PRE, NPGS_SIM_PRE) + grp(NPGS_SP_INPUT, NPGS_SP_LOOP);   // (gt27: its new parts stay in 'pre'; the late pad sleep moved to wait-padlate)
    char line[3000];
    int n = snprintf(line, sizeof line, "[gsplit] %d pics wall %.1f ms/pic: sim %.2f (steps %.2f/pic: pre %.2f clock %.2f ai %.2f npc %.2f prephys %.2f physics %.2f pose %.2f net %.2f events %.2f pad %.2f) render-task %.2f (pre %.2f npcs %.2f stage %.2f snap %.2f blend %.2f chars %.2f stadium %.2f hud %.2f shadow %.2f fx %.2f crowd-pos %.2f crowd-sprites %.2f crowd-draw %.2f numbers %.2f) particles %.2f (update %.2f render %.2f) end-frame %.2f (warble %.2f send %.2f of which skin %.2f) audio %.2f dvd %.2f fe %.2f runner %.2f outside %.2f mem %.2f log %.2f",
        kEvery, (double)(now - g_t0) / 1000.0 / pics,
        sim, (double)g_cnt[NPGS_SIM_AI] / pics,
        simPre, grp(NPGS_SIM_CLOCK, NPGS_SIM_CLOCK), ai, grp(NPGS_SIM_NPC, NPGS_SIM_NPC), grp(NPGS_SIM_PREPHYS, NPGS_SIM_PREPHYS), phys, grp(NPGS_SIM_POSE, NPGS_SIM_POSE), grp(NPGS_SIM_NET, NPGS_SIM_NET), ev, grp(NPGS_SIM_PAD, NPGS_SIM_PAD),
        grp(NPGS_R_PRE, NPGS_R_NUMBERS) + grp(NPGS_RB_POSE, NPGS_RC_POWERUPS), grp(NPGS_R_PRE, NPGS_R_PRE), grp(NPGS_R_NPCS, NPGS_R_NPCS), grp(NPGS_R_STAGE, NPGS_R_STAGE), grp(NPGS_R_REPLAYSNAP, NPGS_R_REPLAYSNAP), grp(NPGS_R_BLEND, NPGS_R_BLEND) + grp(NPGS_RB_POSE, NPGS_RB_CHARS), grp(NPGS_R_CHARS, NPGS_R_CHARS) + grp(NPGS_RC_CHARS, NPGS_RC_POWERUPS), grp(NPGS_R_STADIUM, NPGS_R_STADIUM), grp(NPGS_R_HUD, NPGS_R_HUD), grp(NPGS_R_SHADOW, NPGS_R_SHADOW), grp(NPGS_R_FX, NPGS_R_FX), grp(NPGS_R_CROWDPOS, NPGS_R_CROWDPOS), grp(NPGS_R_CROWDSPR, NPGS_R_CROWDSPR), grp(NPGS_R_CROWDDRAW, NPGS_R_CROWDDRAW), grp(NPGS_R_NUMBERS, NPGS_R_NUMBERS),
        grp(NPGS_P_UPDATE, NPGS_P_RENDER), grp(NPGS_P_UPDATE, NPGS_P_UPDATE), grp(NPGS_P_RENDER, NPGS_P_RENDER),
        grp(NPGS_E_WARBLE, NPGS_E_SKIN) + grp(NPGS_E_STILL, NPGS_E_SENDTAIL), grp(NPGS_E_WARBLE, NPGS_E_WARBLE), grp(NPGS_E_SEND, NPGS_E_SEND) + grp(NPGS_E_SKIN, NPGS_E_SKIN) + grp(NPGS_E_STILL, NPGS_E_SENDTAIL), grp(NPGS_E_SKIN, NPGS_E_SKIN),
        grp(NPGS_AUDIO, NPGS_AUDIO) + grp(NPGS_AUDIO_MIX, NPGS_AUDIO_MIX), grp(NPGS_DVD, NPGS_DVD), grp(NPGS_FE, NPGS_FE) + grp(NPGS_FE_INPUT, NPGS_FE_DRAW) + grp(NPGS_FE_LIGHT, NPGS_FE_REST),
        grp(NPGS_RUNNER, NPGS_RUNNER) + grp(NPGS_RUN_TICKER, NPGS_RUN_MOVIE), grp(NPGS_OUTSIDE, NPGS_OUTSIDE), grp(NPGS_MEM, NPGS_MEM), grp(NPGS_LOG, NPGS_LOG));
    for (int i = NPGS_TASK0; i < NPGS_TASK0 + g_nTasks && n < (int)sizeof line - 80; ++i) {
        double v = (double)g_acc[i] / 1000.0 / pics;
        if (g_taskName[i - NPGS_TASK0] && !strcmp(g_taskName[i - NPGS_TASK0], "Begin Frame")) v += grp(NPGS_BF_PACE, NPGS_BF_SETUP) + grp(NPGS_BF_FPS, NPGS_BF_PROBE);
        n += snprintf(line + n, sizeof line - n, " task:%s %.2f", Name(i), v);
    }
    n += snprintf(line + n, sizeof line - n, " | wait (blocked, not CPU) %.2f (swap %.2f join %.2f handover %.2f pace %.2f padlate %.2f gate %.2f other %.2f) | sum %.2f of CPU %.2f (%.0f%%)",
        sumWait, grp(NPGS_WAIT_SWAP, NPGS_WAIT_SWAP), grp(NPGS_WAIT_JOIN, NPGS_WAIT_JOIN), grp(NPGS_WAIT_HANDOVER, NPGS_WAIT_HANDOVER), grp(NPGS_WAIT_PACE, NPGS_WAIT_PACE),
        grp(NPGS_WAIT_PADLATE, NPGS_WAIT_PADLATE), grp(NPGS_WAIT_GATE, NPGS_WAIT_GATE), grp(NPGS_WAIT_OTHER, NPGS_WAIT_OTHER),
        sumWork, (double)(cpu - g_cpu0) / 1000.0 / pics, cpu > g_cpu0 ? 100.0 * sumWork / ((double)(cpu - g_cpu0) / 1000.0 / pics) : 0.0);
    // the sampler: the ids it saw, as a share of its samples
    const uint32_t total = g_sampleTotal;
    if (total > 0) {
        int order[NPGS_MAX];
        for (int i = 0; i < NPGS_MAX; ++i) order[i] = i;
        for (int i = 0; i < NPGS_MAX; ++i) for (int j = i + 1; j < NPGS_MAX; ++j) if (g_samples[order[j]] > g_samples[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        n += snprintf(line + n, sizeof line - n, " | sampler %u: ", total);
        for (int k = 0; k < 14 && g_samples[order[k]] * 100u >= total && n < (int)sizeof line - 40; ++k)
            n += snprintf(line + n, sizeof line - n, "%s %.0f%% ", Name(order[k]), 100.0 * g_samples[order[k]] / total);
    }
    snprintf(line + n, sizeof line - n, "\n");
    fputs(line, stderr);

    // v2 detail: per simulation STEP for physics / ai / events (the steps this window ran), per picture for the others; counters per step
    const double steps = g_cnt[NPGS_SIM_AI] > 0 ? (double)g_cnt[NPGS_SIM_AI] : 1.0;
    auto st = [&](int a, int b) { double s = 0; for (int i = a; i <= b; ++i) s += (double)g_acc[i] / 1000.0 / steps; return s; };
    auto c = [&](int k) { return (double)g_npGsCnt[k] / steps; };
    char d[3200];
    int m = snprintf(d, sizeof d, "[gsplit-phys] %.0f steps in %d pics, ms per STEP: physics %.2f (self %.2f precollide %.2f broad %.2f near %.2f contact-cb %.2f joints %.2f net-ball %.2f preupdate %.2f islands %.2f qs-prep %.2f qs-jacobian %.2f qs-rhs %.2f qs-sor %.2f qs-integrate %.2f postupdate %.2f ball-body %.2f events %.2f)",
        steps, kEvery, phys * pics / steps, st(NPGS_SIM_PHYSICS, NPGS_SIM_PHYSICS), st(NPGS_PH_PRECOLLIDE, NPGS_PH_PRECOLLIDE), st(NPGS_PH_BROAD, NPGS_PH_BROAD), st(NPGS_PH_NEAR, NPGS_PH_NEAR), st(NPGS_PH_CONTACTCB, NPGS_PH_CONTACTCB),
        st(NPGS_PH_JOINTS, NPGS_PH_JOINTS), st(NPGS_PH_NETBALL, NPGS_PH_NETBALL), st(NPGS_PH_PREUPD, NPGS_PH_PREUPD), st(NPGS_PH_ISLANDS, NPGS_PH_ISLANDS), st(NPGS_PH_QS_PREP, NPGS_PH_QS_PREP), st(NPGS_PH_QS_JAC, NPGS_PH_QS_JAC),
        st(NPGS_PH_QS_RHS, NPGS_PH_QS_RHS), st(NPGS_PH_QS_SOR, NPGS_PH_QS_SOR), st(NPGS_PH_QS_INT, NPGS_PH_QS_INT), st(NPGS_PH_POSTUPD, NPGS_PH_POSTUPD), st(NPGS_PH_BALLBODY, NPGS_PH_BALLBODY), st(NPGS_PH_PHYEV, NPGS_PH_PHYEV));
    // the dynamic phases: narrow phase by class pair (dCollide) and PreUpdate / PostUpdate by body class: time and calls per step, largest first
    int ord[NPGS_MAX];
    int np = 0;
    for (int i = kPair0; i < kPair0 + g_nPairs; ++i) ord[np++] = i;
    for (int i = 0; i < np; ++i) for (int j = i + 1; j < np; ++j) if (g_acc[ord[j]] > g_acc[ord[i]]) { int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    m += snprintf(d + m, sizeof d - m, " by-class [ms/step calls/step]:");
    for (int k = 0; k < np && k < 14 && m < (int)sizeof d - 600; ++k)
        m += snprintf(d + m, sizeof d - m, " %s %.2f/%.1f", Name(ord[k]), (double)g_acc[ord[k]] / 1000.0 / steps, (double)g_cnt[ord[k]] / steps);
    m += snprintf(d + m, sizeof d - m, " | counts per step: geoms-in-walk %.0f pairs %.0f aabb-calls %.0f callbacks %.1f (connected %.1f refused %.1f) narrow %.1f hits %.1f contacts %.1f joints %.1f bodies %.1f islands %.1f island-bodies %.1f island-joints %.1f rows %.1f largest-island %u",
        c(NPGC_GEOMS), c(NPGC_PAIRS), c(NPGC_AABB), c(NPGC_CALLBACK), c(NPGC_CB_CONNECTED), c(NPGC_CB_NOCOLL), c(NPGC_NARROW), c(NPGC_NARROW_HIT), c(NPGC_CONTACTS), c(NPGC_JOINTS), c(NPGC_BODIES), c(NPGC_ISLANDS),
        c(NPGC_ISL_BODIES), c(NPGC_ISL_JOINTS), c(NPGC_ISL_ROWS), g_npGsCnt[NPGC_ISL_MAXBODIES]);
    if (NpGsPhysCensus) { m += snprintf(d + m, sizeof d - m, " | census: "); m += NpGsPhysCensus(d + m, (int)sizeof d - m - 4); }
    snprintf(d + m, sizeof d - m, "\n");
    fputs(d, stderr);
    char e[2200];
    int q = snprintf(e, sizeof e, "[gsplit-ai] ms per STEP: ai %.2f (teampre %.2f misc %.2f team-timers %.2f intercept %.2f team-ai %.2f team-rest %.2f game-ctx %.2f | fielder: self %.2f script %.2f vm-states %.2f action %.2f head %.2f anim %.2f ctrl %.2f | goalie %.2f ball %.2f world %.2f self %.2f) counts per step: script updates %.1f vm state-machine updates %.1f fielders %.1f | events %.2f per step (dispatch %.2f peach %.2f snapshot %.2f self %.2f) npc %.2f prephys %.2f pose %.2f clock %.2f",
        ai * pics / steps, st(NPGS_AI_TEAMPRE, NPGS_AI_TEAMPRE), st(NPGS_AI_MISC, NPGS_AI_MISC), st(NPGS_AI_TEAMTIMERS, NPGS_AI_TEAMTIMERS), st(NPGS_AI_INTERCEPT, NPGS_AI_INTERCEPT), st(NPGS_AI_TEAMAI, NPGS_AI_TEAMAI), st(NPGS_AI_TEAMREST, NPGS_AI_TEAMREST),
        st(NPGS_AI_GAMECTX, NPGS_AI_GAMECTX), st(NPGS_AI_F_SELF, NPGS_AI_F_SELF), st(NPGS_AI_F_SCRIPT, NPGS_AI_F_SCRIPT), st(NPGS_AI_F_VMSTATE, NPGS_AI_F_VMSTATE), st(NPGS_AI_F_ACTION, NPGS_AI_F_ACTION), st(NPGS_AI_F_HEAD, NPGS_AI_F_HEAD),
        st(NPGS_AI_F_ANIM, NPGS_AI_F_ANIM), st(NPGS_AI_F_CTRL, NPGS_AI_F_CTRL), st(NPGS_AI_GOALIE, NPGS_AI_GOALIE), st(NPGS_AI_BALL, NPGS_AI_BALL), st(NPGS_AI_WORLD, NPGS_AI_WORLD), st(NPGS_SIM_AI, NPGS_SIM_AI),
        c(NPGC_AI_SCRIPTS), c(NPGC_AI_VMSTATES), c(NPGC_AI_FIELDERS), ev * pics / steps, st(NPGS_EV_DISPATCH, NPGS_EV_DISPATCH), st(NPGS_EV_PEACH, NPGS_EV_PEACH), st(NPGS_EV_SNAPSHOT, NPGS_EV_SNAPSHOT), st(NPGS_SIM_EVENTS, NPGS_SIM_EVENTS),
        st(NPGS_SIM_NPC, NPGS_SIM_NPC), st(NPGS_SIM_PREPHYS, NPGS_SIM_PREPHYS), st(NPGS_SIM_POSE, NPGS_SIM_POSE), st(NPGS_SIM_CLOCK, NPGS_SIM_CLOCK));
    q += snprintf(e + q, sizeof e - q, " || per PICTURE: begin-frame %.2f (pace %.2f files %.2f presentation %.2f glbegin %.2f chars %.2f camera %.2f setup %.2f) front-end %.2f (input %.2f presentation %.2f draw %.2f) | blend %.2f (posecull %.2f chars+powerups %.2f) posed %.1f culled %.1f characters a picture, chars draw %.2f (characters %.2f powerups %.2f) for %.1f characters a picture\n",
        grp(NPGS_BF_PACE, NPGS_BF_SETUP), grp(NPGS_BF_PACE, NPGS_BF_PACE), grp(NPGS_BF_FILES, NPGS_BF_FILES), grp(NPGS_BF_PRES, NPGS_BF_PRES), grp(NPGS_BF_GLBEGIN, NPGS_BF_GLBEGIN), grp(NPGS_BF_CHARS, NPGS_BF_CHARS), grp(NPGS_BF_CAMERA, NPGS_BF_CAMERA), grp(NPGS_BF_SETUP, NPGS_BF_SETUP),
        grp(NPGS_FE, NPGS_FE) + grp(NPGS_FE_INPUT, NPGS_FE_DRAW), grp(NPGS_FE_INPUT, NPGS_FE_INPUT), grp(NPGS_FE_PRES, NPGS_FE_PRES), grp(NPGS_FE_DRAW, NPGS_FE_DRAW),
        grp(NPGS_R_BLEND, NPGS_R_BLEND) + grp(NPGS_RB_POSE, NPGS_RB_CHARS), grp(NPGS_RB_POSE, NPGS_RB_POSE), grp(NPGS_RB_CHARS, NPGS_RB_CHARS), (double)g_npGsCnt[NPGC_BLEND_CHARS] / pics, (double)g_npGsCnt[NPGC_BLEND_ITEMS] / pics,
        grp(NPGS_R_CHARS, NPGS_R_CHARS) + grp(NPGS_RC_CHARS, NPGS_RC_POWERUPS), grp(NPGS_RC_CHARS, NPGS_RC_CHARS), grp(NPGS_RC_POWERUPS, NPGS_RC_POWERUPS), (double)g_npGsCnt[NPGC_DRAW_CHARS] / pics);
    fputs(e, stderr);
    {   // gt27: [gsplit-rest] the game thread outside the step and the render task, ms per picture (self times; 'self' = the parent's unmarked rest)
        char r[2600];
        double tasksOther = 0;   // the named tasks' own time other than the ones itemised above
        for (int i = NPGS_TASK0; i < NPGS_TASK0 + g_nTasks; ++i) {
            const char* tn = g_taskName[i - NPGS_TASK0];
            if (tn && (!strcmp(tn, "Begin Frame") || !strcmp(tn, "Audio"))) continue;
            tasksOther += (double)g_acc[i] / 1000.0 / pics;
        }
        double taskAudio = 0, taskBf = 0;
        for (int i = NPGS_TASK0; i < NPGS_TASK0 + g_nTasks; ++i) {
            const char* tn = g_taskName[i - NPGS_TASK0];
            if (tn && !strcmp(tn, "Audio")) taskAudio += (double)g_acc[i] / 1000.0 / pics;
            if (tn && !strcmp(tn, "Begin Frame")) taskBf += (double)g_acc[i] / 1000.0 / pics;
        }
        const double bf = taskBf + grp(NPGS_BF_PACE, NPGS_BF_SETUP) + grp(NPGS_BF_FPS, NPGS_BF_PROBE);
        const double fe = grp(NPGS_FE, NPGS_FE) + grp(NPGS_FE_INPUT, NPGS_FE_DRAW) + grp(NPGS_FE_LIGHT, NPGS_FE_REST);
        const double ef = grp(NPGS_E_WARBLE, NPGS_E_SKIN) + grp(NPGS_E_STILL, NPGS_E_SENDTAIL);
        const double run = grp(NPGS_RUNNER, NPGS_RUNNER) + grp(NPGS_RUN_TICKER, NPGS_RUN_MOVIE);
        const double rest = simPre + grp(NPGS_SIM_PAD, NPGS_SIM_PAD) + bf + fe + ef + run + grp(NPGS_P_UPDATE, NPGS_P_RENDER) + taskAudio + grp(NPGS_AUDIO, NPGS_AUDIO) + grp(NPGS_AUDIO_MIX, NPGS_AUDIO_MIX)
                            + grp(NPGS_DVD, NPGS_DVD) + tasksOther + grp(NPGS_OUTSIDE, NPGS_OUTSIDE) + grp(NPGS_MEM, NPGS_MEM) + grp(NPGS_LOG, NPGS_LOG);
        int k = snprintf(r, sizeof r, "[gsplit-rest] ms per picture, work outside the steps and the render task: %.2f = sim-pre %.2f (self %.2f input %.2f loop %.2f edge %.2f padctl %.2f lan %.2f tail %.2f) pad %.2f"
            " | begin-frame %.2f (self %.2f pace %.2f fps %.2f probe %.2f files %.2f presentation %.2f glbegin %.2f chars %.2f camera %.2f setup %.2f)"
            " | particles %.2f (update %.2f render %.2f)"
            " | front-end %.2f (self %.2f input %.2f presentation %.2f draw %.2f = self %.2f light %.2f update %.2f res %.2f scene-upd %.2f scene-render %.2f models %.2f rest %.2f)"
            " | end-frame %.2f (warble %.2f still %.2f send-self %.2f skin %.2f joinwork %.2f snapviews %.2f atfork %.2f pkt-refresh %.2f fork %.2f hook %.2f send-tail %.2f tail %.2f)"
            " | runner %.2f (self %.2f ticker %.2f hooks %.2f movie %.2f) audio %.2f (task %.2f mix %.2f) dvd %.2f other-tasks %.2f outside %.2f"
            " | mem %.2f (%.1f allocs %.1f frees %.0f bytes a picture; by parent:",
            rest, simPre, grp(NPGS_SIM_PRE, NPGS_SIM_PRE), grp(NPGS_SP_INPUT, NPGS_SP_INPUT), grp(NPGS_SP_LOOP, NPGS_SP_LOOP), grp(NPGS_SP_EDGE, NPGS_SP_EDGE), grp(NPGS_SP_PADCTL, NPGS_SP_PADCTL), grp(NPGS_SP_LAN, NPGS_SP_LAN), grp(NPGS_SP_TAIL, NPGS_SP_TAIL), grp(NPGS_SIM_PAD, NPGS_SIM_PAD),
            bf, taskBf, grp(NPGS_BF_PACE, NPGS_BF_PACE), grp(NPGS_BF_FPS, NPGS_BF_FPS), grp(NPGS_BF_PROBE, NPGS_BF_PROBE), grp(NPGS_BF_FILES, NPGS_BF_FILES), grp(NPGS_BF_PRES, NPGS_BF_PRES), grp(NPGS_BF_GLBEGIN, NPGS_BF_GLBEGIN),
            grp(NPGS_BF_CHARS, NPGS_BF_CHARS), grp(NPGS_BF_CAMERA, NPGS_BF_CAMERA), grp(NPGS_BF_SETUP, NPGS_BF_SETUP),
            grp(NPGS_P_UPDATE, NPGS_P_RENDER), grp(NPGS_P_UPDATE, NPGS_P_UPDATE), grp(NPGS_P_RENDER, NPGS_P_RENDER),
            fe, grp(NPGS_FE, NPGS_FE), grp(NPGS_FE_INPUT, NPGS_FE_INPUT), grp(NPGS_FE_PRES, NPGS_FE_PRES), grp(NPGS_FE_DRAW, NPGS_FE_DRAW) + grp(NPGS_FE_LIGHT, NPGS_FE_REST), grp(NPGS_FE_DRAW, NPGS_FE_DRAW),
            grp(NPGS_FE_LIGHT, NPGS_FE_LIGHT), grp(NPGS_FE_UPDATE, NPGS_FE_UPDATE), grp(NPGS_FE_RES, NPGS_FE_RES), grp(NPGS_FE_SCUPD, NPGS_FE_SCUPD), grp(NPGS_FE_SCRENDER, NPGS_FE_SCRENDER), grp(NPGS_FE_MODELS, NPGS_FE_MODELS), grp(NPGS_FE_REST, NPGS_FE_REST),
            ef, grp(NPGS_E_WARBLE, NPGS_E_WARBLE), grp(NPGS_E_STILL, NPGS_E_STILL), grp(NPGS_E_SEND, NPGS_E_SEND), grp(NPGS_E_SKIN, NPGS_E_SKIN), grp(NPGS_E_JOINWORK, NPGS_E_JOINWORK), grp(NPGS_E_SNAPVIEWS, NPGS_E_SNAPVIEWS),
            grp(NPGS_E_ATFORK, NPGS_E_ATFORK), grp(NPGS_E_PKTREFRESH, NPGS_E_PKTREFRESH), grp(NPGS_E_FORK, NPGS_E_FORK), grp(NPGS_E_HOOK, NPGS_E_HOOK), grp(NPGS_E_SENDTAIL, NPGS_E_SENDTAIL), grp(NPGS_E_TAIL, NPGS_E_TAIL),
            run, grp(NPGS_RUNNER, NPGS_RUNNER), grp(NPGS_RUN_TICKER, NPGS_RUN_TICKER), grp(NPGS_RUN_HOOKS, NPGS_RUN_HOOKS), grp(NPGS_RUN_MOVIE, NPGS_RUN_MOVIE),
            taskAudio + grp(NPGS_AUDIO, NPGS_AUDIO) + grp(NPGS_AUDIO_MIX, NPGS_AUDIO_MIX), taskAudio + grp(NPGS_AUDIO, NPGS_AUDIO), grp(NPGS_AUDIO_MIX, NPGS_AUDIO_MIX), grp(NPGS_DVD, NPGS_DVD), tasksOther, grp(NPGS_OUTSIDE, NPGS_OUTSIDE),
            grp(NPGS_MEM, NPGS_MEM), (double)g_npGsCnt[NPGC_MEM_ALLOC] / pics, (double)g_npGsCnt[NPGC_MEM_FREE] / pics, (double)g_npGsCnt[NPGC_MEM_BYTES] / pics);
        int ord2[NPGS_MAX];
        for (int i = 0; i < NPGS_MAX; ++i) ord2[i] = i;
        for (int i = 0; i < 6; ++i) for (int j = i + 1; j < NPGS_MAX; ++j) if (g_memParent[ord2[j]] > g_memParent[ord2[i]]) { int t = ord2[i]; ord2[i] = ord2[j]; ord2[j] = t; }
        for (int i = 0; i < 6 && g_memParent[ord2[i]] > 0 && k < (int)sizeof r - 300; ++i) k += snprintf(r + k, sizeof r - k, " %s %.2f", Name(ord2[i]), (double)g_memParent[ord2[i]] / 1000.0 / pics);
        double marks = (double)g_marks / pics;
        k += snprintf(r + k, sizeof r - k, ") | log %.2f (%.1f lines %.0f bytes a picture) | waits %.2f (swap %.2f join %.2f handover %.2f pace %.2f padlate %.2f gate %.2f other %.2f) | markers %.0f clock reads a picture at %.2f us = %.2f ms of the line is the split's own cost (estimate)\n",
            grp(NPGS_LOG, NPGS_LOG), (double)g_npGsCnt[NPGC_LOG_LINES] / pics, (double)g_npGsCnt[NPGC_LOG_BYTES] / pics,
            sumWait, grp(NPGS_WAIT_SWAP, NPGS_WAIT_SWAP), grp(NPGS_WAIT_JOIN, NPGS_WAIT_JOIN), grp(NPGS_WAIT_HANDOVER, NPGS_WAIT_HANDOVER), grp(NPGS_WAIT_PACE, NPGS_WAIT_PACE),
            grp(NPGS_WAIT_PADLATE, NPGS_WAIT_PADLATE), grp(NPGS_WAIT_GATE, NPGS_WAIT_GATE), grp(NPGS_WAIT_OTHER, NPGS_WAIT_OTHER), marks, g_clockUs, marks * g_clockUs / 1000.0);
        fputs(r, stderr);
        // [gsplit-pic]: by steps in the picture
        char pl[1400];
        int w = snprintf(pl, sizeof pl, "[gsplit-pic] work ms a picture by steps in it (the 60 fps question is the 1-step row; waits left out):");
        for (int c = 0; c < kPicClasses; ++c) {
            const unsigned nn = g_picN[c];
            if (!nn) { w += snprintf(pl + w, sizeof pl - w, " | %s steps: none", c == 0 ? "0" : c == 1 ? "1" : c == 2 ? "2" : "3+"); continue; }
            float* a = g_picWork[c];
            const unsigned m = nn < (unsigned)kEvery ? nn : (unsigned)kEvery;
            for (unsigned i = 1; i < m; ++i) { float v = a[i]; unsigned j = i; while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; --j; } a[j] = v; }
            const float p50 = a[m / 2], p95 = a[(m * 95) / 100 < m ? (m * 95) / 100 : m - 1], mx = a[m - 1];
            float* an = g_picNet[c];
            for (unsigned i = 1; i < m; ++i) { float v = an[i]; unsigned j = i; while (j > 0 && an[j - 1] > v) { an[j] = an[j - 1]; --j; } an[j] = v; }
            const float n50 = an[m / 2], n95 = an[(m * 95) / 100 < m ? (m * 95) / 100 : m - 1];
            const double* sm = g_picSum[c];
            w += snprintf(pl + w, sizeof pl - w, " | %s steps: %u pictures, p50 %.2f p95 %.2f max %.2f (net of the split's own clock reads: p50 %.2f p95 %.2f); mean sim %.2f render-task %.2f particles %.2f end-frame %.2f rest %.2f (waits %.2f)",
                          c == 0 ? "0" : c == 1 ? "1" : c == 2 ? "2" : "3+", nn, p50, p95, mx, n50, n95, sm[0] / nn, sm[1] / nn, sm[2] / nn, sm[3] / nn, sm[4] / nn, sm[5] / nn);
        }
        snprintf(pl + w, sizeof pl - w, "\n");
        fputs(pl, stderr);
        if (g_grpN >= 20) {   // [gsplit-p95]: the 1-step pictures' slowest 5 % (by net work) against all of them, per group: where the tail comes from
            const unsigned n = g_grpN;
            unsigned idx[kEvery];
            for (unsigned i = 0; i < n; ++i) idx[i] = i;
            for (unsigned i = 1; i < n; ++i) { unsigned v = idx[i]; unsigned j = i; while (j > 0 && g_grpNet[idx[j - 1]] < g_grpNet[v]) { idx[j] = idx[j - 1]; --j; } idx[j] = v; }
            const unsigned top = n / 20 > 0 ? n / 20 : 1;
            double all[kGroups] = {0}, slow[kGroups] = {0}, allNet = 0, slowNet = 0;
            for (unsigned i = 0; i < n; ++i) { for (int k = 0; k < kGroups; ++k) all[k] += g_grp[i][k]; allNet += g_grpNet[i]; }
            for (unsigned t = 0; t < top; ++t) { for (int k = 0; k < kGroups; ++k) slow[k] += g_grp[idx[t]][k]; slowNet += g_grpNet[idx[t]]; }
            int ord[kGroups];
            for (int k = 0; k < kGroups; ++k) ord[k] = k;
            for (int i = 0; i < kGroups; ++i) for (int j = i + 1; j < kGroups; ++j) if (slow[ord[j]] / top - all[ord[j]] / n > slow[ord[i]] / top - all[ord[i]] / n) { int t2 = ord[i]; ord[i] = ord[j]; ord[j] = t2; }
            char q[1400];
            int u = snprintf(q, sizeof q, "[gsplit-p95] 1-step pictures: the slowest %u of %u (net work %.2f ms) against all (%.2f ms), by group, largest excess first [slow-mean / mean]:", top, n, slowNet / top, allNet / n);
            for (int k = 0; k < kGroups && u < (int)sizeof q - 60; ++k)
                u += snprintf(q + u, sizeof q - u, " %s %+.2f [%.2f / %.2f]", kGroupName[ord[k]], slow[ord[k]] / top - all[ord[k]] / n, slow[ord[k]] / top, all[ord[k]] / n);
            snprintf(q + u, sizeof q - u, "\n");
            fputs(q, stderr);
        }
        g_grpN = 0;
        memset(g_picN, 0, sizeof g_picN);
        memset(g_picSum, 0, sizeof g_picSum);
        memset(g_memParent, 0, sizeof g_memParent);
        g_marks = 0;
    }
    if (g_kindPics[1] + g_kindPics[2] > 0) {
        // per picture of each kind (work phases only, the waits left out), host CPU ms or console wall ms as the line above: the balance of the pair
        char b[1200];
        int w = snprintf(b, sizeof b, "[gsplit-bg30] ms per picture, by kind (pictures: full %u light %u ordinary %u):", g_kindPics[1], g_kindPics[2], g_kindPics[0]);
        for (int k = 1; k <= 2; ++k) {
            if (!g_kindPics[k]) continue;
            const double pk = (double)g_kindPics[k];
            auto kg = [&](int a, int bb) { double sm = 0; for (int i = a; i <= bb; ++i) sm += (double)g_kind[k][i] / 1000.0 / pk; return sm; };
            double work = 0;
            for (int i = 0; i < NPGS_MAX; ++i) if (!IsWait(i)) work += (double)g_kind[k][i] / 1000.0 / pk;
            double tasks = 0;
            for (int i = NPGS_TASK0; i < NPGS_TASK0 + g_nTasks; ++i) tasks += (double)g_kind[k][i] / 1000.0 / pk;
            w += snprintf(b + w, sizeof b - w, " | %s work %.2f (sim %.2f, crowd pos %.2f spr %.2f draw %.2f, stadium %.2f, blend %.2f chars %.2f, particles %.2f, hud %.2f, shadow %.2f, send %.2f, begin-frame+tasks %.2f)",
                          k == 1 ? "FULL" : "LIGHT", work, kg(NPGS_SIM_PRE, NPGS_SIM_PAD) + kg(NPGS_PH_PRECOLLIDE, NPGS_AI_WORLD), kg(NPGS_R_CROWDPOS, NPGS_R_CROWDPOS), kg(NPGS_R_CROWDSPR, NPGS_R_CROWDSPR),
                          kg(NPGS_R_CROWDDRAW, NPGS_R_CROWDDRAW), kg(NPGS_R_STADIUM, NPGS_R_STADIUM), kg(NPGS_R_BLEND, NPGS_R_BLEND) + kg(NPGS_RB_POSE, NPGS_RB_CHARS), kg(NPGS_R_CHARS, NPGS_R_CHARS) + kg(NPGS_RC_CHARS, NPGS_RC_POWERUPS),
                          kg(NPGS_P_UPDATE, NPGS_P_RENDER), kg(NPGS_R_HUD, NPGS_R_HUD), kg(NPGS_R_SHADOW, NPGS_R_SHADOW), kg(NPGS_E_WARBLE, NPGS_E_SKIN), tasks);
        }
        snprintf(b + w, sizeof b - w, "\n");
        fputs(b, stderr);
    }
    memset(g_kind, 0, sizeof g_kind);
    memset(g_kindPics, 0, sizeof g_kindPics);

    memset(g_acc, 0, sizeof g_acc);
    memset(g_cnt, 0, sizeof g_cnt);
    memset(g_npGsCnt, 0, sizeof g_npGsCnt);
    for (int i = 0; i < NPGS_MAX; ++i) g_samples[i] = 0;
    g_sampleTotal = 0;
    g_t0 = now;
    g_cpu0 = cpu;
}
}  // namespace

extern "C" {
void (*g_npGsMarkHook)(int) = 0;   // mapi17 test hook (NpSimOrder17.cpp, MSC_SIMT_ORDER=I): called on every phase mark of any thread (it filters itself)
int NpGsEnter(int id) {
    if (g_npGsMarkHook && (id < NPGS_WAIT_PADLATE || id >= NPGS_TASK0)) g_npGsMarkHook(id);   // (gt27's ids are not shown to the mapi17 / simt test hooks: their schedules stay as proved)
    if (!g_haveOwner || !pthread_equal(pthread_self(), g_owner)) return -1;
    if (!g_detail && IsDetail(id)) return -1;
    Charge(g_cur);
    const int prev = g_cur;
    if (id == NPGS_MEM && prev != NPGS_MEM) { g_memParentCur = prev; g_memT0 = g_last; }
    g_cur = id;
    g_phase = id;
    ++g_cnt[id];
    ++g_marks;
    return prev;
}
int NpGsEnterIf(int id) { return g_npGsOn ? NpGsEnter(id) : -1; }   // gt27: for the files built without NP_NATIVE (np/gsplit.h NpGsSeqW)
void NpGsLeaveIf(int prev) { if (prev >= 0) NpGsLeave(prev); }
void NpGsCountIf(int counter, unsigned n) { if (g_npGsOn && counter >= 0 && counter < 32 && g_haveOwner && pthread_equal(pthread_self(), g_owner)) g_npGsCnt[counter] += n; }
void NpGsLeave(int prev) {
    if (prev < 0 || !pthread_equal(pthread_self(), g_owner)) return;
    Charge(g_cur);
    ++g_marks;
    if (g_cur == NPGS_MEM && prev != NPGS_MEM && g_memParentCur >= 0 && g_memParentCur < NPGS_MAX) { g_memParent[g_memParentCur] += g_last - g_memT0; g_memParentCur = -1; }
    g_cur = prev;
    g_phase = prev;
}
int NpGsTaskId(const char* name) {
    for (int i = 0; i < g_nTasks; ++i)
        if (g_taskName[i] == name || (name && g_taskName[i] && !strcmp(g_taskName[i], name))) return NPGS_TASK0 + i;
    if (g_nTasks < 32) { g_taskName[g_nTasks] = name ? name : "?"; return NPGS_TASK0 + g_nTasks++; }
    return NPGS_RUNNER;
}
int NpGsPairId(int kind, int c1, int c2) {
    const int key = kind * 10000 + c1 * 100 + c2;
    for (int i = 0; i < g_nPairs; ++i) if (g_pairKey[i] == key) return kPair0 + i;
    if (g_nPairs >= NPGS_MAX - kPair0) return NPGS_SIM_PHYSICS;
    const char* const cn1 = c1 >= 0 && c1 < 16 ? kClass[c1] : "?";
    const char* const cn2 = c2 >= 0 && c2 < 16 ? kClass[c2] : "?";
    const char* const kn = kind == 0 ? "narrow" : kind == 1 ? "preupd" : "postupd";
    if (kind == 0) snprintf(g_pairBuf[g_nPairs], sizeof g_pairBuf[0], "%s:%s-%s", kn, cn1, cn2);
    else snprintf(g_pairBuf[g_nPairs], sizeof g_pairBuf[0], "%s:%s", kn, cn1);
    g_pairKey[g_nPairs] = key;
    return kPair0 + g_nPairs++;
}
void NpGsPassBegin(void) {
    if (!g_inited) Init();
    if (!g_npGsOn || !pthread_equal(pthread_self(), g_owner)) return;
    Charge(NPGS_OUTSIDE);
    g_cur = NPGS_RUNNER;
    g_phase = NPGS_RUNNER;
    g_inPass = true;
    memcpy(g_snap, g_acc, sizeof g_snap);
    g_snapSteps = g_cnt[NPGS_SIM_AI];
    g_snapMarks = g_marks;
}
void NpGsPassEnd(void) {
    if (!g_npGsOn || !g_inPass || !pthread_equal(pthread_self(), g_owner)) return;
    Charge(g_cur);
    g_cur = NPGS_OUTSIDE;
    g_phase = NPGS_OUTSIDE;
    g_inPass = false;
    if (&NpBg30Mode != 0 && &NpBg30Phase != 0 && NpBg30Mode() == 2) {
        const int k = NpBg30Phase();
        ++g_kindPics[k];
        for (int i = 0; i < NPGS_MAX; ++i) g_kind[k][i] += g_acc[i] - g_snap[i];
    }
    {   // gt27: this picture into its steps class ([gsplit-pic])
        const uint32_t steps = g_cnt[NPGS_SIM_AI] - g_snapSteps;
        const int c = steps >= 3 ? 3 : (int)steps;
        double work = 0, wait = 0, simv = 0, rt = 0, pt = 0, ef = 0;
        for (int i = 0; i < NPGS_MAX; ++i) {
            const uint64_t d = g_acc[i] - g_snap[i];
            if (!d) continue;
            const double ms = (double)d / 1000.0;
            if (IsWait(i)) { wait += ms; continue; }
            work += ms;
            if ((i >= NPGS_SIM_PRE && i <= NPGS_SIM_PAD) || (i >= NPGS_PH_PRECOLLIDE && i <= NPGS_EV_SNAPSHOT) || (i >= NPGS_SP_INPUT && i <= NPGS_SP_LOOP) || i >= kPair0) simv += ms;
            else if ((i >= NPGS_R_PRE && i <= NPGS_R_NUMBERS) || (i >= NPGS_RB_POSE && i <= NPGS_RC_POWERUPS)) rt += ms;
            else if (i == NPGS_P_UPDATE || i == NPGS_P_RENDER) pt += ms;
            else if ((i >= NPGS_E_WARBLE && i <= NPGS_E_SKIN) || (i >= NPGS_E_STILL && i <= NPGS_E_SENDTAIL)) ef += ms;
        }
        const double own = (double)(g_marks - g_snapMarks) * g_clockUs / 1000.0;   // the split's clock reads of this picture
        if (g_picN[c] < (unsigned)kEvery) { g_picWork[c][g_picN[c]] = (float)work; g_picNet[c][g_picN[c]] = (float)(work - own); }
        if (c == 1 && g_grpN < (unsigned)kEvery) {
            if (g_bfTask == -2) { g_bfTask = -1; for (int i = 0; i < g_nTasks; ++i) if (g_taskName[i] && !strcmp(g_taskName[i], "Begin Frame")) g_bfTask = NPGS_TASK0 + i; }
            float* gv = g_grp[g_grpN];
            for (int k = 0; k < kGroups; ++k) gv[k] = 0;
            for (int i = 0; i < NPGS_MAX; ++i) {
                const uint64_t d = g_acc[i] - g_snap[i];
                if (d && !IsWait(i)) gv[GroupOf(i)] += (float)((double)d / 1000.0);
            }
            g_grpNet[g_grpN] = (float)(work - own);
            ++g_grpN;
        }
        ++g_picN[c];
        double* sm = g_picSum[c];
        sm[0] += simv; sm[1] += rt; sm[2] += pt; sm[3] += ef; sm[4] += work - simv - rt - pt - ef; sm[5] += wait;
    }
    if (++g_pics % kEvery == 0) Print();
    static const bool brk = [] { const char* e = getenv("MSC_GSPLIT_BREAK"); return e && *e && *e != '0'; }();   // test only: must change the determinism trace
    if (brk && g_pics == 3300) gNetworkRandomSeed ^= 0x5A5A5A5Au;
}
void NpGsWaitBegin(void) {
    if (!g_npGsOn || !pthread_equal(pthread_self(), g_owner)) return;
    if (g_waitDepth++ == 0) g_waitPrev = NpGsEnter(NPGS_WAIT_HANDOVER);
}
void NpGsWaitEnd(void) {
    if (!g_npGsOn || !pthread_equal(pthread_self(), g_owner)) return;
    if (g_waitDepth > 0 && --g_waitDepth == 0) NpGsLeave(g_waitPrev);
}
}
#endif
