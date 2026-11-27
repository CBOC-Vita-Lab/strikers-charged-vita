#include "NL/nlDLListContainer.inl"
#include "Game/Render/CrowdImpostorManager.h"
#include "Game/Render/Frustum.h"
#include "NL/gl/glView.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "Game/MathHelpers.h"
#include "Game/Render/ImpostorManager.h"
#include "Game/Render/ImpostorSprite.h"
#include "np/crowdcut.h"
#include "Game/Render/MegastrikeBackgroundOverlay.h"
#include "Game/Task/GameTaskState.h"
#include "Game/Game.h"

#include <stdlib.h>
#include "Game/SharedStaticStorage.h"
#include "Game/TweakConfig.h"
#include "Game/TweakValue.h"
#include "Game/TweakValueFloat.h"
#include "NL/nlDLListContainer.h"
#include "NL/nlMemory.h"
#include "NL/platvmath.h"

static TweakValueFloat sfDistanceBetweenCrowdRows(
    "sfDistanceBetweenCrowdRows", "/Render/Crowd/Layout", 0.75f);
static TweakValueFloat sfDistanceBetweenCrowdMembers(
    "sfDistanceBetweenCrowdMembers", "/Render/Crowd/Layout", 0.75f);
static TweakValueFloat sfVerticalJitterFraction(
    "sfVerticalJitterFraction", "/Render/Crowd/Layout", 0.4f);
static TweakValueFloat sfHorizontalJitterFraction(
    "sfHorizontalJitterFraction", "/Render/Crowd/Layout", 0.4f);
static TweakValueFloat sfImpostorWidth(
    "sfImpostorWidth", "/Render/Crowd", 1.0f);
static TweakValueFloat sfImpostorHeight(
    "sfImpostorHeight", "/Render/Crowd", 1.5f);

static bool sCrowdRegistrationDisabled;

#ifdef NP_NATIVE
// MSC_CROWD_OCC_CENSUS=1 (gpu24, counters only, default 0: nothing below changes a draw of a random number or a placed figure): at every stadium load, how many of the points the
// layout offers the retail occlusion test (every volume tested against the LAST volume's inverse matrix, mInverseMatrices[0]) removes, and how many a per-volume test would, and how
// they overlap. Printed once per GenerateCrowd: `[crowdocc] ...`. No effect on the simulation, replays or the network: reads matrices, writes only these counters.
static bool NpOccCensusOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("MSC_CROWD_OCC_CENSUS");
        on = (e && e[0] == '1') ? 1 : 0;
    }
    return on == 1;
}
enum { kOccAllMax = 32 };
static nlMatrix4 sOccAll[kOccAllMax];
static int sOccAllN;
static unsigned sOccSeats, sOccBuggy, sOccFixed, sOccBoth, sOccOnlyBuggy, sOccOnlyFixed;
#endif
static int sNumGeneratedCrowdPoints;
static int sNumCrowdImpostorsInSelectedLayouts;

class CrowdPointCallbackBase
{
public:
    CrowdPointCallbackBase(
        CrowdLayoutObject* object)
    {
        mObject = object;
    }

    virtual void Place(nlVector4 point) = 0;

    /* 0x04 */ CrowdLayoutObject* mObject;
}; // size: 0x08

class CrowdPointCallback : public CrowdPointCallbackBase
{
public:
    CrowdPointCallback(CrowdLayoutObject* object)
        : CrowdPointCallbackBase(object)
    {
        mFirst = true;
    }

    virtual void Place(nlVector4 point);
    void UpdateBounds(const nlVector4& worldPoint);

    /* 0x08 */ bool mFirst;
    /* 0x09 */ u8 mPadding009[3];
    /* 0x0C */ CrowdLayoutRecord* mLayout;
}; // size: 0x10

CrowdImpostorManager::CrowdImpostorManager()
{
    mLayouts = 0;
    mPrimaryObjectCount = 0;
    mNumLayouts = 0;
    mNumAngles = 0;
}

CrowdImpostorManager* GetCrowdImpostorManager()
{
    static CrowdImpostorManager manager;
    return &manager;
}

