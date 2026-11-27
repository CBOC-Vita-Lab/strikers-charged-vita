// MSC_SKIN_POSE_PAR (np/skinpose.h)
#ifdef NP_NATIVE
#include "np/skinpose.h"

#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/GL/ShaderSkinMesh.h"
#include "Game/PoseAccumulator.h"
#include "np/overlap.h"
#include "NL/nlMath.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The job pool of platform/vita/src/np_jobs.cpp, called directly and weak (as PhysicsPose.cpp): a build without it runs the old loop.
extern "C" {
typedef void (*NpRangeFn)(void* ctx, size_t begin, size_t end, uint32_t lane);
unsigned NpJobsLanes(void) __attribute__((weak));
unsigned NpParallelFor(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx) __attribute__((weak));
unsigned NpParallelForAvoid(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx, unsigned avoidCores) __attribute__((weak));   // lane24 FIX-20 (np_jobs.cpp)
// MSC_AB (renderer/src/vgx_ab.h): weak, 0 unless MSC_AB is set
extern int VgxAbOn __attribute__((weak));
long VgxAbOverride(const char* key, long dflt) __attribute__((weak));
}

namespace
{
unsigned sPictures, sParallel, sSerialPictures, sItems, sMeshes, sChecked, sMismatch, sDecChecked, sDecMismatch;
unsigned sPicture;

int EnvMode()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_SKIN_POSE_PAR");
        v = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
    }
    return v;
}
// lane24 FIX-20 (MSC_SKIN_POSE_PAR_AVOID=<core digits>, default none = today): the pose jobs do not wake workers on these cores (1 = the back end's core, the finding of the console skinpose runs: a lane on core 1
// preempted the renderer's back end, +2.5 ms of its cpu at The Dump). The caller's core 0 is always left out when the key is set. Results are the same on any lanes (verified: mode 2).
unsigned AvoidCores()
{
    static int v = -1;
    if (v < 0)
    {
        unsigned m = 0;
        const char* e = getenv("MSC_SKIN_POSE_PAR_AVOID");
        for (; e && *e; e++)
            if (*e >= '0' && *e <= '3')
                m |= 1u << (*e - '0');
        v = (int)m;
        if (m && EnvMode())
            fprintf(stderr, "[skinpose] MSC_SKIN_POSE_PAR_AVOID: pose jobs avoid workers on core mask 0x%X (and the caller's core 0)%s\n", m, NpParallelForAvoid ? "" : " (this build has no NpParallelForAvoid: ignored)");
    }
    return (unsigned)v;
}
int EnvBreak()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_SKIN_POSE_PAR_BREAK");
        v = (e && e[0] >= '1' && e[0] <= '4') ? e[0] - '0' : 0;
    }
    return v;
}

// the mesh PoseSkinMesh would pose for this type (a missing one falls back to the normal mesh)
inline ShaderSkinMesh* MeshFor(cCharacter* c, int type)
{
    GLSkinMesh* m = c->m_pSkinMesh[type];
    if (m == 0)
        m = c->m_pSkinMesh[0];
    return static_cast<ShaderSkinMesh*>(m);
}

struct Item
{
    DrawableCharacter* d;
    cCharacter* c;
    const NpRenderPlan* plan;
};
struct Job
{
    Item* items;
    int brk;
    unsigned picture;
};

