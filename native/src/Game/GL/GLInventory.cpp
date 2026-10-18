#ifdef NP_NATIVE
#include "np/framesend.h"
#endif
#include "Game/GL/GLInventory.h"

#include "Game/GL/GLTextureAnim.h"
#include "Game/GL/GLVertexAnim.h"
#include "Game/SAnim.h"
#include "Game/SHierarchy.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glResourceLoader.h"
#include "NL/gl/glTexture.h"
#include "NL/glx/glxLoadModel.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlDLRing.h"
#include "NL/nlMemory.h"

#include <string.h>

#ifdef NP_NATIVE
// NP (frame-send job): the send's texture lookups read m_nLevel while the game thread's per-frame ResourceMark / ResourceRelease (net mesh, impostors: levels without
// textures) change it. Either value finds the same textures (the levels in between are empty); the accesses are relaxed atomics so that the overlap is defined.
#define NP_LEVEL_READ(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)
#define NP_LEVEL_WRITE(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELAXED)
#else
#define NP_LEVEL_READ(v) (v)
#define NP_LEVEL_WRITE(v, x) ((v) = (x))
#endif

GLResourceChunkLoader::GLResourceChunkLoader(
    GLResourcePool* pResource, int textureBundleMode)
{
    m_pResource = pResource;
    m_pModels = 0;
    mTextureBundleMode = textureBundleMode;
}

bool GLResourceChunkLoader::LoadChunk(nlChunk* pChunk)
{
    switch (pChunk->GetID())
    {
    case 0x00024100:
        glEndLoadTextureBundle(pChunk->GetData(),
            pChunk->GetSize()
                - ((u8*)pChunk->GetAlignedData()
                    - (u8*)pChunk->GetUnalignedData()),
            m_pResource,
            mTextureBundleMode);
        break;
    case 0x8001B000:
    case 0x8001B100:
        unsigned long numModels;
        m_pModels = (glModel*)glEndLoadModel(pChunk,
            pChunk->GetSize()
                - ((u8*)pChunk->GetAlignedData()
                    - (u8*)pChunk->GetUnalignedData()),
            &numModels,
            m_pResource);
        break;
    default:
        return false;
    }
    return true;
}

PlatTexture* glx_GetTex(unsigned long handle)
{
    GLResourcePool* pResource = nlDLRingGetStart(glGetResourcePools());
    for (;; pResource = pResource->m_next)
    {
        PlatTexture* pTex = pResource->m_inventory->GetTexture(handle);
        if (pTex != 0)
            return pTex;
        if (nlDLRingIsEnd(glGetResourcePools(), pResource))
            break;
    }
    return 0;
}

GLTextureAnim* glGetTextureAnim(unsigned long texture)
{
    GLResourcePool* pResource = nlDLRingGetStart(glGetResourcePools());
    for (;; pResource = pResource->m_next)
    {
        GLTextureAnim* anim = pResource->m_inventory->GetTextureAnim(texture);
        if (anim != 0)
            return anim;
        if (nlDLRingIsEnd(glGetResourcePools(), pResource))
            break;
    }
    return 0;
}

GLInventory::GLInventory()
{
    m_bCreated = false;
    for (int i = 0; i < 16; i++)
    {
        m_pFileData[i] = 0;
        m_pSkinData[i] = 0;
        m_pTextureAnims[i] = 0;
        m_pVertexAnims[i] = 0;
    }
    m_nLevel = 0;
    memset(&mModelReleaseCallback, 0, sizeof(mModelReleaseCallback));
}

static inline void DeleteFileEntries(ListEntry<void*>* current)
{
    while (current != 0)
    {
        operator delete(current->entry);
        current = current->next;
    }
}

GLInventory::~GLInventory()
{
    Delete();
}

void GLInventory::Create()
{
    m_bCreated = true;

    for (int i = 0; i < 16; i++)
    {
        m_pFileData[i] = new (8, false) nlListContainer<void*>();
        m_pSkinData[i] = new (8, false) freeing_GLInventoryContainer<nlChunk>();
        m_pModels[i] = new (8, false) clearing_GLInventory<glModel>();
        m_pTextureAnims[i] = new (8, false) clearing_GLInventory<GLTextureAnim>();
        m_pVertexAnims[i] =
            new (8, false) deleting_GLInventoryContainer<GLVertexAnim>();
        m_pTextures[i] = new (8, false) clearing_GLInventory<PlatTexture>();
    }
}