CrowdImpostorManager::~CrowdImpostorManager()
{
}

void CrowdImpostorManager::AddObject(CrowdLayoutObject* object, bool enabled)
{
    if (sCrowdRegistrationDisabled)
        return;

    if (enabled)
    {
        mEnabledObjects.AddEnd(object);
        ++mEnabledObjectCount;
    }

    if (object->mIsOcclusionVolume != 0)
    {
        mOcclusionObjects.AddEnd(object);
        ++mOcclusionObjectCount;
    }
    else
    {
        mPrimaryObjects.AddEnd(object);
        ++mPrimaryObjectCount;
    }
}

void CrowdImpostorManager::AddCharacter(ImpostorCharacter* character)
{
    mCharacters.AddEnd(character);
}

void CrowdImpostorManager::GenerateCrowd(int reload)
{
    if (reload == 0)
        LoadTweakConfigFile("ini/Crowd.ini", "/Render", false);

    sNumGeneratedCrowdPoints = 0;
    if (mPrimaryObjects.m_Head != 0)
        ImpostorManager::GetInstance()->SetEnabled(true);

    mLayouts = new (8, false)
        CrowdLayoutRecord[mPrimaryObjectCount];
    mInverseMatrices
        = new (8, false) nlMatrix4[mOcclusionObjectCount];

#ifdef NP_NATIVE
    sOccAllN = 0;
    sOccSeats = sOccBuggy = sOccFixed = sOccBoth = sOccOnlyBuggy = sOccOnlyFixed = 0;
#endif
    nlDLListIterator<CrowdLayoutObject*> occlusionIt;
    occlusionIt = mOcclusionObjects.Begin();
    while (occlusionIt.m_Curr != 0)
    {
        CrowdLayoutObject* object
            = occlusionIt.m_Curr->entry;
        nlInvertMatrix(*mInverseMatrices,
            *object->GetWorldMatrix());
#ifdef NP_NATIVE
        if (NpOccCensusOn() && sOccAllN < kOccAllMax)
            nlInvertMatrix(sOccAll[sOccAllN++], *object->GetWorldMatrix());
#endif
        occlusionIt.Step();
    }

    nlDLListIterator<CrowdLayoutObject*> objectIt;
    objectIt = mPrimaryObjects.Begin();
    while (objectIt.m_Curr != 0)
    {
        CrowdLayoutObject* object = objectIt.m_Curr->entry;
        CrowdPointCallback callback(object);
        callback.mLayout = GetCrowdImpostorManager()->AllocateLayout();
        callback.mLayout->mObject = object;
        sNumGeneratedCrowdPoints += object->PlacePoints(&callback,
            sfDistanceBetweenCrowdRows,
            sfDistanceBetweenCrowdMembers);
        objectIt.Step();
    }
#ifdef NP_NATIVE
    if (NpOccCensusOn())
        fprintf(stderr, "[crowdocc] occlusion volumes %d (primary layouts %d): seats offered %u, removed by the retail test (last volume's matrix for all) %u, by a per-volume test %u, both %u, only retail %u, only per-volume %u; "
                        "impostors made as retail %u, with a per-volume test %u\n",
                mOcclusionObjectCount, mPrimaryObjectCount, sOccSeats, sOccBuggy, sOccFixed, sOccBoth, sOccOnlyBuggy, sOccOnlyFixed, sOccSeats - sOccBuggy, sOccSeats - sOccFixed);
#endif
}

void CrowdImpostorManager::Clear()
{
    ++gNpCrowdStructGen;
    BasicSlotPool<DLListEntry<CrowdLayoutObject*> >* objectPool;

    mPrimaryObjects.Clear();
    objectPool = &mPrimaryObjects.m_Allocator;
    objectPool->FreeBlocks();
    mPrimaryObjectCount = 0;

    mOcclusionObjects.Clear();
    objectPool = &mOcclusionObjects.m_Allocator;
    objectPool->FreeBlocks();
    mOcclusionObjectCount = 0;

    mEnabledObjects.Clear();
    objectPool = &mEnabledObjects.m_Allocator;
    objectPool->FreeBlocks();
    mEnabledObjectCount = 0;

    mCharacters.Clear();
    mCharacters.m_Allocator.FreeBlocks();
    mNumVisibilityFilters = 0;

    if (mLayouts != 0)
    {
        delete mLayouts;
        mLayouts = 0;
    }

    mNumLayouts = 0;
    if (mInverseMatrices != 0)
        delete mInverseMatrices;
}

