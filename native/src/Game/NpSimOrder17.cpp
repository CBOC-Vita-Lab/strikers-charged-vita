// NpSimOrder17.cpp (NP, wave 17 "mapi17"): the draw window of plan-simthread17 (world NPC draw + the crowd block of GameRenderTask::RenderFrame) and the test that proves it
// can run before the step, or between its phases, without changing the simulation or the pictures. See include/np/simorder17.h and w8/mapi17.md.
//
//   MSC_SIMT_ORDER=A|B|I|P A = default, today's order (nothing here changes it: every stage runs at its old place in RenderFrame). B = the six stages run in FixedUpdateTask::Run
//                          before the step loop, RenderFrame then finds them done. I = test: every stage starts at a seeded phase mark inside the step (NP_GS marks, so
//                          MSC_GSPLIT=1 is needed on the host), or before the step (rank 0), or after it (rank past the last mark: RenderFrame runs it).
//                          P = test reference: all six stages run after the step but before the picture's snapshot blend (at the old world NPC place): the same information as B
//                          has, plus the step's results. B == P proves the window does not read what the step writes. (A differs from P only by what the blend writes: the crowd's
//                          animation update in UpdateStadium, see w8/mapi17.md.)
//   MSC_SIMT_SCHED=<n>     seed of the interleave (mode I), per picture: seed ^ picture number.
//   MSC_SIMT_ORDER_SEED=1  the window draws from random streams of its own (swapped in around every stage) in every order, so a picture of A can be compared with one of B
//                          (the shared stream's draws would otherwise fall in a different order). Without it the window uses the shared streams as today.
//   MSC_SIMT=2             (overlap18, np/simrun17.h) the window of order B is drawn by the game thread WHILE the step runs on np_sim: NpSimOrder17Early arms the picture (everything the window
//                          needs from simulation state is read here, before the kick: the stage conditions, the captain flag, the arming conditions) and plans the stages; NpSimtRun starts the step
//                          thread and calls NpSimOrder17RunPending, which runs the planned stages on the game thread. A picture without a step runs them right after the step block
//                          (FixedUpdateTask::Run). The window draws from the render random stream (MSC_RENDER_RNG=2; MSC_SIMT_ORDER_SEED is ignored). Equal to order B by proof (w8/overlap18.md).
//   MSC_SIMT_MUTANT=3      test probe (not a mutant in order A/B/P: all agree): stage 4 draws one number from each of the two streams (nlDefaultSeed, gEffectsRandomSeed), as the crowd does at times; with MSC_SIMT=2
//                          and MSC_SIMT_BREAK=3 (no NpRngSetSimStack) these draws reach the simulation's streams and det / frames must differ from order B.
//   MSC_SIMT_MUTANT=1|2    test mutants: 1 = stage 0 advances gNetworkRandomSeed (a write the step reads: the det trace must differ between A and B), 2 = stage 6 skips the crowd draw
//                          when a bit of the live ball position is odd (a read of something the step writes: the pictures must differ between A, B and the interleave seeds).
#include "np/simorder17.h"
#include "np/simrun17.h"
#include "np/gsplit.h"
#include "np/bg30.h"
#include "Game/Game.h"
#include "Game/Ball.h"
#include "Game/NisPlayer.h"
#include "Game/HBMManager.h"
#include "Game/Task/GameTaskState.h"
#include "Game/Task/GameRenderTask.h"
#include "Game/Camera/CameraMan.h"
#include "NL/nlTask.h"
#include "NL/nlMath.h"

#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned int gNetworkRandomSeed;
extern unsigned int nlDefaultSeed;
extern unsigned int gEffectsRandomSeed;
extern "C" void (*g_npGsMarkHook)(int);

