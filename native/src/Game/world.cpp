#include "NL/nlDLListContainer.inl"
#include "Game/World/WorldVisibility.h"
#include "NL/gl/gl.h"
#include "Game/World.h"
#include "Game/World/WorldEffect.h"
#include "np/scenecut.h"
#ifdef NP_NATIVE
#include "np/cpu22.h"
#endif
#include "np/stadiumcut.h"
#include "Game/Render/StadiumWorldObjects.h"
#include <math.h>

#include "Game/Drawable/DrawableObj.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/GL/GLInventory.h"
#include "Game/SAnim.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glTexture.h"
#include "NL/gl/glView.h"
#include "NL/nlPrint.h"
#include "Game/SharedStaticStorage.h"
#include "Game/Render/CrowdImpostorManager.h"
#include "Game/Render/WorldNPC.h"
#include "Game/World/WorldAnimObjects.h"
#include "NL/nlMemory.h"

class WorldUpdateObject
{
public:
    virtual ~WorldUpdateObject();
    virtual void ReleaseResources();
    virtual void GetWorldMatrix();
    virtual void SetWorldMatrix();
    virtual void OnWorldLoaded();
    virtual void Update(float fDeltaT);
};

u8* WorldObjectLoadContext::GetParentData()
{
    if (m_pParent == 0)
    {
        return 0;
    }

    return m_pParent + 0x20;
}

World::World(GLResourcePool* pResource)
    : m_pResource(pResource)
    , m_pOwnedData(0)
    , m_pOpaqueView(0)
    , m_pAlphaView(0)
{
#ifdef NP_NATIVE
    NpCpu22DrawEpochBump();
#endif
    m_pVisibilityTree = 0;
    m_bRenderingEnabled = true;
}

World::~World()
{
#ifdef NP_NATIVE
    NpCpu22DrawEpochBump();
#endif
    typedef nlAVLTreeIterator<unsigned long, DrawableObject*,
        DefaultKeyCompare<unsigned long> > DrawableIterator;

    m_renderObjects.Clear();
    m_physicsObjects.Clear();
    m_updateObjects.Clear();

    DrawableIterator* pIterator = m_drawableMap.GetIterator();
    while (pIterator->IsValid())
    {
        pIterator->Current()->value->ReleaseResources();
        if ((pIterator->Current()->value->m_uObjectCreationFlags & 1) == 0)
        {
            delete pIterator->Current()->value;
        }
        pIterator->Next();
    }
    if (pIterator != 0)
    {
        delete pIterator;
    }

    mWorldAnimManager.fn_80342324();
    OnBeforeUnload();
    mWorldAnimManager.Clear();
    if (m_pOwnedData != 0)
    {
        delete m_pOwnedData;
    }
}

void World::AddDrawableObject(DrawableObject* pDrawableObject)
{
#ifdef NP_NATIVE
    NpCpu22DrawEpochBump();   // (MSC_CPU22_POWERUP)
#endif
    m_drawableMap.Add(pDrawableObject->GetHashID(), pDrawableObject);

    if (pDrawableObject->m_uObjectCreationFlags & 2)
    {
        m_renderObjects.AddEnd(
            (WorldDrawable*)pDrawableObject);
    }

    if (pDrawableObject->m_uObjectCreationFlags & 4)
    {
        m_physicsObjects.AddEnd(
            (WorldPhysicsDrawable*)pDrawableObject);
        if (pDrawableObject->m_uObjectCreationFlags & 8)
        {
            m_updateObjects.AddEnd(
                (WorldUpdateObject*)pDrawableObject);
        }
    }
}