int CrowdLayoutObject::PlacePoints(CrowdPointCallback* callback, float rowSpacing, float memberSpacing)
{
    nlVector4 corners[4];
    int total = 0;
    GetCorners(corners);

    int numRows = (int)floorf(mLength / rowSpacing) + 1;

    for (int row = 0; row < numRows; ++row)
    {
        float rowStep = mLength;
        rowStep = rowSpacing / rowStep;
        float occupiedLength = (numRows - 1) * rowSpacing;
        float rowOffset = 0.5f
            * (mLength - occupiedLength)
            / mLength;
        float rowAmount[1] = { row * rowStep + rowOffset };
        nlVector4 right;
        nlVector4 left;
        nlVec4Scale(left, corners[0], rowAmount[0]);
        nlVec4ScaleAdd(left, 1.0f - rowAmount[0],
            corners[3], left);
        nlVec4Scale(right, corners[1], rowAmount[0]);
        nlVec4ScaleAdd(right, 1.0f - rowAmount[0],
            corners[2], right);

        float rowLength = nlSqrt(
            CalculateDistanceSquared(*(const nlVector3*)&left,
                *(const nlVector3*)&right),
            true);
        int numMembers = (int)floorf(rowLength / memberSpacing) + 1;
        float memberStep = memberSpacing / rowLength;

        for (int member = 0; member < numMembers; ++member)
        {
            float occupiedWidth = (numMembers - 1) * memberSpacing;
            float memberOffset
                = 0.5f * (rowLength - occupiedWidth)
                / rowLength;
            float memberAmount[1] = { member * memberStep + memberOffset };
            nlVector4 point;
            nlVec4Scale(point, left, memberAmount[0]);
            nlVec4ScaleAdd(point, 1.0f - memberAmount[0],
                right, point);
            callback->Place(point);
            ++total;
        }
    }

    return total;
}

void CrowdImpostorManager::AddVisibilityFilter(CrowdSidelineFilter* filter)
{
    if (mNumVisibilityFilters >= 5)
        return;

    mVisibilityFilters[mNumVisibilityFilters] = filter;
    ++mNumVisibilityFilters;
}

static bool IsCrowdImpostorVisible(CrowdImpostorManager* manager, Impostor* impostor)
{
    for (int filter = 0; filter < manager->mNumVisibilityFilters; ++filter)
    {
        if (!manager->mVisibilityFilters[filter]->IsVisible(impostor))
            return false;
    }
    return true;
}

