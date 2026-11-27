#include "Game/Task/GameTaskState.h"
#include "np/gsplit.h"
#include "Game/RenderSnapshot.h"

#include "Game/Camera/CameraMan.h"
#include "Game/CharacterTemplate.h"
#include "Game/GameInfo.h"
#include "Game/GL/GLInventory.h"
#include "Game/GL/GLTextureAnim.h"
#include "Game/Goalie.h"
#include "Game/Physics/PhysicsNet.h"
#include "Game/Physics/PhysicsPatch.h"
#include "Game/Player.h"
#include "Game/Render/ChainChomp.h"
#include "Game/Render/DiddyBanana.h"
#include "Game/Render/FlyingCamera.h"
#include "Game/Render/NetMesh.h"
#include "Game/Render/NPCManager.h"
#include "Game/Render/StadiumLoading.h"
#include "Game/Render/WindDebris.h"
#include "Game/Task/FixedUpdateTask.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
#include "NL/nlMemory.h"
#include "NL/nlTask.h"
#include "port/parallel.h"
#include "Game/CharacterEffects.h"
#include "np/overlap.h"
#include "np/skinpose.h"
#include "np/snap_onecopy.h"
#include "np/render_neon.h"
#include "np/framesend.h"   // gt27 (MSC_BLEND_ASYNC): the helper lane
#include <stdlib.h>
#include <stdio.h>

#include "Game/SharedStaticStorage.h"

float g_AllActorsHidden;
extern int g_nLeftPropJointIndex;   // (DrawableCharacter.cpp)
static nlVector3 sInvalidDrawablePosition = {};

// NP: helper-core jobs for the snapshot (strikersVita PR #1, adapted to this tree). A snapshot is ten
// DrawableCharacters whose Grab and Blend write only their own pose accumulator, plus 150 powerups
// that are plain value interpolation, so the loops split cleanly. DrawableCharacter::Grab/Blend
// allocate the accumulator on first use and the game's allocator is not a multi-thread heap: those
// first uses run serially on the calling thread (the mask), everything else on the helpers. With no
// helper threads (PortParallelWorkers() == 0) the original loops run unchanged.
namespace
{
struct NpCharacterGrabJob
{
    RenderSnapshot* dst;
    unsigned int serialMask;
};

int NpGrabCharacters(void* opaque, size_t begin, size_t end, uint32_t)
{
    NpCharacterGrabJob& job = *static_cast<NpCharacterGrabJob*>(opaque);
    for (size_t i = begin; i < end; ++i)
    {
        if ((job.serialMask & (1u << i)) == 0)
            job.dst->mCharacters[i].Grab(*g_pCharacters[i]);
    }
    return 1;
}

struct NpBlendJob
{
    RenderSnapshot* dst;
    RenderSnapshot* lhs;
    RenderSnapshot* rhs;
    const float* factors;
    unsigned int serialMask;
};

// MSC_SNAP_PAR_BREAK=1 (proof only, platform/vita/src/np_port_parallel.cpp): the worker lanes blend with a wrong factor, so the frame hashes must change
int NpSnapParBreak()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_SNAP_PAR_BREAK");
        v = (e != 0 && *e == '1') ? 1 : 0;
    }
    return v;
}

int NpBlendCharacters(void* opaque, size_t begin, size_t end, uint32_t lane)
{
    NpBlendJob& job = *static_cast<NpBlendJob*>(opaque);
    float broken[3];
    const float* factors = job.factors;
    if (lane != 0 && NpSnapParBreak())
    {
        broken[0] = broken[1] = broken[2] = 0.5f * job.factors[0];
        factors = broken;
    }
    for (size_t i = begin; i < end; ++i)
    {
        if ((job.serialMask & (1u << i)) == 0)
            job.dst->mCharacters[i].Blend(factors, job.lhs->mCharacters[i], job.rhs->mCharacters[i]);
    }
    return 1;
}

int NpBlendPowerups(void* opaque, size_t begin, size_t end, uint32_t)
{
    NpBlendJob& job = *static_cast<NpBlendJob*>(opaque);
    for (size_t i = begin; i < end; ++i)
        job.dst->mPowerups[i].Blend(job.factors, job.lhs->mPowerups[i], job.rhs->mPowerups[i]);
    return 1;
}