void World::RemoveDrawableObject(DrawableObject* pObject)
{
#ifdef NP_NATIVE
    NpCpu22DrawEpochBump();
#endif
    m_drawableMap.Remove(pObject->GetHashID());
    unsigned long uFlags = pObject->m_uObjectCreationFlags;

    if (uFlags & 2)
    {
        nlDLListIterator<WorldDrawable*> iterator;
        iterator = m_renderObjects.Begin();
        while (iterator.hasNext())
        {
            if (*iterator == (WorldDrawable*)pObject)
            {
                m_renderObjects.Remove(&iterator);
                return;
            }
            iterator.next();
        }
    }

    if (uFlags & 4)
    {
        nlDLListIterator<WorldPhysicsDrawable*> iterator;
        iterator = m_physicsObjects.Begin();
        while (iterator.hasNext())
        {
            if (*iterator == (WorldPhysicsDrawable*)pObject)
            {
                m_physicsObjects.Remove(&iterator);
                break;
            }
            iterator.next();
        }

        if (pObject->m_uObjectCreationFlags & 4)
        {
            nlDLListIterator<WorldUpdateObject*> updateIterator;
            updateIterator = m_updateObjects.Begin();
            while (updateIterator.hasNext())
            {
                if (*updateIterator
                    == (WorldUpdateObject*)pObject)
                {
                    m_updateObjects.Remove(&updateIterator);
                    return;
                }
                updateIterator.next();
            }
        }
    }
}

bool World::LoadData(void* pData0, unsigned long uSize0, void* pData1,
    unsigned long uSize1, bool bKeepData)
{
    m_pAnimationSet = 0;
    if (pData0 != 0)
    {
        LoadChunks((nlChunk*)pData0, uSize0);
    }
    LoadChunks((nlChunk*)pData1, uSize1);
    if (bKeepData)
    {
        m_pOwnedData = (u8*)pData1;
    }
    else
    {
        m_pOwnedData = 0;
    }
    return true;
}

bool World::LoadChunks(nlChunk* pChunk, unsigned long uSize)
{
    nlChunk* pCurrent = pChunk->GetFirstChunk();
    nlChunk* pEnd = pChunk->GetLastChunk();

    while (pCurrent != pEnd)
    {
        switch (pCurrent->GetID())
        {
        case 0x00026000:
            LoadObjects(pCurrent->GetData(), pCurrent->GetDataSize(), false);
            break;
        case 0x80018000:
            m_pAnimationSet
                = mWorldAnimManager.LoadHierarchy(pCurrent);
            break;
        case 0x80017000:
            mWorldAnimManager.LoadAnimationSet(
                m_pAnimationSet, pCurrent);
            break;
        case 0x80026100:
            m_pVisibilityTree = LoadWorldVisibilityTree(pCurrent);
            break;
        case 0x00024100:
            glEndLoadTextureBundle(pCurrent->GetData(),
                pCurrent->GetDataSize(),
                m_pResource, false);
            break;
        case 0x8001B000:
        case 0x8001B100:
            glEndLoadModel(pCurrent, 0, 0, m_pResource);
            break;
        default:
            HandleUnknownChunk(pCurrent);
            break;
        }
        pCurrent = pCurrent->GetNextChunk();
    }
    return true;
}

bool World::LoadObjects(
    void* pData, unsigned long uSize, bool bKeepData)
{
    unsigned long i = 0;

    if (bKeepData)
    {
        m_pOwnedData = (u8*)pData;
    }
    else
    {
        m_pOwnedData = 0;
    }

    WorldObjectLoadContext context;
    context.m_pObject = (u8*)pData + 0x10;
    context.m_pWorld = this;
    context.m_uNumObjectsLoaded = 0;
    context.m_pParent = 0;

    for (; i < *(unsigned long*)pData; ++i)
    {
        unsigned long uType = *(unsigned long*)(context.m_pObject + 8);
        if (uType == 0x10)
        {
            u8* pParent = context.m_pObject;
            context.m_pObject += *(unsigned long*)(context.m_pObject + 0xC);
            context.m_pParent = pParent;
            continue;
        }

        DrawableObject* pObject;
        if (uType < 0x10000)
        {
            pObject = CreateObject(uType, &context);
        }
        else
        {
            pObject = HandleObjectCreation(uType, &context);
        }
#ifdef NP_NATIVE
        NpStadiumCensusObject(uType);   // MSC_STADIUM_CENSUS (np/scenecut.h)
#endif

        pObject->m_pWorldContext = this;
        if (pObject != 0)
        {
            AddDrawableObject(pObject);
        }
        context.m_pParent = 0;
    }

    mWorldAnimManager.BindObjects();
    return true;
}

