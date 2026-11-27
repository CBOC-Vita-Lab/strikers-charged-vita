// MSC_PHYS_POSE (np/physpose.h)
#ifdef NP_NATIVE
#include "np/physpose.h"

#include "Game/Player.h"
#include "ode/NLGAdditions.h"
#include "Game/AI/HeadTrack.h"
#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/PoseAccumulator.h"
#include "Game/Physics/PhysicsCharacter.h"
#include "Game/Physics/PhysicsWorld.h"

// The job pool of platform/vita/src/np_jobs.cpp, called directly (weak: a build without it, the pure host harness, simply runs the old loop). Not PortParallelFor:
// that is compiled only with NP_PARALLEL, which would also switch on the render snapshot's lanes (simthread's MSC_SNAP_PAR).
extern "C" {
typedef void (*NpRangeFn)(void* ctx, size_t begin, size_t end, uint32_t lane);
unsigned NpJobsLanes(void) __attribute__((weak));
unsigned NpParallelFor(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx) __attribute__((weak));
}

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int NpPoseFastInit();
extern int gNpPoseFast;

namespace
{
cCharacter* sPend[10];
int sPendCount = 0;
unsigned sSteps, sParallel, sSerial, sPoses, sChecked, sMismatch;

int ReadMode()
{
    const char* e = getenv("MSC_PHYS_POSE");
    return (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
}
int ReadBreak()
{
    const char* e = getenv("MSC_PHYS_POSE_BREAK");
    return (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
}

struct PoseJob
{
    cCharacter** list;
    int breakMode;
    unsigned step;
};

inline void PoseOne(cCharacter* c)
{
    c->m_pPoseAccumulator->Pose(*c->m_pPoseTree, c->m_m4WorldMatrix);
}

// MSC_SIM26_BONES (PhysicsCharacterBase.cpp): set by NpPhysPoseRun on the game thread before the lanes run
bool sBonesStep = false;

void PoseRange(void* opaque, size_t begin, size_t end, uint32_t lane)
{
    PoseJob& job = *static_cast<PoseJob*>(opaque);
    for (size_t i = begin; i < end; ++i)
    {
        cCharacter* c = job.list[i];
        PoseOne(c);
        if (sBonesStep)
            c->m_pPhysicsCharacter->NpSim26Precompute(c->m_pPoseAccumulator, (int)i);
        if (job.breakMode == 2 && lane != 0 && c->m_nCharacterIndex == 3 && (job.step & 7) == 0)
            c->m_pPoseAccumulator->m_NodeMatrices[2].m41 += 0.0009765625f; // test mutant: only a lane's result differs from the serial one
    }
}

// ---- verify (MSC_PHYS_POSE=2): everything Pose can change, saved, hashed and restored
struct Saved
{
    unsigned char* mem;
    size_t cap;
    nlMatrix4* pCur;
    nlMatrix4* pPrev;
    size_t headBytes;
};
Saved sSaved[10];

struct Img
{
    unsigned char* p;
    size_t n;
};

size_t Layout(cPoseAccumulator* a, Img* out, int maxOut, cHeadTrack* ht)
{
    const size_t N = a->m_nNumNodes;
    int k = 0;
    size_t total = 0;
    auto add = [&](void* p, size_t n) { if (p && k < maxOut) { out[k].p = (unsigned char*)p; out[k].n = n; k++; total += n; } };
    add(a->m_NodeMatrices, (N + 1) * sizeof(nlMatrix4));
    add(a->m_PrevNodeMatrices, (N + 1) * sizeof(nlMatrix4));
    add(a->m_pQuaternions, (N + 1) * sizeof(nlQuaternion));
    add(a->m_rot, N * sizeof(RotAccum));
    add(a->m_scale, N * sizeof(ScaleAccum));
    add(a->m_trans, N * sizeof(TransAccum));
    add(&a->m_MorphWeights, sizeof(MorphWeightAccum));
    if (ht)
        add(ht, sizeof(cHeadTrack));
    return total;
}

uint64_t HashImg(const Img* im, int n, const cPoseAccumulator* a, const nlMatrix4* pCur)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++)
        for (size_t j = 0; j < im[i].n; j++)
            h = (h ^ im[i].p[j]) * 1099511628211ull;
    h = (h ^ (a->m_NodeMatrices == pCur ? 1u : 2u)) * 1099511628211ull;
    return h;
}

void SaveState(int idx, cCharacter* c)
{
    cPoseAccumulator* a = c->m_pPoseAccumulator;
    Img im[8];
    const size_t total = Layout(a, im, 8, c->m_pHeadTrack);
    Saved& s = sSaved[idx];
    if (s.cap < total)
    {
        free(s.mem);
        s.mem = (unsigned char*)malloc(total);
        s.cap = total;
    }
    size_t off = 0;
    for (int i = 0; i < 8 && im[i].p; i++)
    {
        memcpy(s.mem + off, im[i].p, im[i].n);
        off += im[i].n;
    }
    s.pCur = a->m_NodeMatrices;
    s.pPrev = a->m_PrevNodeMatrices;
}

void RestoreState(int idx, cCharacter* c)
{
    cPoseAccumulator* a = c->m_pPoseAccumulator;
    Saved& s = sSaved[idx];
    a->m_NodeMatrices = s.pCur;
    a->m_PrevNodeMatrices = s.pPrev;
    Img im[8];
    memset(im, 0, sizeof im);
    Layout(a, im, 8, c->m_pHeadTrack);
    size_t off = 0;
    for (int i = 0; i < 8 && im[i].p; i++)
    {
        memcpy(im[i].p, s.mem + off, im[i].n);
        off += im[i].n;
    }
}

uint64_t HashState(int idx, cCharacter* c)
{
    cPoseAccumulator* a = c->m_pPoseAccumulator;
    Img im[8];
    memset(im, 0, sizeof im);
    Layout(a, im, 8, c->m_pHeadTrack);
    int n = 0;
    while (n < 8 && im[n].p)
        n++;
    return HashImg(im, n, a, sSaved[idx].pCur);
}
} // namespace

