#include "NL/gl/glTextureManager.h"

#include "Game/GL/GLTextureAnim.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glTextureMemo.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlMemory.h"
#include "np/framesend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSC_GL_TEXMEMO (glTextureMemo.h): 1 on, 0 the walk for every bind, 2 both and report a difference. The generation only moves when loading or unloading textures: the game
// thread's writers wait for a send first (NpFsGate), and a generation read before a walk tags what the walk found, so a change during a walk loses the entry, not the truth.
static unsigned long sTextureLookupGen = 0;
static glTextureMemoTable sTextureMemo[2];   // [0] the game thread, [1] the frame-send worker: each table has one thread

void glTextureLookupChanged()
{
    __atomic_add_fetch(&sTextureLookupGen, 1, __ATOMIC_RELEASE);
}

static inline int glTextureMemoMode()
{
    static int mode = -1;
    if (mode < 0)
    {
        const char* e = getenv("MSC_GL_TEXMEMO");
        mode = (e != 0 && e[0] != 0) ? atoi(e) : 1;
    }
    return mode;
}

static unsigned long glTextureMemoIndex(glTextureManager* manager, unsigned long handle, int mode)
{
    const unsigned long gen = __atomic_load_n(&sTextureLookupGen, __ATOMIC_ACQUIRE);
    glTextureMemoTable& table = sTextureMemo[(&NpFsOnWorker != 0 && NpFsOnWorker() != 0) ? 1 : 0];
    const unsigned long index = glTextureMemoGet(table, gen, handle, [manager](unsigned long h) { return manager->GetTextureIndex(h); });
    if (mode == 2 && index != manager->GetTextureIndex(handle) && gen == __atomic_load_n(&sTextureLookupGen, __ATOMIC_ACQUIRE))
    {
        static int reported = 0;
        if (reported++ < 8)
        {
            fprintf(stderr, "[gl] texture memo MISMATCH: handle %08lx memo %lu\n", handle, index);
        }
    }
    return index;
}

glTextureManager::glTextureManager(unsigned long count)
{
    mCapacity = count;
    unsigned long size = count * sizeof(PlatTexture*);
    mTextures = (PlatTexture**)nlMalloc(size, 8, false);
    memset(mTextures, 0, size);
    mIndexBuffer = new (8, false) u16[count];
    mFreeIndices = new (8, false) glTextureIndexQueue(mIndexBuffer, count);
    for (u32 i = 0; i < count; ++i)
    {
        mFreeIndices->AddEnd((u16)i);
    }
}

unsigned long glTextureManager::GetTextureIndex(unsigned long texture)
{
    GLTextureAnim* anim = glGetTextureAnim(texture);
    if (anim != 0)
    {
        return anim->m_textureIndex;
    }
    PlatTexture* platformTexture = glx_GetTex(texture);
    if (platformTexture == 0)
    {
        return 0xFFFF;
    }
    return platformTexture->GetTextureIndex();
}

void glTextureManager::ResolveTextureIndex(glTextureBinding* texture)
{
    if (texture->textureIndex == 0xFFFF)
    {
        texture->textureIndex = GetTextureIndex(texture->texture);
    }
}

PlatTexture* glTextureManager::GetTextureAtIndex(const unsigned long* texture)
{
    if (*texture == 0xFFFF)
    {
        return 0;
    }
    return mTextures[*texture];
}

PlatTexture* glTextureManager::GetTexture(glTextureBinding* texture)
{
    const int memo = glTextureMemoMode();
    if (memo != 0 && texture->textureIndex == 0xFFFF)
    {
        texture->textureIndex = glTextureMemoIndex(this, texture->texture, memo);
    }
    else
    {
        ResolveTextureIndex(texture);
    }
    unsigned long textureIndex = texture->textureIndex;
    return GetTextureAtIndex(&textureIndex);
}

void glTextureManager::RegisterTexture(PlatTexture* texture)
{
    texture->m_TextureIndex = mFreeIndices->RemoveStart();
    mTextures[texture->GetTextureIndex()] = texture;
    glTextureLookupChanged();
}

void glTextureManager::RegisterTextureAnim(GLTextureAnim* anim)
{
    unsigned long index = mFreeIndices->RemoveStart();
    anim->m_textureIndex = index;
    mTextures[index] = GetTextureAtIndex(&anim->GetTexture(-1).m_TexHandle);
    glTextureLookupChanged();
}

void glTextureManager::RefreshTextureAnim(GLTextureAnim* anim)
{
    u32 index = anim->m_textureIndex;
#ifdef NP_NATIVE
    // PIPE: the frame send of the last frame reads mTextures; the store waits for it (np/framesend.h)
    PlatTexture* current = GetTextureAtIndex(&anim->GetTexture(-1).m_TexHandle);
    NpFsStore((void**)&mTextures[index], (void*)current);
#else
    mTextures[index] = GetTextureAtIndex(&anim->GetTexture(-1).m_TexHandle);
#endif
}