namespace
{
const int kStages = 7;
int sMode = -1;
int sOwnSeed = 0;
int sMutant = 0;
unsigned sSched = 0;
unsigned sNoise = 0;         // MSC_SIMT_NOISE (test): stage 0 leaks this many bytes of game heap per picture (address-dependence probe)
unsigned sMask = 0x7F;       // MSC_SIMT_STAGEMASK (test): which stages may start early; the others run at their old place (crowd stages 2..6 only with all of 1..n set)
unsigned sWinDef = 0x5A17C0DEu, sWinFx = 0x17A5E2B1u;   // the window's own streams (MSC_SIMT_ORDER_SEED)

bool sArmed;                 // this picture's window was started before RenderFrame (B / I)
bool sPlanned;               // I: the schedule of this picture is set
unsigned sDone;              // bit per stage: ran this picture (armed pictures)
bool sCaptainEarly;
bool sPostWanted;            // P: this picture is one where B would have started the window early
bool sInStage;
int sCamType;             // P2: type of the top camera at arm time
bool sOverlap;               // MSC_SIMT=2: the stages of an armed picture run in NpSimOrder17RunPending (game thread, beside the step)
bool sPending;               // overlap: stages planned, not run yet
unsigned sPlanMask;          // overlap: the stages Wanted() accepted at arm time
bool sFxArm;                 // overlap: g_bRenderWorldEffects as the arm saw it
bool sFx1;                   // the effects flag as stage 1 saw it (to count a flip between stage 1 and the crowd place)
unsigned long sPipAfterArm, sPictures, sArmedPictures, sNotArmed, sUnconsumed, sCaptainFlip, sEffectsMismatch, sStagesEarly, sStagesAtSpot, sNetMoved, sMarksSeen;
unsigned long sStageRuns[kStages];
unsigned long sWinDefDraws, sWinFxDraws;   // stage runs after which the render stream (nlDefaultSeed / gEffectsRandomSeed, no own seed) had moved: the window draws random numbers on this thread
unsigned sRank[kStages];
unsigned sMarkNo, sLastMarks = 600;
bool sInSteps;
unsigned sRng;

unsigned Next()
{
    sRng ^= sRng << 13; sRng ^= sRng >> 17; sRng ^= sRng << 5;
    return sRng;
}

void ReadEnv()
{
    const char* o = getenv("MSC_SIMT_ORDER");
    sMode = 0;
    if (o != 0 && (o[0] == 'B' || o[0] == 'b' || o[0] == '1')) sMode = 1;
    else if (o != 0 && (o[0] == 'I' || o[0] == 'i' || o[0] == '2')) sMode = 2;
    else if (o != 0 && (o[0] == 'P' || o[0] == 'p' || o[0] == '3')) sMode = 3;
    const char* s = getenv("MSC_SIMT_ORDER_SEED");
    sOwnSeed = s != 0 && s[0] != '0' && s[0] != 0;
    const char* m = getenv("MSC_SIMT_MUTANT");
    sMutant = m != 0 ? atoi(m) : 0;
    const char* c = getenv("MSC_SIMT_SCHED");
    sSched = c != 0 ? (unsigned)strtoul(c, 0, 0) : 0;
    const char* nz = getenv("MSC_SIMT_NOISE");
    sNoise = nz != 0 ? (unsigned)atoi(nz) : 0;
    const char* k = getenv("MSC_SIMT_STAGEMASK");
    if (k != 0 && *k) sMask = (unsigned)strtoul(k, 0, 0) & 0x7F;
    if (NpSimtOverlapMode())
    {
        sMode = 1;
        sOverlap = true;
        sOwnSeed = 0;   // the window draws the render stream (MSC_RENDER_RNG=2): nothing is swapped while a second thread runs the step
    }
    if (sMode != 0 || sOwnSeed || sMutant)
    {
        fprintf(stderr, "[simtorder] MSC_SIMT_ORDER=%s own-seed=%d sched=%u mutant=%d\n", sMode == 0 ? "A" : (sMode == 1 ? "B" : (sMode == 2 ? "I" : "P")), sOwnSeed, sSched, sMutant);
    }
}

void Report()
{
    fprintf(stderr, "[simtorder] pictures %lu armed %lu (not armed %lu) stages early %lu at the old place %lu | per stage runs %lu %lu %lu %lu %lu %lu %lu | window moved gNetworkRandomSeed in %lu stage runs | "
        "window drew the render stream (default / effects, stage runs) %lu / %lu | marks seen %lu | unconsumed %lu, captain flag flipped in the step %lu, effects flag mismatch %lu, picture-in-picture pass after an early window %lu\n",
        sPictures, sArmedPictures, sNotArmed, sStagesEarly, sStagesAtSpot, sStageRuns[0], sStageRuns[1], sStageRuns[2], sStageRuns[3], sStageRuns[4], sStageRuns[5], sStageRuns[6], sNetMoved,
        sWinDefDraws, sWinFxDraws, sMarksSeen, sUnconsumed, sCaptainFlip, sEffectsMismatch, sPipAfterArm);
}

// one stage, with the window's own streams swapped in when asked
void RunStage(int stage, int pip, int captain, bool early)
{
    unsigned sd = 0, sf = 0;
    if (sOwnSeed)
    {
        sd = nlDefaultSeed; sf = gEffectsRandomSeed;
        nlDefaultSeed = sWinDef; gEffectsRandomSeed = sWinFx;
    }
    const unsigned def0 = nlDefaultSeed, fx0 = gEffectsRandomSeed;   // (the game thread's own stream: the step never writes these globals with MSC_RENDER_RNG=2)
    const unsigned net0 = sOverlap ? 0u : gNetworkRandomSeed;   // (overlap: the step advances it at the same time; the counter is not kept)
    const bool wasIn = sInStage;
    sInStage = true;
    if (stage == 1) sFx1 = sOverlap ? sFxArm : (NpWin17Wanted(1, 0, 0) != 0);   // (overlap: the step may be writing the flag now; the stage only runs when it was 1 before the kick, and the step only sets it)
    if (sMutant == 1 && stage == 0) gNetworkRandomSeed = gNetworkRandomSeed * 1664525u + 1013904223u;
    if (sMutant == 3 && stage == 4) { nlRandom(1000, &nlDefaultSeed); nlRandomf(1.0f, &gEffectsRandomSeed); }   // test probe (the routes' window draws no random numbers): a draw of both streams like the crowd's, every armed picture
    if (sNoise && stage == 0) { volatile char* leak = new char[sNoise]; leak[0] = 1; }
    bool skip = false;
    if (sMutant == 2 && stage == 6 && g_pBall != 0)
    {
        unsigned bits;
        memcpy(&bits, &g_pBall->GetPosition().x, sizeof bits);
        skip = (bits & 1) != 0;   // reads a value the step writes
    }
    if (!skip) NpWin17Stage(stage, pip, captain);
    sInStage = wasIn;
    if (!sOverlap && gNetworkRandomSeed != net0) ++sNetMoved;
    if (sOwnSeed)
    {
        sWinDef = nlDefaultSeed; sWinFx = gEffectsRandomSeed;
        nlDefaultSeed = sd; gEffectsRandomSeed = sf;
    }
    if (!sOwnSeed) { if (nlDefaultSeed != def0) ++sWinDefDraws; if (gEffectsRandomSeed != fx0) ++sWinFxDraws; }
    ++sStageRuns[stage];
    if (early) ++sStagesEarly; else ++sStagesAtSpot;
}

bool Wanted(int stage, int captain)
{
    if (!((sMask >> stage) & 1)) return false;
    for (int j = 1; j < stage; ++j) if (!((sMask >> j) & 1)) return false;   // a crowd stage starts early only if every crowd stage before it does
    if (stage > 1 && !NpWin17Wanted(1, 0, captain)) return false;   // stages 1..6 stand or fall together
    return NpWin17Wanted(stage, 0, captain) != 0;
}

// mode I: the mark hook (called from NpGsEnter on every phase mark of the game thread, which includes the marks of the simulation step)
void MarkHook(int id)
{
    if (!sPlanned || sInStage) return;
    ++sMarksSeen;
    if (id == NPGS_SIM_PAD)   // the end of the step loop
    {
        sLastMarks = sMarkNo + 8;
        sPlanned = false;
        return;
    }
    ++sMarkNo;
    for (int i = 0; i < kStages; ++i)
    {
        if (sDone & (1u << i)) continue;
        if (sRank[i] > sMarkNo) break;
        if (Wanted(i, sCaptainEarly)) { RunStage(i, 0, sCaptainEarly, true); sDone |= 1u << i; }
    }
}
}