namespace {

// MSC_CROWD_VISFAST: for every layout, the slots of its figures by sprite, in figure order. "all": every figure that has a sprite and is not skipped; "pos": those of them
// that the filter's position tests show whatever the camera is. A pass appends one or the other, which is what the plain loop's Release calls add, in the same order.
// (plain arrays: the game's own operator new and <vector> do not meet in this unit)
struct VisEntry
{
    ImpostorSprite* sprite;
    int* slots;
    int count, cap;
};
struct VisList
{
    VisEntry* entry;
    int count, cap;
};
struct VisLayout
{
    VisList all, pos;
};
struct VisCache
{
    bool valid;
    unsigned gen;
    int layouts, used;
    float key[3];
    VisLayout* layout;
};
VisCache sVis;

void VisFree(VisList& list)
{
    for (int i = 0; i < list.count; ++i) free(list.entry[i].slots);
    free(list.entry);
    list.entry = 0;
    list.count = list.cap = 0;
}

void VisFreeAll()
{
    for (int i = 0; i < sVis.layouts; ++i)
    {
        VisFree(sVis.layout[i].all);
        VisFree(sVis.layout[i].pos);
    }
    free(sVis.layout);
    sVis.layout = 0;
    sVis.layouts = 0;
    sVis.valid = false;
}

void VisAdd(VisList& list, ImpostorSprite* sprite, int slot)
{
    VisEntry* e = 0;
    for (int i = 0; i < list.count; ++i)
    {
        if (list.entry[i].sprite == sprite)
        {
            e = &list.entry[i];
            break;
        }
    }
    if (e == 0)
    {
        if (list.count == list.cap)
        {
            list.cap = list.cap ? list.cap * 2 : 16;
            list.entry = (VisEntry*)realloc(list.entry, (size_t)list.cap * sizeof(VisEntry));
        }
        e = &list.entry[list.count++];
        e->sprite = sprite;
        e->slots = 0;
        e->count = e->cap = 0;
    }
    if (e->count == e->cap)
    {
        e->cap = e->cap ? e->cap * 2 : 32;
        e->slots = (int*)realloc(e->slots, (size_t)e->cap * sizeof(int));
    }
    e->slots[e->count++] = slot;
}

// MSC_CROWD_THIN=n (nxvis25, VISIBLE, default 0 = off; 2, 3 or 4): in live play (TASK_GAMEPLAY, no Mega overlay) only every n-th seat of each list is drawn (the seat's number in its list,
// no random number): the stands look n times thinner, the figures' poses and the sprite passes are as before. Render only: nothing here is read by the simulation.
static int NpCrowdThinN()
{
    static const int n = [] { const char* e = getenv("MSC_CROWD_THIN"); int v = e && *e ? atoi(e) : 0; return v < 2 ? 0 : (v > 4 ? 4 : v); }();
    return n;
}
static int NpCrowdThinNow()
{
    const int n = NpCrowdThinN();
    if (n == 0 || nlTaskManager::m_pInstance == 0 || nlTaskManager::m_pInstance->mCurrentState != TASK_GAMEPLAY || gMegastrikeBackgroundOverlay.mActive) return 0;
    return n;
}

void VisAppend(const VisList& list)
{
    const int thin = NpCrowdThinNow();
    for (int i = 0; i < list.count; ++i)
    {
        ImpostorSprite* sprite = list.entry[i].sprite;
        int room = sprite->mCapacity - sprite->mNumRenderSlots;
        int n = list.entry[i].count;
        if (room <= 0) continue;
        if (thin != 0)
        {
            // the seat's own slot number decides (the same test as UpdateCrowdVisibilityPlain): the ratio is exact and a seat does not change when the camera pans
            int* dst = sprite->mRenderSlots + sprite->mNumRenderSlots;
            int kept = 0;
            for (int j = 0; j < n && kept < room; ++j)
                if (list.entry[i].slots[j] % thin == 0) dst[kept++] = list.entry[i].slots[j];
            sprite->mNumRenderSlots += kept;
            continue;
        }
        if (n > room) n = room;
        if (n > 0)
        {
            memcpy(sprite->mRenderSlots + sprite->mNumRenderSlots, list.entry[i].slots, (size_t)n * sizeof(int));
            sprite->mNumRenderSlots += n;
        }
    }
}

// all sprites' slot lists into a malloc'd array (count first, then the slots), for the comparison of MSC_CROWD_VISFAST=2
int* VisSnapshot(int* outCount)
{
    int total = 0;
    int cap = 4096;
    int* out = (int*)malloc((size_t)cap * sizeof(int));
    nlDLListIterator<ImpostorCharacter*> it;
    it = ImpostorManager::GetInstance()->mCharacters.Begin();
    while (it.hasNext())
    {
        nlDLListIterator<ImpostorSprite*> sprites;
        sprites = (*it)->mSprites.Begin();
        while (sprites.hasNext())
        {
            ImpostorSprite* sprite = *sprites;
            if (total + sprite->mNumRenderSlots + 1 > cap)
            {
                cap = (total + sprite->mNumRenderSlots + 1) * 2;
                out = (int*)realloc(out, (size_t)cap * sizeof(int));
            }
            out[total++] = sprite->mNumRenderSlots;
            for (int i = 0; i < sprite->mNumRenderSlots; ++i) out[total++] = sprite->mRenderSlots[i];
            sprites.Step();
        }
        it.Step();
    }
    *outCount = total;
    return out;
}

} // namespace