inline uint64_t Fnv(uint64_t h, const void* p, size_t n)
{
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
// everything Pose writes: the pose matrices and the morph weights with what UpdateMorphWeights derives
uint64_t HashMesh(const ShaderSkinMesh* m)
{
    uint64_t h = 1469598103934665603ull;
    if (m == 0)
        return h;
    h = Fnv(h, m->poseMatrices, m->numBones * sizeof(nlMatrix4));
    h = Fnv(h, m->morphWeights, m->numMorphs * sizeof(MorphWeight));
    h = Fnv(h, &m->numActiveMorphs, sizeof m->numActiveMorphs);
    h = Fnv(h, &m->morphWeightsChanged, sizeof m->morphWeightsChanged);
    return h;
}

// verify: the morph state Pose reads and writes (the pose matrices it overwrites whole from its inputs)
struct Morph
{
    MorphWeight w[256];
    unsigned long n, active;
    bool changed;
};
Morph sSaved[10][4];
void SaveMorph(Morph& s, const ShaderSkinMesh* m)
{
    s.n = m->numMorphs < 256 ? m->numMorphs : 256;
    memcpy(s.w, m->morphWeights, s.n * sizeof(MorphWeight));
    s.active = m->numActiveMorphs;
    s.changed = m->morphWeightsChanged;
}
void RestoreMorph(const Morph& s, ShaderSkinMesh* m)
{
    memcpy(m->morphWeights, s.w, s.n * sizeof(MorphWeight));
    m->numActiveMorphs = s.active;
    m->morphWeightsChanged = s.changed;
}

// verify (ownership audit): before and after the lanes, (a) every skin mesh no item lists and (b) every blended pose accumulator's node matrices and morph weights
// (the inputs) must be unchanged: the lanes wrote only the meshes they were given and read the inputs only
uint64_t HashAcc(const cPoseAccumulator* a)
{
    uint64_t h = 1469598103934665603ull;
    if (a == 0)
        return h;
    h = Fnv(h, a->m_NodeMatrices, (size_t)a->GetNumNodes() * sizeof(nlMatrix4));
    h = Fnv(h, &a->m_MorphWeights, sizeof a->m_MorphWeights);
    return h;
}
uint64_t sForeignBefore[10][4], sInputBefore[10];
bool sListed[10][4];
unsigned sAuditChecked, sAuditBad;

void PoseRange(void* opaque, size_t begin, size_t end, uint32_t)
{
    Job& job = *static_cast<Job*>(opaque);
    for (size_t k = begin; k < end; ++k)
    {
        Item& it = job.items[k];
        cCharacter* c = it.c;
        cPoseAccumulator* acc = it.d->poseAccumulator;
        const bool brkNow = job.brk != 0 && (job.picture & 7) == 0;
        const int idx = c->m_nCharacterIndex;
        if (brkNow && job.brk == 2 && idx == 5)
        {
            for (int j = 0; j < it.plan->nPose; j++)   // test mutant: the pose left out, the flags set
                c->unknown_0x018[c->m_pSkinMesh[it.plan->types[j]] == 0 ? 0 : it.plan->types[j]] = true;
            continue;
        }
        for (int j = 0; j < it.plan->nPose; j++)
        {
            int type = it.plan->types[j];
            if (brkNow && job.brk == 3 && idx == 7 && type == 2 && c->m_pSkinMesh[2] != 0)
            {
                c->m_pSkinMesh[0]->Pose(acc);   // test mutant: the wrong model type posed, the flag of the named one set
                c->unknown_0x018[2] = true;
                continue;
            }
            c->PoseSkinMesh(acc, type);   // what the serial draw calls: the flag of the (fallback-resolved) type is set after the pose
        }
        if (brkNow && job.brk == 4 && idx == 2 && g_pCharacters[6] != 0 && g_pCharacters[6]->m_pSkinMesh[1] != 0)   // test mutant: a job writes another character's mesh
            static_cast<ShaderSkinMesh*>(g_pCharacters[6]->m_pSkinMesh[1])->poseMatrices[1].m41 += 0.0009765625f;
        if (brkNow && job.brk == 1 && idx == 3)
            MeshFor(c, it.plan->types[0])->poseMatrices[2].m41 += 0.0009765625f;   // test mutant: the result differs from the serial one
    }
}
} // namespace

int NpSkinPoseMode(void)
{
    if (NpParallelFor == 0 || NpJobsLanes == 0 || NpJobsLanes() < 2)
        return 0;
    int m = EnvMode();
    if (&VgxAbOn && VgxAbOn && VgxAbOverride)   // (MSC_AB: the in-run A/B harness answers while it flips this key)
    {
        const long o = VgxAbOverride("MSC_SKIN_POSE_PAR", -1);
        if (o >= 0)
            m = o > 2 ? 2 : (int)o;
    }
    return m;
}

