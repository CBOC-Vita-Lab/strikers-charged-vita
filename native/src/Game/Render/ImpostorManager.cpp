#include "NL/nlDLListContainer.inl"
#include "Game/Render/ImpostorManager.h"

#include "Game/Render/Impostor.h"
#include "Game/Render/ImpostorCharacter.h"
#include "Game/CharacterEffects.h"
#include "Game/SharedStaticStorage.h"
#include "Game/TweakConfig.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
#include "NL/nlDebug.h"
#include "Game/TweakValueFloat.h"
#include "Game/TweakValueInt.h"
#include "Game/Render/ImpostorSprite.h"
#include "NL/gl/glView.h"
#include "np/crowdcut.h"
#include "np/crowdgov.h"
#include "np/fps.h"
#include "np/overlap.h"
#include "Game/PoseAccumulator.h"
#include "port/parallel.h"
#include <stdlib.h>
#include <stdio.h>

u8 gDisableImpostorBlending;
u8 gImpostorSpritesInvalid;

static int sImpostorAlphaTestReference = 0x80;
static int sImpostorUpdatePeriod = 1;
static int sImpostorThrottle = 1;   // W12 FG: the renderer's background step 'crowd refresh' makes the sprites update this many times less often
static float sDefaultImpostorSizeScale = 1.0f;

static GLMemoryRequirement sImpostorResourceRequirements[2] = {
    { GLM_Header, 0xC000 },
    { GLM_VertexData, 0x50000 },
};

TweakValueFloat sfImpostorSizeScale(
    "sfImpostorSizeScale", "Render/Impostor/Visual Tweaks");
TweakValueInt sNumImpostorsRendered(
    "sNumImpostorsRendered", gLastTweakCategory, 0);

ImpostorManager::ImpostorManager()
    : mEnabled(false)
    , mImpostors(0)
    , mNumUsed(0)
    , mCapacity(0)
    , mPadding010(0)
{
    mParentView = 0;
    mInitialized = false;
    mPadding035 = false;
    mHasClusters = false;
    mUpdateClusters = false;
    mCurrentResource = 0;
    mUseRenderCache = false;
    mLastRenderChecksum = 0;
    mFrameCount = 0;
    mCaptured = false;

    SetEnabled(false);
}

void ImpostorManager::Initialize(GLView* parentView, int capacity,
    const GLMemoryRequirement* requirements, int numRequirements,
    bool invalidateCaptureOnRender)
{
    mParentView = parentView;
    mImpostors = new (8, false) Impostor[capacity];
    mCapacity = capacity;
    mNumUsed = 0;
    mInitialized = true;
    sfImpostorSizeScale.value = sDefaultImpostorSizeScale;
    gImpostorSpritesInvalid = false;

    for (int i = 0; i < 2; ++i)
    {
        if (requirements == 0)
        {
            mResources[i] = glCreateResourcePool(
                sImpostorResourceRequirements, 2, "Impostors");
        }
        else
        {
            mResources[i] = glCreateResourcePool(
                requirements, numRequirements, "Impostors");
        }
        mResourceMarkers[i] = mResources[i]->MarkResource();
    }

    mUseRenderCache = false;
    mCaptured = false;
    mUpdateClusters = false;
    mHasClusters = false;
    mPadding035 = false;
    mInvalidateCaptureOnRender = invalidateCaptureOnRender;
    LoadTweakConfigFile("ini/ImpostorCharacterTweaks.ini",
        "/Render/Impostor/CharacterTweaks", false);
}

void ImpostorManager::InvalidateCapture()
{
    mCaptured = false;
}

void ImpostorManager::ResetImpostors()
{
    ++gNpCrowdStructGen;
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    while (it.hasNext())
    {
        (*it)->ReleaseSprites();
        it.Step();
    }

    for (int i = 0; i < mNumUsed; ++i)
    {
        mImpostors[i].Reset();
        ResetSpriteSlots();
    }

    mNumUsed = 0;
}