void World::HandleUnknownChunk(nlChunk* pChunk)
{
    nlPrintf("Unknown Chunk = 0x%08x\n", pChunk->GetID());
}

DrawableObject* World::CreateObject(
    unsigned long uType, WorldObjectLoadContext* pContext)
{
    DrawableObject* pObject = 0;

    switch (uType)
    {
    case 0xFFFFFFFF:
        break;
    case WORLD_OBJECT_DRAWABLE:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) WorldDrawable;
        ((WorldDrawable*)pObject)->Initialize(pContext);
        pContext->m_pObject += 0x70;
        ++pContext->m_uNumObjectsLoaded;
        break;
    case WORLD_OBJECT_VISIBILITY:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) WorldVisibilityDrawable;
        InitializeWorldVisibilityDrawable(
            (WorldVisibilityDrawable*)pObject, pContext);
        pContext->m_pObject += 0x30;
        ++pContext->m_uNumObjectsLoaded;
        break;
    case WORLD_OBJECT_PHYSICS:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) WorldPhysicsDrawable;
        InitializeWorldPhysicsDrawable((WorldPhysicsDrawable*)pObject, pContext);
        pContext->m_pObject += 0x90;
        ++pContext->m_uNumObjectsLoaded;
        break;
    case WORLD_OBJECT_HELPER:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) WorldHelperObject;
        ((WorldHelperObject*)pObject)->Initialize(pContext);
        pContext->m_pObject += 0x60;
        ++pContext->m_uNumObjectsLoaded;
        break;
    case WORLD_OBJECT_ANIMATION:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) WorldAnimObject;
        ((WorldAnimObject*)pObject)->Initialize(pContext);
        pContext->m_pObject += 0x90;
        ++pContext->m_uNumObjectsLoaded;
        break;
    case WORLD_OBJECT_CROWD_LAYOUT:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) CrowdLayoutObject;
        ((CrowdLayoutObject*)pObject)->RegisterWithCrowdManager(pContext);
        pContext->m_pObject += 0x80;
        ++pContext->m_uNumObjectsLoaded;
        break;
    case WORLD_OBJECT_NPC:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) WorldNPC;
        ((WorldNPC*)pObject)->Initialize(pContext);
        pContext->m_pObject += 0x70;
        ++pContext->m_uNumObjectsLoaded;
        break;
    case WORLD_OBJECT_EFFECT:
        pObject = (DrawableObject*)pContext->m_pObject;
        new (pObject) WorldEffect;
        ((WorldEffect*)pObject)->Initialize(pContext);
        pContext->m_pObject += 0xA0;
        ++pContext->m_uNumObjectsLoaded;
        break;
    default:
        break;
    }

    return pObject;
}

bool World::ResolveModel(glModel*& pModel) const
{
    unsigned long uHashID = (unsigned long)pModel;
    pModel = m_pResource->m_inventory->GetModel(uHashID);
    if (pModel == 0)
    {
        nlPrintf(
            "Warning: Failed to find GL model to match world object 0x%08x\n",
            uHashID);
        return false;
    }
    return true;
}

void World::InitializeObjects()
{
    typedef nlAVLTreeIterator<unsigned long, DrawableObject*,
        DefaultKeyCompare<unsigned long> > DrawableIterator;

    DrawableIterator* pIterator = m_drawableMap.GetIterator();
    while (pIterator->IsValid())
    {
        pIterator->Current()->value->OnWorldLoaded(this);
        pIterator->Next();
    }
    if (pIterator != 0)
    {
        delete pIterator;
    }
}