void GLInventory::Delete()
{
    m_bCreated = false;
    for (int i = 0; i < 16; i++)
    {
        ReleaseLevel(i);
        delete m_pFileData[i];
        delete m_pSkinData[i];
        delete m_pTextureAnims[i];
        delete m_pVertexAnims[i];
        delete m_pModels[i];
        delete m_pTextures[i];
    }
}

void GLInventory::ReleaseLevel(int nLevel)
{
    DeleteFileEntries(m_pFileData[nLevel]->m_Head);
    m_pFileData[nLevel]->Clear();
    m_pSkinData[nLevel]->Release();
    m_pModels[nLevel]->Release(mModelReleaseCallback);
    m_pTextureAnims[nLevel]->Release(glReleaseTextureAnim);
    m_pVertexAnims[nLevel]->Release();
    m_pTextures[nLevel]->Release(glReleaseTexture);
}

void GLInventory::SetModelReleaseCallback(const ModelReleaseCallback& callback)
{
    mModelReleaseCallback = callback;
}

void GLInventory::ResourceMark()
{
    NP_LEVEL_WRITE(m_nLevel, m_nLevel + 1);   // (the send looks textures up from m_nLevel down: a new level is empty, nothing to wait for)
}

void GLInventory::ResourceRelease(int nLevel)
{
#ifdef NP_NATIVE
    // NP: the send looks textures and texture animations up in these trees, and the texture memory goes back to the pool: a pending send finishes
    // before a level that holds one is released (levels without textures, e.g. the net mesh's per-frame mark / release, cost nothing)
    for (int level = m_nLevel; level > nLevel; --level)
    {
        if (m_pTextures[level]->m_pItems->m_Root != 0 || m_pTextureAnims[level]->m_pItems->m_Root != 0)
        {
            NpFsGate("invrelease");
            break;
        }
    }
#endif
    while (m_nLevel != nLevel)
    {
        ReleaseLevel(m_nLevel);
        NP_LEVEL_WRITE(m_nLevel, m_nLevel - 1);
    }
}

void GLInventory::AddModel(
    unsigned long key, glModel* model)
{
    unsigned long k = key;
    glModel* value = model;
    clearing_GLInventory<glModel>::Tree* pTree = m_pModels[m_nLevel]->m_pItems;
    pTree->Add(k, value);
}

glModel* GLInventory::GetModel(unsigned long id)
{
    for (int i = m_nLevel; i >= 0; i--)
    {
        unsigned long key = id;
        glModel** pResult;
        bool found = m_pModels[i]->m_pItems->FindGet(key, &pResult);
        glModel* result;
        if (found)
            result = *pResult;
        else
            result = 0;
        if (result != 0)
            return result;
    }
    return 0;
}

void GLInventory::AddTexture(unsigned long key, PlatTexture* texture)
{
#ifdef NP_NATIVE
    NpFsGate("texadd");   // NP: the send reads the texture trees
#endif
    unsigned long k = key;
    PlatTexture* value = texture;
    clearing_GLInventory<PlatTexture>::Tree* pTree = m_pTextures[m_nLevel]->m_pItems;
    pTree->Add(k, value);
}

PlatTexture* GLInventory::GetTexture(unsigned long id)
{
    for (int i = NP_LEVEL_READ(m_nLevel); i >= 0; i--)
    {
        unsigned long key = id;
        PlatTexture** pResult;
        bool found = m_pTextures[i]->m_pItems->FindGet(key, &pResult);
        PlatTexture* result;
        if (found)
            result = *pResult;
        else
            result = 0;
        if (result != 0)
            return result;
    }
    return 0;
}

void GLInventory::AddTextureAnim(unsigned long key, GLTextureAnim* anim)
{
#ifdef NP_NATIVE
    NpFsGate("texanimadd");
#endif
    unsigned long k = key;
    GLTextureAnim* value = anim;
    clearing_GLInventory<GLTextureAnim>::Tree* pTree = m_pTextureAnims[m_nLevel]->m_pItems;
    pTree->Add(k, value);
}