// gt27 (MSC_BLEND_ASYNC, default off; np/overlap.h has no say): the characters' and power-ups' blend of a live-play picture runs on the helper lane (np_framesend.cpp
// NpLaneForce, core 2) from the top of RenderFrame, while the game thread draws the world NPCs, the crowd and the stadium; the join is at the old place
// (RenderSnapshot::Blend). The lane writes what the pool lanes write today (the ten DrawableCharacters' own fields and accumulators, the 150 power-ups);
// the head of Blend (pose cull, first-use allocations) runs on the game thread at the kick. Exactness is checked at the join: the same target,
// the same two snapshots and the same factor bits, else the lane is waited for and the whole blend is made again the old way.
struct NpBlendAsyncState
{
    int on;                     // -1 undecided
    int brk;                    // MSC_BLEND_ASYNC_BREAK=1 (proof only): the lane blends with half the factor
    bool pending;
    RenderSnapshot* dst;
    RenderSnapshot* lhs;
    RenderSnapshot* rhs;
    float factors[3];
    float brokenFactors[3];
    NpBlendJob job;
    unsigned long kicks, taken, mismatched, notKicked, drained;
};
NpBlendAsyncState sBA = {-1, 0, false, 0, 0, 0, {0, 0, 0}, {0, 0, 0}, {0, 0, 0, 0, 0}, 0, 0, 0, 0, 0};
int NpBlendCharacters(void* opaque, size_t begin, size_t end, uint32_t lane);
int NpBlendPowerups(void* opaque, size_t begin, size_t end, uint32_t);
// sh18 (MSC_BLEND_SHARE, default 1; 0 = the lane blends everything and the join waits for it, as gt27 had it): the blend is cut into items
// (each character, then the power-ups in chunks of kNpShareChunk) that the lane and, at the join, the game thread claim one at a time from one counter.
// Each item is blended once, by whoever claims it, with the same function and the same factors as before, so the picture is the same bit for bit;
// the join then only waits for the item the lane is in (at most one character), instead of the lane's whole remaining blend (console s50: 2.7 ms a join, up to 14).
static const unsigned kNpShareChunk = 16;
static const unsigned kNpShareItems = 10 + (150 + kNpShareChunk - 1) / kNpShareChunk;
static unsigned sNpShareNext;   // __atomic: the next unclaimed item of the pending blend
static unsigned long sNpShareJoinItems, sNpShareJoins;
static int NpBlendShareOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("MSC_BLEND_SHARE");
        on = (e != 0 && *e == '0') ? 0 : 1;
    }
    return on;
}
// lane24 FIX-07 (MSC_BLEND_TAKEOVER, default 1; 0 = off). The join (RenderSnapshot::Blend) first claims the items nobody has started, then waited for the lane's JOB to finish. When the lane thread had been
// woken at the kick but had not been scheduled (core 2: the front-end record player runs at the lane's priority, and a woken thread waits for its slice), the job was not started at all, every item was
// already blended here, and the game thread still waited kLaneStallSlices x 4 ms = 12 ms before it ran the (empty) job itself: the 12.4-12.9 ms maximum waits and the 1,560-2,126 blocked waits a match of
// console s52, which are exactly that stall. Now the unstarted job is run at once (NpLaneTakeUnstarted: the same job function, one executor at a time, nothing left to claim, so it returns in
// microseconds). Exact: the items, their factors and the function are the ones the lane would have used. A job the lane thread is already inside is still waited for.
static unsigned long sNpTakeJoins, sNpTakeHits;
static int NpBlendTakeoverOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("MSC_BLEND_TAKEOVER");
        on = (e != 0 && *e == '0') ? 0 : 1;
    }
    return on;
}
static void NpBlendItem(NpBlendJob* job, unsigned item)
{
    if (item < 10)
        NpBlendCharacters(job, item, item + 1, 0);
    else
    {
        const size_t b = (size_t)(item - 10) * kNpShareChunk;
        const size_t e = b + kNpShareChunk < 150 ? b + kNpShareChunk : 150;
        NpBlendPowerups(job, b, e, 0);
    }
}
static unsigned NpBlendClaimAll(NpBlendJob* job)   // claims and blends items until none is left; returns how many this caller blended
{
    unsigned n = 0;
    for (;;)
    {
        const unsigned item = __atomic_fetch_add(&sNpShareNext, 1u, __ATOMIC_ACQ_REL);
        if (item >= kNpShareItems)
            return n;
        NpBlendItem(job, item);
        ++n;
    }
}
void NpBlendAsyncRun(void* p)
{
    NpBlendJob* job = static_cast<NpBlendJob*>(p);
    if (NpBlendShareOn())
    {
        NpBlendClaimAll(job);
        return;
    }
    NpBlendCharacters(job, 0, 10, 0);
    NpBlendPowerups(job, 0, 150, 0);
}
inline bool NpSameFactors(const float* a, const float* b)
{
    return memcmp(a, b, 3 * sizeof(float)) == 0;
}

inline size_t NpCharacterGrain()
{
    return PortParallelLanes() >= 4 ? 2 : 3;
}

inline bool NpBallRefers(const DrawableBallFlags& ball, int i)
{
    return i == (int)ball.bits.ownerIndex || i == (int)ball.bits.previousOwnerIndex || i == (int)ball.bits.passTargetIndex || i == (int)ball.bits.lastTouchIndex;
}