void CrowdImpostorManager::UpdateCrowdVisibilityPlain(GLView* view)
{
    Impostor* impostors = ImpostorManager::GetInstance()->mImpostors;
    ImpostorManager::GetInstance()->ResetSpriteSlots();
    sNumCrowdImpostorsInSelectedLayouts = 0;
    const int thin = NpCrowdThinNow();

    for (int layoutIndex = 0; layoutIndex < mNumLayouts;
        ++layoutIndex)
    {
        CrowdLayoutRecord& layout
            = mLayouts[layoutIndex];
        const nlVector4* visibilityView
            = view->m_Interface->GetShadowMatrix();
        if (!ClassifyBoxInFrustum(visibilityView, &layout.mBoundsMin,
                &layout.mBoundsMax, 0))
        {
            continue;
        }

        for (int i = 0; i < layout.mNumImpostors; ++i)
        {
            Impostor* impostor
                = &impostors[i + layout.mFirstImpostor];
            if (impostor->mSkipCrowdVisibilityPass)
                continue;

            if (IsCrowdImpostorVisible(this, impostor) && (thin == 0 || impostor->mSlot % thin == 0))   // (MSC_CROWD_THIN: as VisAppend)
                impostor->Release();
        }
        sNumCrowdImpostorsInSelectedLayouts += layout.mNumImpostors;
    }
}

void CrowdImpostorManager::UpdateCrowdVisibility(GLView* view)
{
    const int mode = NpCrowdVisFast();
    if (mode == 0 || mNumVisibilityFilters != 1 || !CrowdSidelineFilterIs(mVisibilityFilters[0]))
    {
        UpdateCrowdVisibilityPlain(view);
        return;
    }

    CrowdSidelineFilter* filter = mVisibilityFilters[0];
    Impostor* impostors = ImpostorManager::GetInstance()->mImpostors;
    float key[3];
    filter->PositionKey(key);
    const int used = ImpostorManager::GetInstance()->mNumUsed;
    if (!sVis.valid || sVis.gen != gNpCrowdStructGen || sVis.layouts != mNumLayouts || sVis.used != used || memcmp(sVis.key, key, sizeof(key)) != 0)
    {
        VisFreeAll();
        sVis.layout = (VisLayout*)calloc((size_t)(mNumLayouts > 0 ? mNumLayouts : 1), sizeof(VisLayout));
        sVis.layouts = mNumLayouts;
        for (int layoutIndex = 0; layoutIndex < mNumLayouts; ++layoutIndex)
        {
            const CrowdLayoutRecord& layout = mLayouts[layoutIndex];
            VisLayout& built = sVis.layout[layoutIndex];
            for (int i = 0; i < layout.mNumImpostors; ++i)
            {
                Impostor* impostor = &impostors[i + layout.mFirstImpostor];
                if (impostor->mSkipCrowdVisibilityPass || impostor->mpSprite == 0)
                    continue;
                VisAdd(built.all, impostor->mpSprite, impostor->mSlot);
                if (filter->PositionVisible(impostor))
                    VisAdd(built.pos, impostor->mpSprite, impostor->mSlot);
            }
        }
        sVis.gen = gNpCrowdStructGen;
        sVis.used = used;
        memcpy(sVis.key, key, sizeof(key));
        sVis.valid = true;
    }

    ImpostorManager::GetInstance()->ResetSpriteSlots();
    sNumCrowdImpostorsInSelectedLayouts = 0;
    const bool all = CrowdSidelineFilter::CameraShowsAll();
    const nlVector4* visibilityView = view->m_Interface->GetShadowMatrix();
    for (int layoutIndex = 0; layoutIndex < mNumLayouts; ++layoutIndex)
    {
        CrowdLayoutRecord& layout = mLayouts[layoutIndex];
        if (!ClassifyBoxInFrustum(visibilityView, &layout.mBoundsMin, &layout.mBoundsMax, 0))
        {
            continue;
        }
        const VisLayout& built = sVis.layout[layoutIndex];
        VisAppend(all ? built.all : built.pos);
        sNumCrowdImpostorsInSelectedLayouts += layout.mNumImpostors;
    }

    if (mode == 2)
    {
        int nFast = 0, nPlain = 0;
        int* fast = VisSnapshot(&nFast);
        UpdateCrowdVisibilityPlain(view);
        int* plain = VisSnapshot(&nPlain);
        NpCrowdCountVis(nFast != nPlain || memcmp(fast, plain, (size_t)nFast * sizeof(int)) != 0);
        free(fast);
        free(plain);
    }
}