extern "C" int NpSimOrder17Mode(void)
{
    if (sMode < 0) ReadEnv();
    return sMode;
}

extern "C" volatile int gNpCrowdCamType;   // Render/CrowdImpostors.cpp
extern "C" void NpSimOrder17Early(void)
{
    if (sMode < 0) ReadEnv();
    ++sPictures;
    if (sArmed) ++sUnconsumed;
    sArmed = false;
    sPlanned = false;
    sPostWanted = false;
    sDone = 0;
    sPending = false;
    if (sMode == 0)
    {
        return;
    }
    if ((sPictures % 2000) == 0) Report();
    NisPlayer* nis = NisPlayer::Instance();
    if (nlTaskManager::m_pInstance->mCurrentState != TASK_GAMEPLAY || !g_bRenderWorld || (gpHBMManager != 0 && gpHBMManager->mActive && gpHBMManager->mReady)
        || nis == 0 || nis->HasSecondaryNis() || NpBg30Mode() != 0)
    {
        ++sNotArmed;
        return;
    }
    if (sOverlap && !NpSimtOverlapNow())   // MSC_SIMT=2 and this picture's step stays on the game thread (LAN / online / recorded, thread not made): the window stays in its old place
    {
        ++sNotArmed;
        return;
    }
    sCaptainEarly = g_pGame != 0 && g_pGame->mbCaptainShotToScoreOn;
    if (sMode == 3)
    {
        sPostWanted = true;   // P: the same pictures B would start early get their window after the step (Flush)
        return;
    }
    sArmed = true;
    ++sArmedPictures;
    sCaptainEarly = g_pGame != 0 && g_pGame->mbCaptainShotToScoreOn;
    if (sMode == 1)
    {
        if (sOverlap)
        {
            sFxArm = NpWin17Wanted(1, 0, 0) != 0;
            sPlanMask = 0;
            for (int i = 0; i < kStages; ++i)
            {
                if (Wanted(i, sCaptainEarly)) sPlanMask |= 1u << i;
            }
            sPending = true;
            sCamType = (int)cCameraManager::PeekCamera()->GetType();   // (P2: the window reads this, not the camera stack the step may change)
            return;
        }
        for (int i = 0; i < kStages; ++i)
        {
            if (Wanted(i, sCaptainEarly)) { RunStage(i, 0, sCaptainEarly, true); sDone |= 1u << i; }
        }
        return;
    }
    // mode I
    if (g_npGsOn == 0)
    {
        static int warned;
        if (!warned++) fprintf(stderr, "[simtorder] INVALID: MSC_SIMT_ORDER=I needs MSC_GSPLIT=1 (the phase marks)\n");
        return;
    }
    g_npGsMarkHook = MarkHook;
    sRng = (sSched * 2654435761u) ^ ((unsigned)sPictures * 40503u) ^ 0x9E3779B9u;
    if (sRng == 0) sRng = 1;
    Next(); Next();
    unsigned r[kStages];
    for (int i = 0; i < kStages; ++i) r[i] = Next() % (sLastMarks + 2);
    for (int i = 1; i < kStages; ++i) for (int j = i; j > 0 && r[j] < r[j - 1]; --j) { unsigned t = r[j]; r[j] = r[j - 1]; r[j - 1] = t; }
    for (int i = 0; i < kStages; ++i) sRank[i] = r[i];
    sMarkNo = 0;
    sPlanned = true;
    for (int i = 0; i < kStages; ++i)   // rank 0: before the step
    {
        if (sRank[i] > 0) break;
        if (Wanted(i, sCaptainEarly)) { RunStage(i, 0, sCaptainEarly, true); sDone |= 1u << i; }
    }
}