// MSC_POSE_CULL=2 (np/overlap.h): before the blend, on the game thread (the test reads the view's lazily made planes), which players of this frame need no pose at all.
// Open play only: mCurrent is then a Grab, whose matrices the emitters of a skipped player can read instead (GetReplayDrawableCharacter); a replay frame is not. The players
// the ball refers to stay (ball and camera code reads them), and so does whoever has no accumulator yet or a pose of the object path, which Blend treats apart.
inline void NpChoosePoseCull(RenderSnapshot& dst, const float* factors, RenderSnapshot& lhs, RenderSnapshot& rhs)
{
    const bool on = NpPoseCullLevel() >= 2 && nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY;
    const DrawableBallFlags& ball0 = lhs.mBall.mFlags;
    const DrawableBallFlags& ball1 = rhs.mBall.mFlags;
    for (int i = 0; i < 10; i++)
    {
        DrawableCharacter& d = dst.mCharacters[i];
        const DrawableCharacter& a = lhs.mCharacters[i];
        const DrawableCharacter& b = rhs.mCharacters[i];
        bool cull = false;
        if (on && d.poseAccumulator != 0 && a.character != 0 && !a.useObject && !b.useObject
            && !NpBallRefers(ball0, i) && !NpBallRefers(ball1, i))
        {
            nlVector3 bip01;
            nlVecLerp(bip01, a.bip01Position, b.bip01Position, factors[0]);
            cull = !d.WillBeDrawn(*a.character, bip01);
        }
        d.poseCulled = cull;
    }
}
} // namespace

// MSC_SNAP_ONECOPY (np/snap_onecopy.h): 0 (default), 1, 2 (proof: previous matrices poisoned), 3 (proof mutant)
int gNpSnapOneCopy = -1;
int NpSnapOneCopyInit()
{
    const char* e = getenv("MSC_SNAP_ONECOPY");
    int m = (e != 0 && *e != 0) ? atoi(e) : 0;
    gNpSnapOneCopy = (m >= 0 && m <= 3) ? m : 0;
    return gNpSnapOneCopy;
}

RenderSnapshot::RenderSnapshot()
    : mEvents(0)
    , mValid(false)
    , mGoalLight(false)
    , mBall(this)
    , mNumBulletBills(0)
    , mHasHammers(false)
    , mNumFlyingCameras(0)
    , mpNetMeshPositiveX(0)
    , mpNetMeshNegativeX(0)
    , mFrameBlendPercent(0.0f)
    , mFlags(RenderSnapshotFlags())
{
    mCameraUp.x = 0.0f;
    mCameraUp.y = 0.0f;
    mCameraUp.z = 1.0f;
}

void RenderSnapshot::Initialize()
{
    DrawableNetMesh* pNetMesh = new (nlMalloc(sizeof(DrawableNetMesh), 8, false)) DrawableNetMesh(true);
    mpNetMeshPositiveX = pNetMesh;

    pNetMesh = new (nlMalloc(sizeof(DrawableNetMesh), 8, false)) DrawableNetMesh(false);
    mpNetMeshNegativeX = pNetMesh;

    int index = 0;
    for (DrawableFlyingCamera* camera = _2298; index < 10; ++camera)
    {
        camera->mCameraIndex = index++;
    }

    mNumFlyingCameras = 0;
    mWindDebris[0].visible = false;
    mWindDebris[1].visible = false;
    mWindDebris[2].visible = false;

    for (int i = 0; i < 10; ++i)
    {
        int value = g_pCharacters[i]->m_DetChar.m_eCharacterClass;
        if (value == 8)
        {
            mFlags.raw |= 0x80000000;
        }
        else if (value == 5)
        {
            mFlags.raw |= 0x40000000;
        }
        else if (value == 13)
        {
            mFlags.raw |= 0x20000000;
        }
        else if (value == 19)
        {
            mFlags.raw |= 0x10000000;
        }
        else if (value == 14)
        {
            mFlags.raw |= 0x08000000;
        }
        else if (value == 12)
        {
            mFlags.raw |= 0x04000000;
        }
        else if (value == 2)
        {
            mFlags.raw |= 0x02000000;
        }
    }

    if (GameInfoManager::Instance()->GetStadium() == STAD_THUNDER_ISLAND)
    {
        mFlags.raw |= 0x00800000;
    }
    if (GameInfoManager::Instance()->GetStadium() == STAD_SAND_TOMB)
    {
        mFlags.raw |= 0x00400000;
    }

    mValid = false;
}

void RenderSnapshot::Free()
{
    for (int i = 0; i < 10; ++i)
    {
        mCharacters[i].Free();
    }

    mChainChomp.Free();
    mDiddyBanana.Free();

    for (int i = 0; i < 3; ++i)
    {
        mWindDebris[i].Free();
    }

    delete mpNetMeshPositiveX;
    delete mpNetMeshNegativeX;
    mpNetMeshPositiveX = 0;
    mpNetMeshNegativeX = 0;
}