#ifdef NP_NATIVE
// MSC_STADIUM_CUT bit 8 (np/stadiumcut.h): is this plain or stadium drawable too small on the screen to draw? Only static scenery: no animation controller, no banner or blend flags
// (StadiumWorldDrawable m_uFlags), no model-less object. Projected diameter in pixels of a 960-wide picture, from the camera's distance and the projection's x scale.
static float NpStadiumCutObjectPx(const DrawableObject* obj, const nlVector3& cam, float xScale);
static bool NpStadiumCutFarSmall(const DrawableObject* obj, const nlVector3& cam, float xScale)
{
    const WorldDrawable* wd = (const WorldDrawable*)obj;
    if (wd->m_pAnimController != 0 || wd->m_pModel == 0) return false;
    const float px = NpStadiumCutObjectPx(obj, cam, xScale);
    return px >= 0.0f && NpStadiumCutFarSmall(px, NpStadiumCutFarPx());
}
// The projected diameter in pixels of a 960-wide picture, or -1 when it is not a static plain/stadium drawable or the camera is inside it.
static float NpStadiumCutObjectPx(const DrawableObject* obj, const nlVector3& cam, float xScale)
{
    const WorldDrawable* wd = (const WorldDrawable*)obj;
    float cx, cy, cz, radius;
    if (wd->m_uObjectType == WORLD_OBJECT_DRAWABLE)
    {
        const nlMatrix4& m = *const_cast<WorldDrawable*>(wd)->GetWorldMatrix();
        cx = m.e2[3][0]; cy = m.e2[3][1]; cz = m.e2[3][2];
        radius = wd->m_fBoundingRadius;
    }
    else if (wd->m_uObjectType == WORLD_OBJECT_STADIUM_DRAWABLE)
    {
        const StadiumWorldDrawable* sd = (const StadiumWorldDrawable*)wd;
        if ((sd->m_uFlags & 0x3F) != 0) return -1.0f;
        cx = 0.5f * (sd->m_boundsMin.x + sd->m_boundsMax.x);
        cy = 0.5f * (sd->m_boundsMin.y + sd->m_boundsMax.y);
        cz = 0.5f * (sd->m_boundsMin.z + sd->m_boundsMax.z);
        const float dx = sd->m_boundsMax.x - sd->m_boundsMin.x, dy = sd->m_boundsMax.y - sd->m_boundsMin.y, dz = sd->m_boundsMax.z - sd->m_boundsMin.z;
        radius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
    }
    else
    {
        return -1.0f;
    }
    if (!(radius > 0.0f)) return -1.0f;
    const float dx = cx - cam.x, dy = cy - cam.y, dz = cz - cam.z;
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist <= radius) return -1.0f;   // the camera is inside it
    return 2.0f * radius / dist * (xScale < 0.0f ? -xScale : xScale) * 480.0f;
}
#endif

void World::Render()
{
    if (m_pVisibilityTree != 0)
    {
        UpdateWorldVisibility(m_pVisibilityTree,
            m_pOpaqueView->m_Interface->GetShadowMatrix(), 0);
    }

    if (m_bRenderingEnabled)
    {
        nlDLListIterator<WorldDrawable*> iterator;
        iterator = m_renderObjects.Begin();
#ifdef NP_NATIVE
        const bool farCut = NpStadiumCutHas(NP_CUT_FARDECOR) || NpStadiumCutLogObjects();
        nlMatrix4 cutInv, cutProj;
        if (farCut)
        {
            m_pOpaqueView->m_Interface->GetInverseViewMatrix(cutInv);
            m_pOpaqueView->m_Interface->GetProjectionMatrix(cutProj);
        }
#endif
        const nlVector4* frustumPlanes = m_pOpaqueView->m_Interface->GetShadowMatrix();
        while (iterator.hasNext())
        {
            if (((DrawableObject*)*iterator)
                    ->IsVisibleInFrustum(frustumPlanes))
            {
#ifdef NP_NATIVE
                if (NpStadiumCutLogObjects())
                {
                    const WorldDrawable* wd = (const WorldDrawable*)*iterator;
                    const float px = NpStadiumCutObjectPx((DrawableObject*)*iterator, *(const nlVector3*)&cutInv.e2[3][0], cutProj.e2[0][0]);
                    NpStadiumCutLogObject(wd->m_uHashID, (int)wd->m_uObjectType, wd->m_pAnimController != 0, wd->m_uObjectType == WORLD_OBJECT_STADIUM_DRAWABLE ? (int)((const StadiumWorldDrawable*)wd)->m_uFlags : -1, px);
                }
                if (NpStadiumCutHas(NP_CUT_FARDECOR) && NpStadiumCutFarSmall((DrawableObject*)*iterator, *(const nlVector3*)&cutInv.e2[3][0], cutProj.e2[0][0]))
                {
                    NpStadiumCutCount(NP_CUT_FARDECOR, 1);
                    iterator.next();
                    continue;
                }
#endif
                ((DrawableObject*)*iterator)->Draw();
            }
            iterator.next();
        }
    }
}