inline bool CrowdImpostorManager::IsObjectEnabled(CrowdLayoutObject* object)
{
    nlDLListIterator<CrowdLayoutObject*> objectIt;
    objectIt = mEnabledObjects.Begin();
    while (objectIt.hasNext())
    {
        if (object == *objectIt)
            return true;
        objectIt.Step();
    }
    return false;
}

inline bool CrowdImpostorManager::IsPointOccluded(
    const nlVector4& worldPoint, nlVector4& localPoint)
{
    nlDLListIterator<CrowdLayoutObject*> occlusionIt;
    occlusionIt = mOcclusionObjects.Begin();
    while (occlusionIt.hasNext())
    {
        CrowdLayoutObject* object = *occlusionIt;
        nlMultVectorMatrix(localPoint, worldPoint, mInverseMatrices[0]);
        if (object->ContainsLocalPoint((nlVector3*)&localPoint))
            return true;
        occlusionIt.Step();
    }
    return false;
}

inline ImpostorCharacter* CrowdImpostorManager::GetCharacter(int index)
{
    nlDLListIterator<ImpostorCharacter*> characterIt;
    characterIt = mCharacters.Begin();
    while (index-- > 0)
        characterIt.Step();
    return characterIt.m_Curr->entry;
}

void CrowdImpostorManager::ReleaseCrowdImpostors()
{
    Impostor* impostors = ImpostorManager::GetInstance()->mImpostors;
    sNumCrowdImpostorsInSelectedLayouts = 0;

    for (int layoutIndex = 0; layoutIndex < mNumLayouts;
        ++layoutIndex)
    {
        CrowdLayoutRecord& layout
            = mLayouts[layoutIndex];
        if (!IsObjectEnabled(layout.mObject))
            continue;

        for (int i = 0; i < layout.mNumImpostors; ++i)
            impostors[i + layout.mFirstImpostor].Release();
        sNumCrowdImpostorsInSelectedLayouts += layout.mNumImpostors;
    }
}

inline u16 CrowdLayoutObject::GetFacingAngle(int numAngles)
{
    nlVector4 facing = { 0.0f, -1.0f, 0.0f, 0.0f };
    nlMultVectorMatrix(facing, *GetWorldMatrix());
    return QuantizeImpostorAngle(nlVector3ToAngle(*(nlVector3*)&facing),
        numAngles);
}

inline void CrowdPointCallback::UpdateBounds(const nlVector4& worldPoint)
{
    CrowdLayoutRecord* layout = mLayout;

    nlVector3 center = *(const nlVector3*)&worldPoint;
    nlVector3 boundsMin;
    nlVector3 boundsMax;
    boundsMin = center;
    boundsMin.x -= sfImpostorWidth.value;
    boundsMin.y -= sfImpostorWidth.value;
    boundsMax = center;
    boundsMax.x += sfImpostorWidth.value;
    boundsMax.y += sfImpostorWidth.value;
    boundsMax.z += sfImpostorHeight.value;

    if (mFirst)
    {
        layout->mBoundsMin = boundsMin;
        layout->mBoundsMax = boundsMax;
        mFirst = false;
        return;
    }

    if (boundsMin.x < layout->mBoundsMin.x)
        layout->mBoundsMin.x = boundsMin.x;
    if (boundsMin.y < layout->mBoundsMin.y)
        layout->mBoundsMin.y = boundsMin.y;
    if (boundsMin.z < layout->mBoundsMin.z)
        layout->mBoundsMin.z = boundsMin.z;
    if (boundsMax.x > layout->mBoundsMax.x)
        layout->mBoundsMax.x = boundsMax.x;
    if (boundsMax.y > layout->mBoundsMax.y)
        layout->mBoundsMax.y = boundsMax.y;
    if (boundsMax.z > layout->mBoundsMax.z)
        layout->mBoundsMax.z = boundsMax.z;
}