void RenderSnapshot::Grab()
{
    unsigned int i;
    (void)NpSnapOneCopy();   // both switches are read here, on the game thread, before any lane uses them
    (void)NpRenderNeon();
    if (PortParallelSite(0) != 0 && PortParallelWorkers() != 0)
    {
        unsigned int serialMask = 0;
        for (i = 0; i < 10; i++)
        {
            if (mCharacters[i].poseAccumulator == 0)
            {
                mCharacters[i].Grab(*g_pCharacters[i]);
                serialMask |= 1u << i;
            }
        }
        fxGetTexturing(eFXTex_Nothing);   // (wave 9) its one-time texture lookup happens here, not on two lanes at once (DrawableCharacter::Grab)
        NpCharacterGrabJob job = { this, serialMask };
        PortParallelFor(10, NpCharacterGrain(), NpGrabCharacters, &job);
    }
    else
    {
        for (i = 0; i < 10; i++)
        {
            mCharacters[i].Grab(*g_pCharacters[i]);
        }
    }
    for (i = 0; i < 150; i++)
    {
        mPowerups[i].Grab(i);
    }
    if (mFlags.bits.hammers)
    {
        mHasHammers = gNPCManager->GetNumHammers() != 0;
        if (mHasHammers)
        {
            for (unsigned int i = 0; i < 15; i++)
                mHammers[i].Grab(gNPCManager->GetHammer(i));
        }
    }
    if (mFlags.bits.thwomps)
    {
        for (unsigned int i = 0; i < 8; i++)
            mThwomps[i].Grab(gNPCManager->GetThwomp(i));
    }
    if (mFlags.bits.flyingCameras)
    {
        mNumFlyingCameras = gFlyingCameraCount;
        for (i = 0; i < mNumFlyingCameras; i++)
            _2298[i].Grab();
        for (; i < 10; i++)
            _2298[i].mVisible = false;
    }
    if (mFlags.bits.bulletBills)
    {
        mNumBulletBills = gNPCManager->GetNumBulletBills();
        for (i = 0; i < mNumBulletBills; i++)
            mBulletBills[i].Grab(gNPCManager->GetBulletBill(i));
    }
    mChainChomp.Grab(*gNPCManager->GetChainChomp());
    if (mFlags.bits.yoshiEgg)
        mYoshiEgg.Grab(gNPCManager->mpYoshiEgg);
    if (mFlags.bits.birdoEgg)
        mBirdoEgg.Grab(gNPCManager->mpBirdoEgg);
    if (mFlags.bits.koopaShell)
        mKoopaShell.Grab(gNPCManager->mpKoopaShell);
    if (mFlags.bits.daisyFists)
    {
        mNumVisibleDaisyFists = gNPCManager->mNumVisibleDaisyFists;
        for (i = 0; i < 8; i++)
            mDaisyFists[i].Grab(gNPCManager->GetDaisyFist(i));
    }
    if (mFlags.bits.diddyBanana)
    {
        if (gNPCManager->mpDiddyBanana != 0)
            mDiddyBanana.Grab(*gNPCManager->mpDiddyBanana);
        else
            mDiddyBanana.visible = false;
    }
    if (mFlags.bits.windDebris)
    {
        for (i = 0; i < 3; i++)
        {
            if (gNPCManager->fn_801A9DE0(i) != 0)
                mWindDebris[i].Grab(*gNPCManager->fn_801A9DE0(i));
        }
    }
    mBall.Grab();
    mGoalLight = lbl_806E1960;
    if (NetMesh::s_bAnimatedNetMeshEnabled)
    {
        if (mpNetMeshPositiveX != 0)
            mpNetMeshPositiveX->Grab(*PhysicsNet::spPhysNetPositiveX->mpNetMesh);
        if (mpNetMeshNegativeX != 0)
            mpNetMeshNegativeX->Grab(*PhysicsNet::spPhysNetNegativeX->mpNetMesh);
        mPositiveGoalieNetCheck = Goalie::mbPosGoalieNetCheck;
        mNegativeGoalieNetCheck = Goalie::mbNegGoalieNetCheck;
    }
    mCameraUp = g_CameraWorldUpVector;
    if (lbl_806E12C8 != 0)
    {
        for (int i = 0; i < 60; i++)
        {
            PhysicsPatch* patch = lbl_806E12C8->fn_801745B8(i);
            if (patch != 0)
                mPatchPositions[i] = patch->GetPosition();
            else
                mPatchPositions[i].z = -10000.0f;
        }
    }
    mSimulationTime = GetFixedUpdateTask()->mSimulationTime;
    mValid = true;
}

DrawableBulletBill& GetSnapshotBulletBill(RenderSnapshot* snapshot, unsigned int index)
{
    return snapshot->mBulletBills[index];
}

int RenderSnapshot::NumDrawableObjects() const
{
    int count = 11;
    if (mChainChomp.visible)
    {
        count = 12;
    }

    if (mFlags.bits.diddyBanana && mDiddyBanana.visible)
    {
        count++;
    }

    if (mFlags.bits.windDebris)
    {
        for (int i = 0; i < 3; i++)
        {
            if (this->mWindDebris[i].visible)
            {
                count++;
            }
        }
    }

    for (int i = 0; i < 150; i++)
    {
        if (mPowerups[i].mVisible)
        {
            count++;
        }
    }

    if (mFlags.bits.flyingCameras)
    {
        count += this->mNumFlyingCameras;
    }

    return count;
}