// MSC_SIMT=2: the planned stages, in order, on the calling (game) thread. Called by NpSimtRun between the kick and the join, or by FixedUpdateTask::Run when the picture has no step.
extern "C" int NpSimOrder17RunPending(void)
{
    if (!sPending)
    {
        return 0;
    }
    sPending = false;
    gNpCrowdCamType = sCamType;
    for (int i = 0; i < kStages; ++i)
    {
        if ((sPlanMask >> i) & 1u) { RunStage(i, 0, sCaptainEarly, true); sDone |= 1u << i; }
    }
    gNpCrowdCamType = -1;
    return 1;
}

extern "C" void NpSimOrder17Flush(int group, int pip, int captain)
{
    if (sMode < 0) ReadEnv();
    int a = group == 0 ? 0 : 1, b = group == 0 ? 1 : kStages;
    if (sMode == 3 && group == 0 && !pip && sPostWanted)
    {
        for (int i = 0; i < kStages; ++i) RunStage(i, 0, captain, false);
        sDone = (1u << kStages) - 1;
        sArmed = true;
        sCaptainEarly = captain != 0;
        return;
    }
    const bool armed = sArmed && !pip;
    if (sMode == 2 && armed && group == 0) b = kStages;   // I: stages whose rank was past the last phase mark start now, after the step and before the blend (the place P uses), not at the old crowd place
    if (sArmed && pip) ++sPipAfterArm;   // a secondary NIS started inside the step: the picture draws the window again for its picture-in-picture pass
    if (armed)
    {
        sPlanned = false;
        if (group == 0 && sCaptainEarly != (captain != 0))
        {
            ++sCaptainFlip;   // the step flipped the flag after the window started: the NPC stage ran (or not) on the old value; one picture
        }
    }
    for (int i = a; i < b; ++i)
    {
        if (armed && (sDone & (1u << i)))
        {
            if (i == 1 && sFx1 != (NpWin17Wanted(1, 0, captain) != 0)) ++sEffectsMismatch;
            continue;
        }
        RunStage(i, pip, captain, false);
        sDone |= 1u << i;
    }
    if (group == 1 && armed)
    {
        sArmed = false;
    }
}
#else
extern "C" int NpSimOrder17Mode(void) { return 0; }
extern "C" void NpSimOrder17Early(void) {}
extern "C" void NpSimOrder17Flush(int group, int pip, int captain) { (void)group; (void)pip; (void)captain; }
extern "C" int NpSimOrder17RunPending(void) { return 0; }
#endif