int NpPhysPoseMode(void)
{
    // read once on the game thread at the first step (not a guarded static: no barrier at each call; np/hot_knob.h cannot be included next to nlMemory.h)
    static int mode = -1;
    if (mode < 0)
        mode = ReadMode();
    return mode;
}

int NpSim26BonesMode();
void NpSim26BonesReport();
void NpSim26BonesClear();

bool NpSim26BonesOn(void)
{
    return sBonesStep;
}

bool NpPhysPosePending(const cCharacter* c)
{
    for (int i = 0; i < sPendCount; i++)
        if (sPend[i] == c)
            return true;
    return false;
}

bool NpPhysPoseTake(const cCharacter* c)
{
    for (int i = 0; i < sPendCount; i++)
        if (sPend[i] == c)
        {
            sPend[i] = sPend[--sPendCount];
            return true;
        }
    return false;
}

void NpPhysPoseRun(PhysicsWorld* world)
{
    const int mode = NpPhysPoseMode();
    sPendCount = 0;
    if (mode == 0 || NpParallelFor == 0 || NpJobsLanes == 0 || NpJobsLanes() < 2)
        return;
    if (gNpPoseFast < 0)
        NpPoseFastInit(); // read on this thread before the lanes ask
    static int brkv = -1;
    if (brkv < 0)
        brkv = ReadBreak();
    const int brk = brkv;
    cCharacter* list[10];
    int n = 0;
    // pass 1: body order, each character's PostUpdate up to its pose
    for (dBodyID body = dWorldGetFirstBody(world->m_World); body != 0; body = dBodyGetNextBody(body))
    {
        PhysicsObject* obj = (PhysicsObject*)dBodyGetData(body);
        for (int i = 0; i < 10; i++)
        {
            cCharacter* c = g_pCharacters[i];
            if (c != 0 && c->m_pPhysicsCharacter != 0 && (PhysicsObject*)c->m_pPhysicsCharacter == obj && c->m_pPoseAccumulator != 0 && c->m_pPoseTree != 0)
            {
                c->m_pPhysicsCharacter->PostUpdatePre();
                c->PostPhysicsUpdatePre();
                if (brk == 1 && i == 3)
                    c->m_DetChar.m_v3Position.x += 0.001f; // test mutant: the determinism trace must change
                list[n++] = c;
                break;
            }
        }
    }
    if (n == 0)
        return;
    // MSC_SIM26_POSEORDER=1 (sim26): the lanes claim the characters one at a time, in list order; the heavy ones (the goalies: more bone volumes,
    // a bigger pose tree) go first so that no lane starts one at the end while the others wait (longest first). Pass 1 above and pass 3 stay in body
    // order; the poses are independent of each other (MSC_PHYS_POSE=2 verifies it), so only the timing changes.
    {
        static int ord = -1;
        if (ord < 0)
        {
            const char* e = getenv("MSC_SIM26_POSEORDER");
            ord = (e && e[0] == '1') ? 1 : 0;
        }
        if (ord == 1)
        {
            int w[10], w0[10];
            for (int i = 0; i < n; i++)
                w0[i] = w[i] = list[i]->m_pPhysicsCharacter->NpSim26BoneCount();
            for (int i = 1; i < n; i++)   // stable insertion sort, most bone volumes first
            {
                cCharacter* c = list[i];
                const int wc = w[i];
                int j = i;
                while (j > 0 && w[j - 1] < wc)
                {
                    list[j] = list[j - 1];
                    w[j] = w[j - 1];
                    j--;
                }
                list[j] = c;
                w[j] = wc;
            }
            static unsigned reported = 0;
            if (reported++ % 3000 == 0)
            {
                printf("[sim26order] lane list by bone volumes, body order:");
                for (int i = 0; i < n; i++)
                    printf(" %d", w0[i]);
                printf("; claimed in:");
                for (int i = 0; i < n; i++)
                    printf(" %d", w[i]);
                printf("\n");
                fflush(stdout);
            }
        }
    }
    // pass 2: the poses (MSC_SIM26_BONES: and the arithmetic of each character's UpdatePose)
    sBonesStep = NpSim26BonesMode() != 0;
    if (sBonesStep)
    {
        NpSim26BonesClear();
        NpSim26BonesReport();
    }
    PoseJob job = { list, brk, sSteps };
    if (mode == 2)
        for (int i = 0; i < n; i++)
            SaveState(i, list[i]);
    NpParallelFor((size_t)n, 3, PoseRange, &job);
    if (mode == 2)
    {
        uint64_t h1[10];
        for (int i = 0; i < n; i++)
        {
            h1[i] = HashState(i, list[i]);
            RestoreState(i, list[i]);
            PoseOne(list[i]); // the serial reference, on the game thread
            sChecked++;
            if (HashState(i, list[i]) != h1[i])
                sMismatch++;
        }
    }
    for (int i = 0; i < n; i++)
        sPend[i] = list[i];
    sPendCount = n;
    sPoses += n;
    sParallel++;
    if (++sSteps % 300 == 0)
    {
        printf("[physpose] steps=%u parallel=%u poses=%u lanes=%u mode=%d checked=%u mismatches=%u\n", sSteps, sParallel, sPoses, NpJobsLanes(), mode, sChecked, sMismatch);
        fflush(stdout);
    }
}
#endif