const nlVector3* RenderSnapshot::GetPositionForDrawableObject(int index) const
{
    if (index == 0)
    {
        return &mBall.mPosition;
    }

    index--;
    if (index < 10)
    {
        return &mCharacters[index].position;
    }
    index -= 10;

    if (index == 0)
    {
        if (mChainChomp.visible)
        {
            return &mChainChomp.position;
        }
    }
    else
    {
        index--;
    }

    if (mFlags.bits.diddyBanana)
    {
        if (index == 0)
        {
            if (mDiddyBanana.visible)
            {
                return &mDiddyBanana.position;
            }
        }
        else
        {
            index--;
        }
    }

    if (mFlags.bits.windDebris)
    {
        for (int i = 0; i < 3; i++)
        {
            if (mWindDebris[i].visible == true)
            {
                if (index == 0)
                {
                    return &mWindDebris[i].position;
                }
                index--;
            }
        }
    }

    for (int i = 0; i < 150; i++)
    {
        if (mPowerups[i].mVisible)
        {
            if (index == 0)
            {
                return &mPowerups[i].mPosition;
            }
            index--;
        }
    }

    if (mFlags.bits.flyingCameras && index < (s32)mNumFlyingCameras)
    {
        return &_2298[index].mPosition;
    }

    return &sInvalidDrawablePosition;
}

void RenderSnapshot::Invalidate()
{
    mValid = false;
}

void RenderSnapshot::Render(float deltaTime)
{
    unsigned int i;
    bool allActorsHidden = false;
    if (g_AllActorsHidden > 0.0f)
    {
        g_AllActorsHidden -= deltaTime;
        allActorsHidden = true;
    }
    if (!mValid)
        return;
    if (!allActorsHidden)
    {
        mChainChomp.Render(*gNPCManager->GetChainChomp());
        if (mFlags.bits.yoshiEgg)
            mYoshiEgg.Render(gNPCManager->mpYoshiEgg);
        if (mFlags.bits.birdoEgg)
            mBirdoEgg.Render(gNPCManager->mpBirdoEgg);
        if (mFlags.bits.koopaShell)
            mKoopaShell.Render(gNPCManager->mpKoopaShell);
        if (mFlags.bits.daisyFists && mNumVisibleDaisyFists != 0)
        {
            for (i = 0; i < 8; i++)
                mDaisyFists[i].Render(gNPCManager->GetDaisyFist(i));
        }
        if (mFlags.bits.diddyBanana && gNPCManager->mpDiddyBanana != 0)
            mDiddyBanana.Render(*gNPCManager->mpDiddyBanana);
        if (mFlags.bits.bulletBills)
        {
            for (i = 0; i < mNumBulletBills; i++)
                mBulletBills[i].Render(gNPCManager->GetBulletBill(i));
        }
        if (mFlags.bits.windDebris)
        {
            for (i = 0; i < 3; i++)
            {
                if (mWindDebris[i].visible == true)
                    mWindDebris[i].Render(*gNPCManager->fn_801A9DE0(i));
            }
        }
        {
            NP_GS(NPGS_RC_CHARS);
            // MSC_SKIN_POSE_PAR (np/skinpose.h): what each character's Render decides is made for all ten first, the skin-mesh poses it names run on the job
            // lanes, then the draws follow in character order (their PoseSkinMesh calls find the poses done)
            const int skinMode = NpSkinPoseMode();
            if (skinMode != 0)
            {
                NpRenderPlan plans[10];
                bool ok[10];
                for (i = 0; i < 10; i++)
                {
                    plans[i].defer = true;
                    ok[i] = mCharacters[i].RenderDecide(*g_pCharacters[i], plans[i]);
                }
                NpSkinPoseRun(mCharacters, g_pCharacters, plans, ok, 10);
                for (i = 0; i < 10; i++)
                {
                    if (g_npGsOn && !mCharacters[i].poseCulled) NP_GSC(NPGC_DRAW_CHARS, 1);
                    if (skinMode == 2)
                    {
                        NpRenderPlan again;
                        again.defer = true;
                        const bool ok2 = mCharacters[i].RenderDecide(*g_pCharacters[i], again);
                        NpSkinPoseCheckDecision(ok[i], plans[i], ok2, again);
                    }
                    if (ok[i])
                        mCharacters[i].RenderDraw(*g_pCharacters[i], plans[i]);
                }
            }
            else
            for (i = 0; i < 10; i++)
            {
                if (g_npGsOn && !mCharacters[i].poseCulled) NP_GSC(NPGC_DRAW_CHARS, 1);
                mCharacters[i].Render(*g_pCharacters[i]);
            }
            if (NpSkinPoseDigestOn())
                NpSkinPoseDigest(g_pCharacters, 10);
        }
        {
            NP_GS(NPGS_RC_POWERUPS);
            for (i = 0; i < 150; i++)
                mPowerups[i].Render(i);
        }
        if (mFlags.bits.hammers && mHasHammers)
        {
            for (unsigned int i = 0; i < 15; i++)
                mHammers[i].Render(gNPCManager->GetHammer(i));
        }
        if (mFlags.bits.thwomps)
        {
            for (unsigned int i = 0; i < 8; i++)
                mThwomps[i].Render(gNPCManager->GetThwomp(i));
        }
        if (mFlags.bits.flyingCameras)
        {
            for (i = 0; i < mNumFlyingCameras; i++)
                _2298[i].Render();
        }
        mBall.Render();
    }
    mpNetMeshPositiveX->Render();
    mpNetMeshNegativeX->Render();
    if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY)
        cCameraManager::m_UpVectorStack[cCameraManager::m_UpVectorStackSize] = mCameraUp;
    static unsigned long goalLightTexture = glGetTexture("wario_stadium/goallight.ifl");
    GLTextureAnim* animation = glGetCurrentResourcePool()->m_inventory->GetTextureAnim(goalLightTexture);
    if (animation != 0)
        animation->m_bPaused = !mGoalLight;
}