void NpSkinPoseRun(DrawableCharacter* d, cCharacter* const* src, const NpRenderPlan* plans, const bool* ok, int n)
{
    const int mode = NpSkinPoseMode();
    Item items[10];
    int count = 0;
    for (int i = 0; i < n && count < 10; i++)
        if (plans[i].nPose > 0)   // (also a character whose Render returned after the shock pose, at the LOD cull: the serial code posed it too)
        {
            items[count].d = &d[i];
            items[count].c = src[i];
            items[count].plan = &plans[i];
            count++;
        }
    sPictures++;
    ++sPicture;
    if (count == 0)
        return;
    // first uses read on this thread before the lanes ask: the switches ShaderSkinMesh::Pose looks at
    NpSkinPoseNeeded();
    NpPrefetchOn(8);
    if (mode == 2)
    {
        for (int i = 0; i < n; i++)
        {
            for (int t = 0; t < 4; t++)
                sListed[i][t] = false;
            for (int j = 0; j < plans[i].nPose; j++)
                sListed[i][src[i]->m_pSkinMesh[plans[i].types[j]] == 0 ? 0 : plans[i].types[j]] = true;
            for (int t = 0; t < 4; t++)
                sForeignBefore[i][t] = (src[i]->m_pSkinMesh[t] != 0 && !sListed[i][t]) ? HashMesh(static_cast<ShaderSkinMesh*>(src[i]->m_pSkinMesh[t])) : 0;
            sInputBefore[i] = HashAcc(d[i].poseAccumulator);
        }
    }
    if (mode == 2)
        for (int k = 0; k < count; k++)
        {
            bool seen[4] = { false, false, false, false };
            for (int j = 0; j < items[k].plan->nPose; j++)
            {
                const int t = items[k].plan->types[j];
                const int r = items[k].c->m_pSkinMesh[t] == 0 ? 0 : t;
                if (!seen[r])
                {
                    seen[r] = true;
                    SaveMorph(sSaved[k][r], MeshFor(items[k].c, t));
                }
            }
        }
    Job job = { items, EnvBreak(), sPicture };
    if (AvoidCores() != 0 && NpParallelForAvoid != 0)
        NpParallelForAvoid((size_t)count, 3, PoseRange, &job, AvoidCores());
    else
        NpParallelFor((size_t)count, 3, PoseRange, &job);
    for (int i = 0; i < n; i++)
        if (!ok[i] && plans[i].nPose > 0)
            src[i]->fn_8001C574();   // (the reset Render made after those poses, which the decision made before them)
    sParallel++;
    sItems += (unsigned)count;
    for (int k = 0; k < count; k++)
        sMeshes += (unsigned)items[k].plan->nPose;
    if (mode == 2)
    {
        for (int i = 0; i < n; i++)
        {
            for (int t = 0; t < 4; t++)
                if (src[i]->m_pSkinMesh[t] != 0 && !sListed[i][t])
                {
                    sAuditChecked++;
                    if (HashMesh(static_cast<ShaderSkinMesh*>(src[i]->m_pSkinMesh[t])) != sForeignBefore[i][t])
                        sAuditBad++;
                }
            sAuditChecked++;
            if (HashAcc(d[i].poseAccumulator) != sInputBefore[i])
                sAuditBad++;
        }
        // the serial reference: the outputs of every mesh the lanes posed are hashed, its morph state (what UpdateMorphWeights compares against: the weights, the
        // changed flag) is put back to what it was before the lanes, and the poses are made again here with the serial code's flags
        for (int k = 0; k < count; k++)
        {
            cCharacter* c = items[k].c;
            cPoseAccumulator* acc = items[k].d->poseAccumulator;
            bool done[4] = { false, false, false, false };
            uint64_t h1[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < items[k].plan->nPose; j++)
            {
                const int t = items[k].plan->types[j];
                const int r = c->m_pSkinMesh[t] == 0 ? 0 : t;
                if (!done[r])
                {
                    done[r] = true;
                    h1[r] = HashMesh(MeshFor(c, t));
                    RestoreMorph(sSaved[k][r], MeshFor(c, t));
                }
            }
            bool again[4] = { false, false, false, false };
            for (int j = 0; j < items[k].plan->nPose; j++)
            {
                const int t = items[k].plan->types[j];
                const int r = c->m_pSkinMesh[t] == 0 ? 0 : t;
                if (!again[r])
                {
                    again[r] = true;
                    ShaderSkinMesh* m = MeshFor(c, t);
                    m->Pose(acc);
                    sChecked++;
                    if (HashMesh(m) != h1[r])
                        sMismatch++;
                }
            }
        }
    }
    if (sParallel % 300 == 1)
    {
        printf("[skinpose] pictures=%u parallel=%u items=%u meshes=%u mode=%d lanes=%u checked=%u mismatches=%u decisions=%u decision_mismatches=%u audited=%u foreign_or_input_writes=%u\n", sPictures, sParallel, sItems,
            sMeshes, mode, NpJobsLanes(), sChecked, sMismatch, sDecChecked, sDecMismatch, sAuditChecked, sAuditBad);
        fflush(stdout);
    }
}

void NpSkinPoseCheckDecision(bool ok, const NpRenderPlan& plan, bool ok2, const NpRenderPlan& plan2)
{
    sDecChecked++;
    bool same = ok == ok2;
    if (same && ok)
    {
        same = plan.nPose == plan2.nPose && plan.shock == plan2.shock && plan.lowPoly == plan2.lowPoly && plan.level == plan2.level && plan.scene == plan2.scene && plan.flatShadow == plan2.flatShadow;
        for (int i = 0; same && i < plan.nPose; i++)
            same = plan.types[i] == plan2.types[i];
    }
    if (!same)
        sDecMismatch++;
}

// ---- MSC_SKIN_POSE_DIGEST
namespace
{
uint64_t sDigest = 1469598103934665603ull;
unsigned sDigestPictures;
void PrintDigest()
{
    printf("[skindigest] pictures=%u hash=%016llx\n", sDigestPictures, (unsigned long long)sDigest);
    fflush(stdout);
}
} // namespace

int NpSkinPoseDigestOn(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_SKIN_POSE_DIGEST");
        v = (e && e[0] == '1') ? 1 : 0;
        if (v)
            atexit(PrintDigest);
    }
    return v;
}

void NpSkinPoseDigest(cCharacter* const* src, int n)
{
    for (int i = 0; i < n; i++)
    {
        cCharacter* c = src[i];
        if (c == 0)
            continue;
        for (int t = 0; t < 4; t++)
            if (c->m_pSkinMesh[t] != 0)
            {
                const uint64_t h = HashMesh(static_cast<ShaderSkinMesh*>(c->m_pSkinMesh[t]));
                sDigest = Fnv(sDigest, &h, sizeof h);
            }
    }
    if (++sDigestPictures % 300 == 0)
        PrintDigest();
}
#endif
