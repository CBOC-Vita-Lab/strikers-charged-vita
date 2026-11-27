// NpSimThread.cpp (NP, wave 9 "simthread"): measurement probes for the question "can the simulation and the drawing overlap?".
// Not part of the decomp. Host builds only do anything (the probes are env-gated; on the Vita the hooks cost a load and a compare).
//
//   MSC_SIMT_PROBE=1           per task: does the task change the simulation's state (FixedUpdateTask::CalculateChecksum, the network
//                              random seed) or draw from a random-number generator? Counted per task name and per seed, plus the callers of
//                              every draw (backtrace, resolved afterwards with addr2line). Written at exit to MSC_SIMT_PROBE_OUT (default
//                              simt_probe.txt in the run folder).
//   MSC_SIMT_PERTURB=skip|twice  the drawing tasks (Game Render, Particle Update) are skipped on every odd picture, or run twice: if the
//                              simulation's det trace (NP_DET_TRACE) does not change, the simulation does not read what the drawing writes.
#include "Game/Task/FixedUpdateTask.h"
#include "NL/nlTask.h"
#include "Game/Game.h"
#include "Game/Ball.h"
#include "Game/Team.h"

#ifdef NP_NATIVE
#if defined(__linux__)
#include <execinfo.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned int gNetworkRandomSeed;
extern unsigned int nlDefaultSeed;
extern unsigned int gEffectsRandomSeed;

extern "C" int gNpSimtProbe;
extern "C" int gNpSimtPerturb;
int gNpSimtProbe = -1;    // -1 = not read yet
int gNpSimtPerturb = 0;   // 1 skip on odd pictures, 2 run twice

namespace
{
const int kMaxTasks = 32;
struct TaskRow
{
    const char* name;
    unsigned prio;
    unsigned runs;
    unsigned chkChanged;   // runs after which the simulation checksum differed from before the task
    unsigned netSeedMoved; // runs after which gNetworkRandomSeed differed
    unsigned defDraws;     // draws from nlDefaultSeed inside the task
    unsigned netDraws;     // draws from gNetworkRandomSeed inside the task
    unsigned othDraws;     // draws from other seeds
};
TaskRow sTasks[kMaxTasks];
int sNumTasks = 0;
int sCurTask = -1;
unsigned sStepDepth = 0;   // inside FixedUpdateTask::CallFixedUpdateTasks
unsigned sStepDef = 0, sStepNet = 0, sStepOth = 0;
unsigned sChkBefore = 0, sNetBefore = 0;
unsigned sPictures = 0;

const int kMaxSites = 4096;
struct Site
{
    void* ra[4];
    unsigned char seedKind;   // 0 network, 1 default, 2 other
    signed char task;         // task row, -1 none
    unsigned char inStep;
    unsigned count;
};
Site sSites[kMaxSites];
int sNumSites = 0;
unsigned sSiteOverflow = 0;

void Dump()
{
    const char* out = getenv("MSC_SIMT_PROBE_OUT");
    FILE* f = fopen(out != 0 && *out != 0 ? out : "simt_probe.txt", "w");
    if (f == 0)
    {
        return;
    }
    fprintf(f, "[simt] pictures %u; draws inside simulation steps: network %u default %u other %u\n", sPictures, sStepNet, sStepDef, sStepOth);
    for (int i = 0; i < sNumTasks; i++)
    {
        const TaskRow& t = sTasks[i];
        fprintf(f, "[simt] task %-22s prio %2u runs %7u chkChanged %6u netSeedMoved %6u draws(outside steps) net %7u def %7u oth %7u\n", t.name, t.prio,
            t.runs, t.chkChanged, t.netSeedMoved, t.netDraws, t.defDraws, t.othDraws);
    }
    for (int i = 0; i < sNumSites; i++)
    {
        const Site& s = sSites[i];
        fprintf(f, "[simt] site seed=%s task=%s step=%u count=%u ra %p %p %p %p\n", s.seedKind == 0 ? "net" : (s.seedKind == 1 ? "def" : "oth"),
            s.task >= 0 ? sTasks[(int)s.task].name : "-", s.inStep, s.count, s.ra[0], s.ra[1], s.ra[2], s.ra[3]);
    }
    fprintf(f, "[simt] site overflow %u\n", sSiteOverflow);
    fclose(f);
}

void Init()
{
    const char* e = getenv("MSC_SIMT_PROBE");
    gNpSimtProbe = (e != 0 && *e == '1') ? 1 : 0;
    const char* p = getenv("MSC_SIMT_PERTURB");
    gNpSimtPerturb = (p != 0 && strcmp(p, "skip") == 0) ? 1 : ((p != 0 && strcmp(p, "twice") == 0) ? 2 : 0);
    if (gNpSimtProbe)
    {
        atexit(Dump);
    }
    else if (gNpSimtPerturb)
    {
        gNpSimtProbe = 2;   // perturbation only: the task gate stays open, the counting stays off
    }
    if (gNpSimtProbe || gNpSimtPerturb)
    {
        fprintf(stderr, "[simt] probe=%d perturb=%d\n", gNpSimtProbe, gNpSimtPerturb);
    }
}

unsigned SafeChecksum()
{
    if (!GetFixedUpdateTask()->mSimulationStarted || g_pGame == 0 || g_pBall == 0 || g_pTeams[0] == 0 || g_pTeams[1] == 0)
    {
        return 0;
    }
    return GetFixedUpdateTask()->CalculateChecksum();
}

int TaskRowFor(const char* name, unsigned prio)
{
    for (int i = 0; i < sNumTasks; i++)
    {
        if (sTasks[i].name == name || strcmp(sTasks[i].name, name) == 0)
        {
            return i;
        }
    }
    if (sNumTasks >= kMaxTasks)
    {
        return kMaxTasks - 1;
    }
    TaskRow& t = sTasks[sNumTasks];
    memset(&t, 0, sizeof(t));
    t.name = name;
    t.prio = prio;
    return sNumTasks++;
}
}