void RenderSnapshot::RenderDebugInfo(
    const RenderSnapshot&, const RenderSnapshot&, float) const
{
}

// The head of the blend (the game thread's part, also run at the kick of MSC_BLEND_ASYNC), and the characters that must be blended here first.
static void NpBlendHead(RenderSnapshot& d, const float* blendFactors, RenderSnapshot& lhs, RenderSnapshot& rhs)
{
    d.mFrameBlendPercent = blendFactors[0];
    d.mValid = true;
    NP_GS(NPGS_RB_POSE);
    NpChoosePoseCull(d, blendFactors, lhs, rhs);
    if (g_npGsOn)
        for (int i = 0; i < 10; i++)
        {
            if (d.mCharacters[i].poseAccumulator != 0 && !d.mCharacters[i].poseCulled) NP_GSC(NPGC_BLEND_CHARS, 1);
            if (d.mCharacters[i].poseCulled) NP_GSC(NPGC_BLEND_ITEMS, 1);
        }
}
static unsigned int NpBlendSerialFirst(RenderSnapshot& d, const float* blendFactors, RenderSnapshot& lhs, RenderSnapshot& rhs)
{
    unsigned int serialMask = 0;
    for (int i = 0; i < 10; i++)
    {
        // (wave 9) also here: a Hammer Bro whose blend would make the one-time lookup of the prop joints (globals g_n*PropJointIndex, DrawableCharacter::Blend)
        const cCharacter* c = lhs.mCharacters[i].character;
        const bool firstHammer = g_nLeftPropJointIndex <= 0 && c != 0 && c->m_eClassType == FIELDER && c->m_DetChar.m_eCharacterClass == HAMMERBROS;
        if (d.mCharacters[i].poseAccumulator == 0 || firstHammer)
        {
            d.mCharacters[i].Blend(blendFactors, lhs.mCharacters[i], rhs.mCharacters[i]);
            serialMask |= 1u << i;
        }
    }
    return serialMask;
}

// gt27 (MSC_BLEND_ASYNC): the kick, from ReplayManager (NpBlendAsyncKick): the head and the first-use characters here, the rest on the helper lane. Returns false when nothing was kicked.
extern "C" int NpBlendAsyncOn()
{
    if (sBA.on < 0)
    {
        const char* e = getenv("MSC_BLEND_ASYNC");
        sBA.on = (e != 0 && *e != 0 && *e != '0') ? 1 : 0;
        const char* b = getenv("MSC_BLEND_ASYNC_BREAK");
        sBA.brk = (b != 0 && *b == '1') ? 1 : 0;
        if (sBA.on && !(&NpLaneForce != 0 && NpLaneForce() != 0 && &NpLaneTrySubmit != 0 && &NpLaneWait != 0))
        {
            fprintf(stderr, "[blendasync] MSC_BLEND_ASYNC=1 but the helper lane cannot start: the blend stays where it was\n");
            sBA.on = 0;
        }
        if (sBA.on) fprintf(stderr, "[blendasync] on (MSC_BLEND_ASYNC=1): live-play snapshot blends of the characters and power-ups run on the helper lane beside the world / crowd drawing%s\n", sBA.brk ? " (BREAK mutant: half factor)" : "");
    }
    return sBA.on;
}
bool NpBlendAsyncBegin(RenderSnapshot& d, const float* f, RenderSnapshot& lhs, RenderSnapshot& rhs)
{
    if (sBA.pending)
        return false;
    if (!(PortParallelSite(0) != 0 && PortParallelWorkers() != 0))   // (only where the old blend runs its parallel form: the same head, the same first-use set)
        return false;
    NP_GS(NPGS_RB_CHARS);
    NpBlendHead(d, f, lhs, rhs);
    const unsigned int serialMask = NpBlendSerialFirst(d, f, lhs, rhs);
    memcpy(sBA.factors, f, sizeof sBA.factors);
    sBA.brokenFactors[0] = sBA.brokenFactors[1] = sBA.brokenFactors[2] = 0.5f * f[0];
    sBA.dst = &d;
    sBA.lhs = &lhs;
    sBA.rhs = &rhs;
    sBA.job.dst = &d;
    sBA.job.lhs = &lhs;
    sBA.job.rhs = &rhs;
    sBA.job.factors = sBA.brk ? sBA.brokenFactors : sBA.factors;
    sBA.job.serialMask = serialMask;
    __atomic_store_n(&sNpShareNext, 0u, __ATOMIC_RELEASE);
    if (!NpLaneTrySubmit(NpBlendAsyncRun, &sBA.job))
    {
        ++sBA.notKicked;   // (the lane refused: Blend does it all the old way; the head made here is made again there, with the same result)
        return false;
    }
    sBA.pending = true;
    ++sBA.kicks;
    return true;
}
extern "C" void NpBlendAsyncDrain()   // a kicked blend nobody joined (the drawing did not reach Blend): finished, and forgotten
{
    if (!sBA.pending)
        return;
    { NpGsSeqW w(NPGS_WAIT_OTHER); NpLaneWait(); }
    sBA.pending = false;
    ++sBA.drained;
}
extern "C" void NpBlendAsyncCount(int notKicked) { if (notKicked) ++sBA.notKicked; }