GLTextureAnim* GLInventory::GetTextureAnim(unsigned long id)
{
    for (int i = NP_LEVEL_READ(m_nLevel); i >= 0; i--)
    {
        unsigned long key = id;
        GLTextureAnim** foundValue;
        bool found =
            m_pTextureAnims[i]->m_pItems->FindGet(key, &foundValue);
        GLTextureAnim* result;
        if (found)
            result = *foundValue;
        else
            result = 0;
        if (result != 0)
            return result;
    }
    return 0;
}

class UpdateTextureAnimCallback
{
public:
    void Update(const unsigned long&, GLTextureAnim** anim);
    float mDeltaTime;
};

void UpdateTextureAnimCallback::Update(
    const unsigned long&, GLTextureAnim** anim)
{
    (*anim)->Update(mDeltaTime);
}

void GLInventory::AddVertexAnim(unsigned long key, GLVertexAnim* vertexAnim)
{
    unsigned long k = key;
    GLVertexAnim* value = vertexAnim;
    deleting_GLInventoryContainer<GLVertexAnim>::Tree* pTree =
        m_pVertexAnims[m_nLevel]->m_pItems;
    pTree->Add(k, value);
}

GLVertexAnim* GLInventory::GetVertexAnim(unsigned long id)
{
    for (int i = m_nLevel; i >= 0; i--)
    {
        unsigned long key = id;
        GLVertexAnim** pResult;
        bool found = m_pVertexAnims[i]->m_pItems->FindGet(key, &pResult);
        GLVertexAnim* result;
        if (found)
            result = *pResult;
        else
            result = 0;
        if (result != 0)
            return result;
    }
    return 0;
}

class UpdateVertexAnimCallback
{
public:
    void Update(const unsigned long&, GLVertexAnim** vertexAnim);
    float mDeltaTime;
};

void UpdateVertexAnimCallback::Update(
    const unsigned long&, GLVertexAnim** vertexAnim)
{
    (*vertexAnim)->Update(mDeltaTime);
}

void GLInventory::AddSkinData(unsigned long key, nlChunk* skinData)
{
    unsigned long key2 = key;
    nlChunk* skinData2 = skinData;
    freeing_GLInventoryContainer<nlChunk>* pSkinData =
        m_pSkinData[m_nLevel];
    freeing_GLInventoryContainer<nlChunk>::Tree* tree = pSkinData->m_pItems;
    tree->Add(key2, skinData2);
}

GLSkinMesh* GLInventory::MakeSkinMesh(
    unsigned long hashID, cSHierarchy* hierarchy)
{
    struct SkinDataHelper
    {
        static inline nlChunk* Get(GLInventory* self, unsigned long id)
        {
            nlChunk* result;
            int i;
            for (i = self->m_nLevel; i >= 0; i--)
            {
                unsigned long key = id;
                nlChunk** pResult;
                bool found =
                    self->m_pSkinData[i]->m_pItems->FindGet(key, &pResult);
                if (found)
                    result = *pResult;
                else
                    result = 0;
                if (result != 0)
                    return result;
            }
            return 0;
        }
    };

    nlChunk* foundChunk = SkinDataHelper::Get(this, hashID);
    nlChunk* pChunk = foundChunk;
    glModel* pModel = GetModel(hashID);

    return glx_MakeSkinMesh(pChunk, pModel, hierarchy);
}

void GLInventory::UpdateTextureAnims(float dt)
{
    UpdateTextureAnimCallback textureCallback;
    textureCallback.mDeltaTime = dt;
    for (int i = m_nLevel; i >= 0; i--)
    {
        m_pTextureAnims[i]->Walk(
            &textureCallback, &UpdateTextureAnimCallback::Update);
    }
}

void GLInventory::UpdateVertexAnims(float dt)
{
    UpdateVertexAnimCallback vertexCallback;
    vertexCallback.mDeltaTime = dt;
    for (int i = m_nLevel; i >= 0; i--)
    {
        m_pVertexAnims[m_nLevel]->Walk(
            &vertexCallback, &UpdateVertexAnimCallback::Update);
    }
}

void GLInventory::Update(float dt)
{
#ifdef NP_NATIVE
    // NP (frame-send worker): texture and vertex animations advance here; the send of the last frame still reads their current frames
    NpFsGateNotPipe("invupd");   // (the pipeline keeps the one table the send reads, glTextureManager::mTextures, and stores into it after the join)
#endif
    UpdateTextureAnims(dt);
    UpdateVertexAnims(dt);
}