extern "C" int NpSimtEnabled()
{
    if (gNpSimtProbe < 0)
    {
        Init();
    }
    return gNpSimtProbe == 1 ? 1 : 0;
}

extern "C" int NpSimtPerturbTask(const char* name)
{
    if (gNpSimtProbe < 0)
    {
        Init();
    }
    if (gNpSimtPerturb == 0)
    {
        return 0;
    }
    static const char* sList = 0;
    if (sList == 0)
    {
        const char* e = getenv("MSC_SIMT_PERTURB_TASKS");
        sList = (e != 0 && *e != 0) ? e : "Game Render,Particle Update";
    }
    const size_t n = strlen(name);
    for (const char* p = sList; (p = strstr(p, name)) != 0; p += n)
    {
        if ((p == sList || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
        {
            return gNpSimtPerturb;
        }
    }
    return 0;
}

// ---- wave 10 simthread10: the simulation must not depend on what the drawing does.
//
// MSC_RENDER_RNG=1 (default OFF in code and OFF in the r15 Vita plan; kit run renderrng1 is the ON side): the two shared generators the drawing and the simulation both drew from get a
// SIMULATION stream of their own. nlDefaultSeed (crowd, stadium effects, HUD, meter, sound volume/pitch of sounds started by the audio task) and
// gEffectsRandomSeed (particles) keep being the variables every draw site names; at the start of every simulation step they are swapped for the
// simulation's own copies and swapped back at the end of the step. So draws made inside a step (sounds started by the step, the meter rumble, the
// emitters an event creates) come from a stream that only steps advance, whatever the drawing did before; draws made by any other task (drawing,
// front end, audio, loading, menus) come from the render stream, which a step never touches. The sequence of cosmetic randomness differs from
// before (a different crowd jitter, sound pitch and particle spread); the simulation (gNetworkRandomSeed, the checksummed state) is unchanged.
// The simulation stream is re-seeded to a constant at the first step of a match (FixedUpdateTask mFrame == 0).
//
//
// MSC_RENDER_RNG=2 (default OFF; rng17): the same two streams, but the globals are NEVER swapped, so a second thread may draw the render stream while a step runs. nlRandom and
// nlSetRandomSeed (NL/nlRandom.cpp) ask NpRngSeedFor(seed) when gNpRngRedirect is set: inside a step the answer for &nlDefaultSeed / &gEffectsRandomSeed is the simulation's copy
// (sSimDef / sSimFx), anything else the pointer itself. "Inside a step": with the step on the game thread, sStepDepth > 0; with the step on its own thread, the caller's stack
// address lies in the range given to NpRngSetSimStack (that test alone decides then, so the game thread never sees the step's depth). The sequence of draws, the re-seed at
// mFrame == 0 and the [renderrng] counters equal mode 1's. MSC_RENDER_RNG_MUTANT=1 (proof only): the accessor answers the render stream inside a step.
//
// MSC_FRAMELOCK_STEP=1 (default OFF in code and OFF in the r15 Vita plan; kit run renderrng1 is the ON side): the frame-lock countdown (FixedUpdateTask::DecrementFrameLock, which ends the
// game's pause after a goal / kick-off hold and so decides on which picture the simulation resumes) moves from GameRenderTask::RenderFrame to the
// end of FixedUpdateTask::Run, the same picture, before the drawing, with the same conditions (a state the drawing task is active in, the home
// button menu not up, g_bRenderWorld). A picture whose drawing is skipped or doubled no longer changes when the simulation resumes.
extern "C" int gNpRngRedirect;
int gNpRngRedirect = 0;

namespace
{
int sRngMode = -1;
int sRngMutant = 0;
char* sSimStackLo = 0;
char* sSimStackHi = 0;
int sLockMode = -1;
int sVisMode = -1;
int sBlendMode = -1;
unsigned long sBlendEarly = 0, sBlendReused = 0;
unsigned long sVisSends = 0, sVisDiffers = 0, sVisSendsWithDiff = 0;
unsigned sSimDef = 0x2468ACE1u, sSimFx = 0x13579BDFu;   // the simulation's streams
unsigned sRenderDef = 0, sRenderFx = 0;                 // the render streams while a step runs
unsigned long sRngSteps = 0, sRngDefMoved = 0, sRngFxMoved = 0, sRngReseeds = 0;
unsigned long sLockMoves = 0, sLockSkippedInRender = 0;
unsigned sDefAtEnter = 0, sFxAtEnter = 0;

void ReadModes()
{
    const char* e = getenv("MSC_RENDER_RNG");
    sRngMode = (e != 0 && e[0] == '1') ? 1 : ((e != 0 && e[0] == '2') ? 2 : 0);
    const char* m = getenv("MSC_RENDER_RNG_MUTANT");
    sRngMutant = (m != 0 && m[0] == '1') ? 1 : 0;
    gNpRngRedirect = (sRngMode == 2) ? 1 : 0;
    const char* l = getenv("MSC_FRAMELOCK_STEP");
    sLockMode = (l != 0 && l[0] == '1') ? 1 : 0;
    const char* v = getenv("MSC_VIS_STEP");
    sVisMode = (v != 0 && v[0] == '1') ? 1 : 0;
    const char* b = getenv("MSC_BLEND_STEP");
    sBlendMode = (b != 0 && b[0] == '1') ? 1 : 0;
    fprintf(stderr, "[renderrng] MSC_RENDER_RNG=%d MSC_FRAMELOCK_STEP=%d MSC_VIS_STEP=%d MSC_BLEND_STEP=%d\n", sRngMode, sLockMode, sVisMode, sBlendMode);
}
}

extern "C" int NpFrameLockStepOn()
{
    if (sLockMode < 0)
    {
        ReadModes();
    }
    return sLockMode;
}

// MSC_VIS_STEP=1 (default OFF in code and OFF in the r15 Vita plan; kit run renderrng1 is the ON side): the on-screen flags the simulation takes every 10 steps (cGame::SendPlayerVisibility, the
// camera's one way into the simulation) are made from the simulation's own player positions projected with the picture's camera, in the step, instead of
// from m_v3ScreenPosition written by the drawing task (Indicators.cpp: the projection of the blended position of the last picture drawn). Found by the
// perturbation test: with Game Render skipped on odd pictures the det trace moved at step 2121 (m_DetChar.m_bOnScreen is checksummed).
extern "C" int NpVisStepOn()
{
    if (sVisMode < 0)
    {
        ReadModes();
    }
    return sVisMode;
}

extern "C" void NpVisStepCount(int differs)
{
    ++sVisSends;
    sVisDiffers += (unsigned long)differs;
    if (differs) ++sVisSendsWithDiff;
    if ((sVisSends % 100) == 1)
    {
        fprintf(stderr, "[visstep] flags made in the step: %lu sends; players whose flag would have differed from the drawing task's projection: %lu (in %lu sends)\n",
            sVisSends, sVisDiffers, sVisSendsWithDiff);
    }
}

// MSC_BLEND_STEP=1 (default OFF in code and OFF in the r15 Vita plan; kit run renderrng1 is the ON side): the snapshot blend (ReplayManager mRender) is made by the simulation task each gameplay picture
// (NpBlendEarly, end of FixedUpdateTask::Run) and the drawing only draws it. The gameplay camera aims at that blend and the simulation takes the camera's
// on-screen flags (see MSC_VIS_STEP), so a picture whose drawing is skipped must not leave the blend stale. Same blend, same factor, same inputs: the pictures do not change.
extern "C" int NpBlendStepOn()
{
    if (sBlendMode < 0)
    {
        ReadModes();
    }
    return sBlendMode;
}

extern "C" void NpBlendStepCount(int made)
{
    if (made)
    {
        ++sBlendEarly;
    }
    else
    {
        ++sBlendReused;
    }
    if (made && (sBlendEarly % 1000) == 1)
    {
        fprintf(stderr, "[blendstep] blend made by the simulation task: %lu pictures, found done by the drawing task: %lu\n", sBlendEarly, sBlendReused);
    }
}

// called from FixedUpdateTask::Run (moved = 1) and from GameRenderTask::RenderFrame (moved = 0: the old place, skipped when the switch is on)
extern "C" void NpFrameLockCount(int moved)
{
    if (moved)
    {
        ++sLockMoves;
    }
    else
    {
        ++sLockSkippedInRender;
    }
    if (moved && (sLockMoves % 600) == 1)
    {
        fprintf(stderr, "[framelock] countdown made by the simulation task: %lu pictures (the drawing task skipped it: %lu)\n", sLockMoves, sLockSkippedInRender);
    }
}

// the step's own thread tells which stack it runs on (block17/overlap18); until then the step is on the game thread and sStepDepth decides
extern "C" void NpRngSetSimStack(void* lo, void* hi)
{
    sSimStackLo = (char*)lo;
    sSimStackHi = (char*)hi;
}

extern "C" unsigned int* NpRngSeedFor(unsigned int* seed)
{
    if (sRngMode != 2 || (seed != &nlDefaultSeed && seed != &gEffectsRandomSeed))
    {
        return seed;
    }
    if (sSimStackHi != 0)
    {
        char here;
        if (&here < sSimStackLo || &here >= sSimStackHi)
        {
            return seed;
        }
    }
    else if (sStepDepth == 0)
    {
        return seed;
    }
    if (sRngMutant)
    {
        return seed;
    }
    return seed == &nlDefaultSeed ? &sSimDef : &sSimFx;
}

extern "C" void NpSimtStep(int begin)
{
    if (sRngMode < 0)
    {
        ReadModes();
    }
    if (begin)
    {
        if (++sStepDepth == 1 && sRngMode >= 1)
        {
            if (GetFixedUpdateTask()->mFrame == 0)
            {
                sSimDef = 0x2468ACE1u;
                sSimFx = 0x13579BDFu;
                ++sRngReseeds;
            }
            if (sRngMode == 1)
            {
                sRenderDef = nlDefaultSeed;
                sRenderFx = gEffectsRandomSeed;
                nlDefaultSeed = sSimDef;
                gEffectsRandomSeed = sSimFx;
            }
            sDefAtEnter = sSimDef;
            sFxAtEnter = sSimFx;
        }
    }
    else if (sStepDepth > 0)
    {
        if (--sStepDepth == 0 && sRngMode >= 1)
        {
            if (sRngMode == 1)
            {
                sSimDef = nlDefaultSeed;
                sSimFx = gEffectsRandomSeed;
                nlDefaultSeed = sRenderDef;
                gEffectsRandomSeed = sRenderFx;
            }
            ++sRngSteps;
            if (sSimDef != sDefAtEnter) ++sRngDefMoved;
            if (sSimFx != sFxAtEnter) ++sRngFxMoved;
            if ((sRngSteps % 2000) == 1)
            {
                fprintf(stderr, "[renderrng] simulation stream used in %lu steps: default stream moved in %lu, effects stream in %lu (re-seeded %lu times)\n",
                    sRngSteps, sRngDefMoved, sRngFxMoved, sRngReseeds);
            }
        }
    }
}

extern "C" void NpSimtTaskBegin(const char* name, unsigned prio)
{
    if (NpSimtEnabled() <= 0)
    {
        return;
    }
    sCurTask = TaskRowFor(name, prio);
    if (prio == 4)   // Begin Frame: a new picture
    {
        if (++sPictures % 3000 == 0)
        {
            Dump();   // (the host harness ends the process with _exit: the table is rewritten every 3000 pictures)
        }
    }
    sChkBefore = SafeChecksum();
    sNetBefore = gNetworkRandomSeed;
}

extern "C" void NpSimtTaskEnd()
{
    if (NpSimtEnabled() <= 0 || sCurTask < 0)
    {
        return;
    }
    TaskRow& t = sTasks[sCurTask];
    ++t.runs;
    if ((SafeChecksum()) != sChkBefore)
    {
        ++t.chkChanged;
    }
    if (gNetworkRandomSeed != sNetBefore)
    {
        ++t.netSeedMoved;
    }
    sCurTask = -1;
}

extern "C" void NpSimtRandom(const unsigned int* seed)
{
    if (gNpSimtProbe != 1)
    {
        return;
    }
    const unsigned char kind = seed == &gNetworkRandomSeed ? 0 : (seed == &nlDefaultSeed ? 1 : 2);
    if (sStepDepth > 0)
    {
        if (kind == 0) ++sStepNet; else if (kind == 1) ++sStepDef; else ++sStepOth;
    }
    else if (sCurTask >= 0)
    {
        TaskRow& t = sTasks[sCurTask];
        if (kind == 0) ++t.netDraws; else if (kind == 1) ++t.defDraws; else ++t.othDraws;
    }
    void* frames[6] = {0, 0, 0, 0, 0, 0};
#if defined(__linux__)
    backtrace(frames, 6);
#endif
    // frames[0] = here, [1] = nlRandom, [2..5] = callers
    void* ra[4] = {frames[2], frames[3], frames[4], frames[5]};
    const unsigned char inStep = sStepDepth > 0 ? 1 : 0;
    for (int i = 0; i < sNumSites; i++)
    {
        Site& s = sSites[i];
        if (s.seedKind == kind && s.inStep == inStep && s.task == sCurTask && memcmp(s.ra, ra, sizeof(ra)) == 0)
        {
            ++s.count;
            return;
        }
    }
    if (sNumSites >= kMaxSites)
    {
        ++sSiteOverflow;
        return;
    }
    Site& s = sSites[sNumSites++];
    memcpy(s.ra, ra, sizeof(ra));
    s.seedKind = kind;
    s.task = (signed char)sCurTask;
    s.inStep = inStep;
    s.count = 1;
}
#endif