void ImpostorManager::Uninitialize()
{
    if (mImpostors != 0)
    {
        delete[] mImpostors;
        mImpostors = 0;
    }

    mCharacters.Free();

    BasicSlotPool<DLListEntry<ImpostorCharacter*> >* pool =
        &mCharacters.m_Allocator;
    pool->FreeBlocks();

    glDestroyResourcePool(mResources[0]);
    glDestroyResourcePool(mResources[1]);
    mInitialized = false;
}

void ImpostorManager::ResetSpriteSlots()
{
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    while (it.hasNext())
    {
        nlDLListIterator<ImpostorSprite*> sprites;
        sprites = (*it)->mSprites.Begin();
        while (sprites.hasNext())
        {
            ImpostorSprite* sprite = *sprites;
            sprite->ClearRenderSlots();
            if (gImpostorSpritesInvalid != 0)
            {
                sprite->QueueAllSlots();
            }
            sprites.Step();
        }
        it.Step();
    }
}

ImpostorManager* ImpostorManager::GetInstance()
{
    static ImpostorManager sInstance;
    return &sInstance;
}

ImpostorManager::~ImpostorManager()
{
}

int ImpostorManager::GetNumImpostors()
{
    return mNumUsed;
}

static inline u32 AccumulateRenderChecksums(
    const nlDLListSlotPool<ImpostorCharacter*>& characters, u32 total)
{
    nlDLListIterator<ImpostorCharacter*> it;
    it = characters.Begin();
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    while (entry != 0)
    {
        nlDLListIterator<ImpostorSprite*> sprites;
        sprites = entry->entry->mSprites.Begin();
        DLListEntry<ImpostorSprite*>* spriteEntry = sprites.m_Curr;
        DLListEntry<ImpostorSprite*>* spriteHead = sprites.m_Head;
        while (spriteEntry != 0)
        {
            total += spriteEntry->entry->CalculateRenderChecksum();
            if (nlDLRingIsEnd(spriteHead, spriteEntry) || spriteEntry == 0)
            {
                spriteEntry = 0;
            }
            else
            {
                spriteEntry = spriteEntry->m_next;
            }
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
    return total;
}

void ImpostorManager::Render(GLView* target, bool skipCapture)
{
    u32 total;
    if (mEnabled == 0)
    {
        return;
    }

    if (mInvalidateCaptureOnRender != 0)
    {
        mCaptured = false;
    }

    bool cached = false;
    if (mUseRenderCache != 0 && !skipCapture)
    {
        cached = true;
    }

    if (cached)
    {
        total = AccumulateRenderChecksums(mCharacters, 0);
        if (total != mLastRenderChecksum)
        {
            cached = false;
            mLastRenderChecksum = total;
        }
    }

    if (NpCrowdGT() && !skipCapture) NpCrowdGTCount(cached ? 4 : 5, 1);   // (the counters of MSC_CROWD_GT)
    if (!cached && !skipCapture && mCaptured == 0)
    {
        ImpostorManager* instance = GetInstance();
        instance->mCurrentResource = (instance->mCurrentResource + 1) % 2;
        instance->mResources[instance->mCurrentResource]->ReleaseResource(
            instance->mResourceMarkers[instance->mCurrentResource]);
        instance->mCaptured = true;
        instance->mResourceMarkers[instance->mCurrentResource] =
            instance->mResources[instance->mCurrentResource]->MarkResource();
        u32* marker = (u32*)instance->mResourceMarkers[instance->mCurrentResource];
        if (marker[0] != 0 || marker[1] != 0)
        {
            nlBreak();
        }
    }

    sNumImpostorsRendered = 0;
    glSetDefaultState(true);
    glSetRasterState(GLS_DepthWrite, 1);
    glSetRasterState(GLS_Culling, 0);
    glSetRasterState(GLS_DepthTest, 1);
    glSetRasterState(GLS_AlphaBlend, gDisableImpostorBlending == 0);
    if (gDisableImpostorBlending == 0)
    {
        glSetRasterState(GLS_AlphaTestRef, sImpostorAlphaTestReference);
        glSetRasterState(GLS_AlphaTest, 1);
    }
    glSetCurrentRasterState(glHandleizeRasterState());

    int rendered;
    DLListEntry<ImpostorSprite*>* spriteEntry;
    ImpostorCharacter* character;
    nlDLListIterator<ImpostorCharacter*> drawIt;
    drawIt = mCharacters.Begin();
    for (; drawIt.hasNext(); drawIt.next())
    {
        character = *drawIt;
        if (character->mUseAdditiveBlend != 0)
        {
            glSetRasterState(GLS_DepthTest, 1);
            glSetRasterState(GLS_DepthWrite, 0);
            glSetRasterState(GLS_AlphaBlend,
                gDisableImpostorBlending != 0 ? GLB_None : GLB_Additive);
            glSetCurrentRasterState(glHandleizeRasterState());
        }
        else
        {
            glSetRasterState(GLS_DepthTest, 1);
            glSetRasterState(GLS_AlphaBlend, gDisableImpostorBlending == 0);
            glSetCurrentRasterState(glHandleizeRasterState());
        }

        nlDLListIterator<ImpostorSprite*> sprites;
        sprites = character->mSprites.Begin();
        DLListEntry<ImpostorSprite*>* spriteHead = sprites.m_Head;
        spriteEntry = sprites.m_Curr;
        while (spriteEntry != 0)
        {
            rendered = sNumImpostorsRendered.mValue;
            sNumImpostorsRendered.mValue = rendered
                + spriteEntry->entry->Render(target, mImpostors, cached, skipCapture);
            if (nlDLRingIsEnd(spriteHead, spriteEntry) || spriteEntry == 0)
            {
                spriteEntry = 0;
            }
            else
            {
                spriteEntry = spriteEntry->m_next;
            }
        }
    }

    mFrameCount++;
}

Impostor* ImpostorManager::AllocImpostor(int* outIndex)
{
    ++gNpCrowdStructGen;
    int index = mNumUsed;
    if (index == mCapacity)
    {
        return 0;
    }

    Impostor* impostor = &mImpostors[index];
    impostor->mSlot = index;
    mNumUsed++;
    *outIndex = index;
    return impostor;
}

void ImpostorManager::AddCharacter(ImpostorCharacter* character)
{
    mCharacters.AddEnd(character);
    character->RegisterSprites(mParentView);
    if (character->mIsCluster != 0)
    {
        mHasClusters = true;
    }
}

extern bool gCrowdGovActive;      // ImpostorCharacter.cpp
extern unsigned gCrowdGovTick;
extern unsigned gCrowdGovWake;
extern unsigned gCrowdGovBankTick;
extern bool gCrowdGovBank;

void ImpostorManager::UpdateCharacters(float blendTime, const char* name)
{
    gCrowdGovWake = gCrowdGovTick;   // MSC_CROWD_GOV: an animation change: the crowd is left alone for a moment (ImpostorCharacter.cpp)
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    while (entry != 0)
    {
        entry->entry->PlayAnimation(blendTime, name);
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

void ImpostorManager::UpdateAnimations(float dt)
{
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    while (entry != 0)
    {
        entry->entry->UpdateAnimation(dt);
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

static int sUpdateSlot;
static bool sGtLazy;            // MSC_CROWD_GT: sprite views are made at capture
static nlVector3 sGtDirection;
static nlVector3 sGtUp;
static bool sSlotOfFloor;   // the slot counts in the period of UpdateSpritesWithFloor
static bool sSkipClusters;  // MSC_CROWD_CLUSTER_LAZY: nothing reads the cluster's picture

void ImpostorManager::SetSkipClusters(bool skip)
{
    sSkipClusters = skip;
}

static int sGovLastTarget;
static unsigned sGovChangeTick;
static bool sGovOff;                // some sprite is still off level 0

// crowd25 (MSC_CROWD_GOV): called by the gameplay camera's crowd stage with the governor's level, after the first visibility pass. Nothing outside UpdateSpritesWithFloor sees the governor.
void ImpostorManager::SetCrowdGov(int level, GLView* view)
{
    if (level <= 0 && !sGovOff)
    {
        gCrowdGovBank = false;
        return;
    }
    if (level != sGovLastTarget)
    {
        sGovLastTarget = level;
        sGovChangeTick = gCrowdGovTick;
    }
    gCrowdGovActive = true;
    nlMatrix4 viewMatrix, projection;
    view->m_Interface->GetViewMatrix(viewMatrix);
    view->m_Interface->GetProjectionMatrix(projection);
    NpCrowdViewInfo vi;
    vi.depthRow[0] = viewMatrix.e2[0][2];
    vi.depthRow[1] = viewMatrix.e2[1][2];
    vi.depthRow[2] = viewMatrix.e2[2][2];
    vi.depthRow[3] = viewMatrix.e2[3][2];
    vi.projY = projection.e2[1][1];
    vi.figureScale = GetImpostorSizeScale();
    bool off = false;
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    while (it.hasNext())
    {
        if ((*it)->mIsCluster == 0)
        {
            (*it)->MeasureNear(mImpostors, vi, gCrowdGovTick);
            off |= (*it)->GovFollow(level, gCrowdGovTick, gCrowdGovTick - sGovChangeTick);
        }
        it.Step();
    }
    sGovOff = off || level > 0;
    gCrowdGovBank = sGovOff;
    gCrowdGovBankTick = gCrowdGovTick;
}

void ImpostorManager::UpdateSpritesWithFloor(int floor)
{
    const int throttle = sImpostorThrottle;
    if (floor > throttle)
    {
        SetUpdateThrottle(floor);
    }
    sSlotOfFloor = false;
    // (a floor set under a camera of another period: the slot must lie in this one, or no sprite would update this frame)
    sUpdateSlot %= sImpostorUpdatePeriod * sImpostorThrottle;
    UpdateSprites();
    gCrowdGovActive = false;
    sSlotOfFloor = floor > throttle;
    sImpostorThrottle = throttle;
}

static ImpostorSprite* sPreparedSprites[160];   // the sprites whose view a job made on this picture (taken down at the end of UpdateSprites)
static int sNumPrepared;

namespace
{
struct PoseJob
{
    ImpostorModel* models[96];
    ImpostorSprite* sprites[160];
    int count;
    int nSprites;
    const nlVector3* direction;
    const nlVector3* up;
};
int PoseJobRange(void* opaque, size_t begin, size_t end, uint32_t lane)
{
    PoseJob& job = *static_cast<PoseJob*>(opaque);
    (void)lane;
    for (size_t i = begin; i < end; ++i)
    {
        if (i < (size_t)job.count)
        {
            if (NpPoseJobsBreak() && (i % 3) == 2) continue;   // (proof mutant: every third model keeps last picture's pose)
            job.models[i]->PoseForJob(true);
        }
        else
        {
            job.sprites[i - job.count]->UpdateView(job.direction, job.up);   // (MSC_CROWD_GT bit 1's view of a captured sprite: its own matrices only)
        }
    }
    return 1;
}
} // namespace

void ImpostorManager::PoseCapturedModels()
{
    PoseJob job;
    job.count = 0;
    job.nSprites = 0;
    job.direction = &sGtDirection;
    job.up = &sGtUp;
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorCharacter* character = entry->entry;
        if ((mUpdateClusters == 0 || sSkipClusters) && character->mIsCluster != 0)
        {
            break;
        }
        character->CollectPoseModels(sImpostorUpdatePeriod * sImpostorThrottle, sUpdateSlot, sGtLazy && character->mIsCluster == 0, job.models, &job.count, 96, job.sprites, &job.nSprites, 160);
        if (nlDLRingIsEnd(head, entry) || entry == 0) entry = 0;
        else entry = entry->m_next;
    }
    if (job.count == 0 && job.nSprites == 0) return;
    // (an impostor model's accumulator keeps last picture's matrices too, and every build swaps the two buffers; nothing reads the previous ones but Character.cpp, which has its own accumulator: how often a
    // picture builds only decides which of the two holds the current pose)
    PortParallelFor((size_t)(job.count + job.nSprites), 1, PoseJobRange, &job);
    for (int i = 0; i < job.count; ++i) ImpostorModel::MarkPosed(job.models[i]);
    for (int i = 0; i < job.nSprites; ++i)
    {
        job.sprites[i]->mViewPrepared = true;
        sPreparedSprites[sNumPrepared++] = job.sprites[i];
    }
    NpPoseJobsCount(0, job.count);
    NpPoseJobsCount(1, 1);
}

void ImpostorManager::UpdateSprites()
{
    // the next caller without a floor (the front end needs every sprite on the frame it asks) must not start from a slot beyond its own period
    if (sSlotOfFloor)
    {
        sSlotOfFloor = false;
        sUpdateSlot %= sImpostorUpdatePeriod * sImpostorThrottle;
    }

    // MSC_POSE_JOBS bit 1 (wave 12): the poses of the models this picture captures are made on the job lanes first (evaluate, node matrices, skin pose: about three quarters of this
    // function's work); the loop below then only builds each sprite's packets. Each model's pose is the same function of the same inputs, so the matrices are the same bit for bit.
    if ((NpPoseJobs() & 1) != 0 && NpCrowdPoseOnce() && PortParallelWorkers() != 0)
    {
        PoseCapturedModels();
    }

    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorCharacter* character = entry->entry;
        if ((mUpdateClusters == 0 || sSkipClusters) && character->mIsCluster != 0)
        {
            break;
        }
        character->UpdateSprites(sImpostorUpdatePeriod * sImpostorThrottle, sUpdateSlot, sGtLazy && character->mIsCluster == 0, &sGtDirection, &sGtUp);
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }

    ImpostorModel::ClearPosed();
    for (int i = 0; i < sNumPrepared; ++i) sPreparedSprites[i]->mViewPrepared = false;
    sNumPrepared = 0;
    sUpdateSlot = (sUpdateSlot + 1) % (sImpostorUpdatePeriod * sImpostorThrottle);
    ++gCrowdGovTick;
}

float ImpostorManager::GetImpostorSizeScale()
{
    return sfImpostorSizeScale.value;
}

void ImpostorManager::SetImpostorSizeScale(float scale)
{
    sfImpostorSizeScale.value = scale;
}

static bool sSizedOn;              // MSC_CROWD_SIZED: the pictures of this stretch are chosen by size
static unsigned sSizedFrame;       // sized pictures so far
static nlVector3 sSizedDirection;  // MSC_CROWD_LAZYVIEW: the view the sprites are drawn from, kept for the ones drawn
static nlVector3 sSizedUp;
static bool sSizedLazy;
static NpCrowdViewInfo sSizedView;

namespace
{
const int kCrowdPosParMax = 96;
unsigned long sCrowdPosParCalls, sCrowdPosParSerial;
struct CrowdPosJob
{
    ImpostorCharacter* const* list;
    const nlVector3* direction;
    const nlVector3* up;
};
// MSC_CROWD_POS_PAR_BREAK=1 (proof only): the worker lanes use a wrong up vector, so the frame hashes must change
int CrowdPosParBreak()
{
    static int v = -1;
    if (v < 0)
    {
        const char* e = getenv("MSC_CROWD_POS_PAR_BREAK");
        v = (e != 0 && *e == '1') ? 1 : 0;
    }
    return v;
}
int CrowdPosRange(void* opaque, size_t begin, size_t end, uint32_t lane)
{
    const CrowdPosJob& job = *static_cast<const CrowdPosJob*>(opaque);
    nlVector3 brokenUp = { 0.1f, 0.0f, 1.0f };
    const nlVector3* up = (lane != 0 && CrowdPosParBreak()) ? &brokenUp : job.up;
    for (size_t i = begin; i < end; ++i)
    {
        job.list[i]->UpdateView(job.direction, up);
    }
    return 1;
}
} // namespace

void ImpostorManager::UpdatePositions(const nlVector3* direction, const nlVector3* up)
{
    sSizedLazy = false;
    sGtLazy = false;
    if ((NpCrowdGT() & 1) && !sSizedOn)
    {
        // MSC_CROWD_GT: a sprite's view matrices are only read by the capture of that sprite (a sprite whose view target is None is not recorded), so they are
        // made where the sprite is captured, from this direction (UpdateSprites). A cluster keeps its own call (a fixed direction).
        static int sBreakPhase;
        if (!NpCrowdGTBreak() || (++sBreakPhase & 1))   // (the break variant keeps last picture's direction on every second picture)
        {
            sGtDirection = *direction;
            sGtUp = *up;
        }
        sGtLazy = true;
        sSizedLazy = false;
        nlDLListIterator<ImpostorCharacter*> cit;
        cit = mCharacters.Begin();
        while (cit.hasNext())
        {
            if ((*cit)->mIsCluster != 0)
                (*cit)->UpdateView(direction, up);
            cit.Step();
        }
        return;
    }
    if (sSizedOn && NpCrowdLazyView())
    {
        // the view matrices of a sprite are computed where the sprite is drawn (ImpostorCharacter::UpdateSpritesSized); a cluster's are not the camera's, so it keeps its own call
        sSizedDirection = *direction;
        sSizedUp = *up;
        sSizedLazy = true;
    }
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    // MSC_CROWD_POS_PAR (wave 9, platform/vita/src/np_port_parallel.cpp, default 0): the same UpdateView calls, one crowd character per item, on the job
    // pool's lanes. UpdateView writes only the character's own sprites' ImpostorView matrices and reads the two vectors, the character's scale and
    // camera distance and constant tweak values, so the order does not matter and the matrices are the same bit for bit.
    if (PortParallelSite(1) != 0 && PortParallelWorkers() != 0)
    {
        ImpostorCharacter* list[kCrowdPosParMax];
        int count = 0;
        bool fits = true;
        for (DLListEntry<ImpostorCharacter*>* e = entry; e != 0;)
        {
            if (!sSizedLazy || e->entry->mIsCluster != 0)
            {
                if (count == kCrowdPosParMax)
                {
                    fits = false;
                    break;
                }
                list[count++] = e->entry;
            }
            e = (nlDLRingIsEnd(head, e) || e == 0) ? 0 : e->m_next;
        }
        if (fits)
        {
            CrowdPosJob job = { list, direction, up };
            PortParallelFor((size_t)count, 2, CrowdPosRange, &job);
            if (++sCrowdPosParCalls % 1800 == 1)
            {
                fprintf(stderr, "[snappar] crowd views: %lu pictures on the lanes (%d characters this one), %lu serial (table full)\n", sCrowdPosParCalls, count,
                    sCrowdPosParSerial);
            }
            return;
        }
        ++sCrowdPosParSerial;   // more characters than the table holds: the original loop below
    }
    while (entry != 0)
    {
        if (!sSizedLazy || entry->entry->mIsCluster != 0)
        entry->entry->UpdateView(direction, up);
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

void ImpostorManager::StaggerAnimations()
{
    float phase = 0.0f;
    int count = mCharacters.CountElements();
    float step = 1.0f / (4.0f * (float)count);

    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorCharacter* character = entry->entry;
        int numTextures = character->mNumTextures;
        for (int i = 0; i < numTextures; ++i)
        {
            float value = (float)i / (float)(numTextures * 4);
            value += phase;
            while (value > 1.0f)
            {
                value -= 1.0f;
            }
            character->SetAnimationTime(i, value);
        }
        phase += step;
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

void ImpostorManager::SetEnabled(bool enable)
{
    mEnabled = enable;
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    while (it.hasNext())
    {
        (*it)->EnableSprites(enable);
        it.Step();
    }
}

void ImpostorManager::SetSpritesInvalid()
{
    gImpostorSpritesInvalid = true;
}

void ImpostorManager::SetUpdateThrottle(int throttle)
{
    sImpostorThrottle = throttle < 1 ? 1 : (throttle > 4 ? 4 : throttle);
}

void ImpostorManager::SetUpdatePeriod(int period)
{
    sImpostorUpdatePeriod = period;
}

void ImpostorManager::SetSizedMode(bool on)
{
    if (on && !sSizedOn)
    {
        // a stretch begins: every sprite is drawn once on its first picture (its picture is as old as the last gameplay frame that had it in view, or never drawn)
        nlDLListIterator<ImpostorCharacter*> it;
        it = mCharacters.Begin();
        while (it.hasNext())
        {
            nlDLListIterator<ImpostorSprite*> sprites;
            sprites = (*it)->mSprites.Begin();
            while (sprites.hasNext())
            {
                (*sprites)->mSeenLast = false;
                (*sprites)->mSizedStep = 0;
                (*sprites)->mNearFrame = -1;
                sprites.Step();
            }
            it.Step();
        }
    }
    sSizedOn = on;
    if (on) sGtLazy = false;
}

void ImpostorManager::MeasureSizes(GLView* view)
{
    nlMatrix4 viewMatrix, projection;
    view->m_Interface->GetViewMatrix(viewMatrix);
    view->m_Interface->GetProjectionMatrix(projection);
    sSizedView.depthRow[0] = viewMatrix.e2[0][2];
    sSizedView.depthRow[1] = viewMatrix.e2[1][2];
    sSizedView.depthRow[2] = viewMatrix.e2[2][2];
    sSizedView.depthRow[3] = viewMatrix.e2[3][2];
    sSizedView.projY = projection.e2[1][1];
    sSizedView.figureScale = GetImpostorSizeScale();
    ++sSizedFrame;

    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    while (it.hasNext())
    {
        if ((*it)->mIsCluster == 0)
        {
            (*it)->MeasureSprites(mImpostors, sSizedView, sSizedFrame);
        }
        it.Step();
    }
}

void ImpostorManager::UpdateSpritesSized()
{
    int inView = 0, groups = 0, drawn = 0;
    int bins[7] = { 0, 0, 0, 0, 0, 0, 0 };
    const int minStep = sImpostorThrottle > 3 ? 3 : sImpostorThrottle;
    nlDLListIterator<ImpostorCharacter*> it;
    it = mCharacters.Begin();
    DLListEntry<ImpostorCharacter*>* head = it.m_Head;
    DLListEntry<ImpostorCharacter*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorCharacter* character = entry->entry;
        if (sSkipClusters && character->mIsCluster != 0)
        {
            break;
        }
        drawn += character->UpdateSpritesSized(sSizedFrame, minStep, NpFpsPolicy() >= 2, sSizedLazy, &sSizedDirection, &sSizedUp, &inView, &groups);
        if (character->mIsCluster == 0)
        {
            nlDLListIterator<ImpostorSprite*> sprites;
            sprites = character->mSprites.Begin();
            while (sprites.hasNext())
            {
                const ImpostorSprite* sprite = *sprites;
                if (sprite->mNumRenderSlots > 0)
                {
                    const float px = sprite->mNearPx;
                    ++bins[px < 10.0f ? 0 : px < 20.0f ? 1 : px < 40.0f ? 2 : px < 80.0f ? 3 : px < 160.0f ? 4 : px < 320.0f ? 5 : 6];
                }
                sprites.Step();
            }
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
    NpCrowdCountPicture(inView, drawn, groups, bins);
}