void RenderSnapshot::Blend(const float* blendFactors, RenderSnapshot& lhs, RenderSnapshot& rhs)
{
    bool npAsyncTaken = false;
    if (sBA.pending)   // gt27 (MSC_BLEND_ASYNC): the lane's blend of this picture
    {
        if (NpBlendShareOn())   // sh18: what the lane has not started yet is blended here (same items, same factors), then only its current item is waited for
        {
            NpGsSeq gsShare(NPGS_RB_CHARS);
            sNpShareJoinItems += NpBlendClaimAll(&sBA.job);
            if (++sNpShareJoins % 1800 == 1)
                fprintf(stderr, "[blendshare] %lu joins: the game thread blended %.2f of the %u items a join, the lane the rest\n", sNpShareJoins, (double)sNpShareJoinItems / (double)sNpShareJoins, kNpShareItems);
        }
        if (NpBlendShareOn() && NpBlendTakeoverOn() && &NpLaneTakeUnstarted != 0)   // lane24 FIX-07 (MSC_BLEND_TAKEOVER, default 1, exact): a lane thread that is woken but not running yet is not waited for (see below)
        {
            NpGsSeq gsTake(NPGS_RB_CHARS);
            ++sNpTakeJoins;
            if (NpLaneTakeUnstarted() > 0 && (++sNpTakeHits % 600 == 1))
                fprintf(stderr, "[blendtake] %lu of %lu joins: the lane thread had not started the blend job, the game thread ran it (nothing was left to blend: the share counter had no item; the 12 ms stall wait is gone)\n", sNpTakeHits, sNpTakeJoins);
        }
        { NpGsSeqW w(NPGS_WAIT_OTHER); NpLaneWait(); }
        sBA.pending = false;
        if (sBA.dst == this && sBA.lhs == &lhs && sBA.rhs == &rhs && NpSameFactors(sBA.factors, blendFactors))
        {
            npAsyncTaken = true;
            ++sBA.taken;
        }
        else
        {
            ++sBA.mismatched;   // (made again below, the old way, over what the lane wrote)
        }
        if ((sBA.taken + sBA.mismatched) % 1800 == 1)
            fprintf(stderr, "[blendasync] %lu kicks: %lu taken at the join, %lu made again (target, snapshots or factor changed), %lu drained unused, %lu pictures not kicked\n", sBA.kicks, sBA.taken, sBA.mismatched, sBA.drained, sBA.notKicked);
    }
    NpGsSeq gsBlend(NPGS_RB_CHARS);
    if (!npAsyncTaken)
    {
    NpBlendHead(*this, blendFactors, lhs, rhs);
    if (PortParallelSite(0) != 0 && PortParallelWorkers() != 0)
    {
        const unsigned int serialMask = NpBlendSerialFirst(*this, blendFactors, lhs, rhs);
        NpBlendJob job = { this, &lhs, &rhs, blendFactors, serialMask };
        PortParallelFor(10, NpCharacterGrain(), NpBlendCharacters, &job);
        PortParallelFor(150, 48, NpBlendPowerups, &job);
        static unsigned long sParBlends;
        if (++sParBlends % 1800 == 1)
            fprintf(stderr, "[snappar] snapshot blends on the lanes: %lu (characters blended here first, this one: mask 0x%x)\n", sParBlends, serialMask);
    }
    else
    {
        for (int i = 0; i < 10; i++)
            mCharacters[i].Blend(blendFactors, lhs.mCharacters[i], rhs.mCharacters[i]);
        for (int i = 0; i < 150; i++)
            mPowerups[i].Blend(blendFactors, lhs.mPowerups[i], rhs.mPowerups[i]);
    }
    }
    gsBlend.Next(NPGS_R_BLEND);
    if (mFlags.bits.hammers)
    {
        mHasHammers = lhs.mHasHammers && rhs.mHasHammers;
        if (mHasHammers)
        {
            for (unsigned int i = 0; i < 15; i++)
                mHammers[i].Blend(blendFactors, lhs.mHammers[i], rhs.mHammers[i]);
        }
    }
    if (mFlags.bits.thwomps)
    {
        for (unsigned int i = 0; i < 8; i++)
            mThwomps[i].Blend(blendFactors, lhs.mThwomps[i], rhs.mThwomps[i]);
    }
    if (mFlags.bits.flyingCameras)
    {
        mNumFlyingCameras = lhs.mNumFlyingCameras <= rhs.mNumFlyingCameras ? lhs.mNumFlyingCameras : rhs.mNumFlyingCameras;
        for (unsigned int i = 0; i < mNumFlyingCameras; i++)
            _2298[i].Blend(blendFactors, lhs._2298[i], rhs._2298[i]);
    }
    mChainChomp.Blend(blendFactors, lhs.mChainChomp, rhs.mChainChomp);
    if (mFlags.bits.yoshiEgg)
        mYoshiEgg.Blend(blendFactors, lhs.mYoshiEgg, rhs.mYoshiEgg);
    if (mFlags.bits.birdoEgg)
        mBirdoEgg.Blend(blendFactors, lhs.mBirdoEgg, rhs.mBirdoEgg);
    if (mFlags.bits.koopaShell)
        mKoopaShell.Blend(blendFactors, lhs.mKoopaShell, rhs.mKoopaShell);
    if (mFlags.bits.daisyFists)
    {
        mNumVisibleDaisyFists = lhs.mNumVisibleDaisyFists >= rhs.mNumVisibleDaisyFists ? lhs.mNumVisibleDaisyFists : rhs.mNumVisibleDaisyFists;
        for (unsigned int i = 0; i < 8 && mNumVisibleDaisyFists != 0; i++)
            mDaisyFists[i].Blend(blendFactors, lhs.mDaisyFists[i], rhs.mDaisyFists[i]);
    }
    if (mFlags.bits.diddyBanana && gNPCManager->mpDiddyBanana != 0)
        mDiddyBanana.Blend(blendFactors, lhs.mDiddyBanana, rhs.mDiddyBanana);
    if (mFlags.bits.bulletBills)
    {
        mNumBulletBills = lhs.mNumBulletBills <= rhs.mNumBulletBills ? lhs.mNumBulletBills : rhs.mNumBulletBills;
        for (unsigned int i = 0; i < mNumBulletBills; i++)
            mBulletBills[i].Blend(blendFactors, lhs.mBulletBills[i], rhs.mBulletBills[i]);
    }
    if (mFlags.bits.windDebris)
    {
        for (int i = 0; i < 3; i++)
        {
            if (gNPCManager->fn_801A9DE0(i) != 0)
                mWindDebris[i].Blend(blendFactors, lhs.mWindDebris[i], rhs.mWindDebris[i]);
        }
    }
    mBall.Blend(blendFactors, lhs.mBall, rhs.mBall);
    mpNetMeshPositiveX->Blend(blendFactors[0], *lhs.mpNetMeshPositiveX, *rhs.mpNetMeshPositiveX);
    mpNetMeshNegativeX->Blend(blendFactors[0], *lhs.mpNetMeshNegativeX, *rhs.mpNetMeshNegativeX);
    nlVecLerp(mCameraUp, lhs.mCameraUp, rhs.mCameraUp, blendFactors[0]);
    nlVec3Normalize(mCameraUp, mCameraUp);
    if (lbl_806E12C8 != 0 && (nlTaskManager::m_pInstance->mCurrentState & 0x20018) == 0)
    {
        const RenderSnapshot& previous = lhs;
        const RenderSnapshot& current = rhs;
        for (int i = 0; i < 60; i++)
        {
            PhysicsPatch* patch = lbl_806E12C8->fn_801745B8(i);
            if (patch != 0)
            {
                if (previous.mPatchPositions[i].z > -100.0f && current.mPatchPositions[i].z > -100.0f)
                {
                    nlVector3 position;
                    nlVecLerp(position, previous.mPatchPositions[i], current.mPatchPositions[i], blendFactors[0]);
                    patch->m_SpawnPosition = position;
                }
                else
                    patch->m_SpawnPosition = patch->GetPosition();
            }
        }
    }
    static float sPreviousStadiumTime = -1.0f;
    mSimulationTime = (1.0f - blendFactors[0]) * lhs.mSimulationTime + blendFactors[0] * rhs.mSimulationTime;
    if (sPreviousStadiumTime > 0.0f)
    {
        float deltaTime = mSimulationTime - sPreviousStadiumTime;
        if (deltaTime > 0.0f)
            { NP_GS(NPGS_R_STADIUM); UpdateStadium(deltaTime); }
    }
    mGoalLight = rhs.mGoalLight;
    mPositiveGoalieNetCheck = rhs.mPositiveGoalieNetCheck;
    sPreviousStadiumTime = mSimulationTime;
    mNegativeGoalieNetCheck = rhs.mNegativeGoalieNetCheck;
}

RenderSnapshot& RenderSnapshot::GetMutable()
{
    mValid = true;
    return *this;
}