void CrowdPointCallback::Place(
    nlVector4 point)
{
    float horizontalJitter;
    float verticalJitter = (
        horizontalJitter = sfDistanceBetweenCrowdMembers.value
            * sfHorizontalJitterFraction.value,
        sfDistanceBetweenCrowdRows.value * sfVerticalJitterFraction.value);

    nlVector4 localPoint;
    localPoint.x = point.x
        + nlRandomf(-horizontalJitter, horizontalJitter, &nlDefaultSeed);
    localPoint.y = point.y
        + nlRandomf(-verticalJitter, verticalJitter, &nlDefaultSeed);
    localPoint.z = point.z;
    localPoint.w = 1.0f;

    nlVector4 worldPoint;
    nlMultVectorMatrix(
        worldPoint, localPoint, *mObject->GetWorldMatrix());

    nlVector4 occlusionPoint;
#ifdef NP_NATIVE
    if (NpOccCensusOn())
    {
        // (counters only: the retail test below is made as before)
        bool fixedOcc = false;
        nlVector4 lp;
        nlDLListIterator<CrowdLayoutObject*> vIt;
        vIt = GetCrowdImpostorManager()->mOcclusionObjects.Begin();
        for (int v = 0; vIt.hasNext() && v < sOccAllN; ++v)
        {
            nlMultVectorMatrix(lp, worldPoint, sOccAll[v]);
            if ((*vIt)->ContainsLocalPoint((nlVector3*)&lp))
            {
                fixedOcc = true;
                break;
            }
            vIt.Step();
        }
        nlVector4 tmp;
        const bool buggyOcc = GetCrowdImpostorManager()->IsPointOccluded(worldPoint, tmp);
        ++sOccSeats;
        sOccBuggy += buggyOcc ? 1 : 0;
        sOccFixed += fixedOcc ? 1 : 0;
        sOccBoth += (buggyOcc && fixedOcc) ? 1 : 0;
        sOccOnlyBuggy += (buggyOcc && !fixedOcc) ? 1 : 0;
        sOccOnlyFixed += (!buggyOcc && fixedOcc) ? 1 : 0;
    }
#endif
    if (GetCrowdImpostorManager()->IsPointOccluded(worldPoint, occlusionPoint))
        return;

    CrowdImpostorManager* manager = GetCrowdImpostorManager();
    int numCharacters = nlDLRingCountElements(manager->mCharacters.m_Head);
    int characterIndex = nlRandom(numCharacters, &nlDefaultSeed);
    ImpostorCharacter* character = manager->GetCharacter(characterIndex);

    if (GetCrowdImpostorManager()->mNumAngles == 0)
        GetCrowdImpostorManager()->mNumAngles = character->mNumAngles;

    int numAngles = GetCrowdImpostorManager()->mNumAngles;
    u16 angle = mObject->GetFacingAngle(numAngles);

    int impostorIndex = -1;
    Impostor* impostor
        = ImpostorManager::GetInstance()->AllocImpostor(&impostorIndex);
    if (impostor == 0)
        return;

    impostor->Set(character, *(nlVector3*)&worldPoint,
        sfImpostorWidth, sfImpostorHeight, angle);

    if (GetCrowdImpostorManager()->IsObjectEnabled(mObject))
        impostor->mSkipCrowdVisibilityPass = true;
    ++gNpCrowdStructGen;   // MSC_CROWD_VISFAST

    if (mFirst)
    {
        mLayout->mFirstImpostor = impostorIndex;
        mLayout->mNumImpostors = 0;
    }
    ++mLayout->mNumImpostors;
    UpdateBounds(worldPoint);
}