void World::UpdateAnimations(float fDeltaT)
{
    mWorldAnimManager.Update(fDeltaT);
}

void World::Update(float fDeltaT, bool bUpdateState)
{
    if (bUpdateState)
    {
        UpdateAnimations(fDeltaT);
    }

    if (EmissionManager::Instance() != 0)
    {
        nlListIterator<WorldEffect*> iterator
            = m_worldEffects.Begin();
        while (iterator.IsValid())
        {
            iterator.Current()->Update(fDeltaT);
            iterator.Next();
        }
    }

    nlDLListIterator<WorldUpdateObject*> iterator;
    iterator = m_updateObjects.Begin();
    while (iterator.hasNext())
    {
        (*iterator)->Update(fDeltaT);
        iterator.next();
    }
}

DrawableObject* World::FindDrawableObject(unsigned long uHashID)
{
    DrawableObject** foundValue;
    if (!m_drawableMap.FindGet(uHashID, &foundValue))
    {
        return 0;
    }
    return *foundValue;
}

void World::AddEffect(WorldEffect* pEffect)
{
    m_worldEffects.AddStart(pEffect);
}

void World::UpdateEffects(float fDeltaT)
{
    if (EmissionManager::Instance() != 0)
    {
        nlListIterator<WorldEffect*> iterator
            = m_worldEffects.Begin();
        while (iterator.IsValid())
        {
            iterator.Current()->Update(fDeltaT);
            iterator.Next();
        }
    }
}

void World::ResetEffects()
{
    nlListIterator<WorldEffect*> iterator
        = m_worldEffects.Begin();
    while (iterator.IsValid())
    {
        WorldEffect* pEffect = iterator.Current();
        pEffect->m_fEmissionTime = 0.0f;
        pEffect->m_nRemainingEmissions = pEffect->m_nEmissionCount;
        pEffect->m_fPreviousEmissionTime = 0.0f;
        if (pEffect->m_nTimingMode == WORLD_EFFECT_ELAPSED_TIME)
        {
            float fEmissionTime = pEffect->m_fEmissionInterval;
            fEmissionTime = 1.0f + fEmissionTime;
            pEffect->m_fEmissionTime = fEmissionTime;
        }
        iterator.Next();
    }
}

void World::TriggerEffects(unsigned long uType)
{
    nlListIterator<WorldEffect*> iterator
        = m_worldEffects.Begin();
    while (iterator.IsValid())
    {
        WorldEffect* pEffect = iterator.Current();
        if (uType == pEffect->m_nTimingMode)
        {
            pEffect->m_nRemainingEmissions = pEffect->m_nEmissionCount;
            float fEmissionTime = pEffect->m_fEmissionInterval;
            fEmissionTime = 1.0f + fEmissionTime;
            pEffect->m_fEmissionTime = fEmissionTime;
        }
        iterator.Next();
    }
}
