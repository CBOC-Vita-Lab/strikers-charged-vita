#ifndef NL_GL_GL_TEXTURE_MEMO_H
#define NL_GL_GL_TEXTURE_MEMO_H

// 1.3 MSC_GL_TEXMEMO: what glTextureManager::GetTextureIndex answers for a texture handle, kept for the frame send. Most bindings a send meets carry no resolved index (a
// program's local binding, or a parameter block refreshed from a packet that never was resolved), so each bind walks every pool's level trees: on the profile the largest
// render-side cost by far. The answer depends only on those trees, on the pool ring and on the indices given out; every writer of those calls glTextureLookupChanged, which empties the
// memo at its next use. glTextureMemoTable has one reader-writer: one table per thread role. Host test: renderer/tests/gl_texture_memo.cpp.
enum
{
    glTextureMemoSize = 1024
};

struct glTextureMemoEntry
{
    unsigned long handle;
    unsigned long index;
};

struct glTextureMemoTable
{
    unsigned long liveGen;   // the generation the entries belong to, plus one (a zeroed table belongs to none)
    glTextureMemoEntry entries[glTextureMemoSize];
};

// The handle of "no texture": never a stored key, and the empty mark of the table.
#define GL_TEXTURE_MEMO_EMPTY 0xFFFFFFFFul

template <class Slow>
static inline unsigned long glTextureMemoGet(glTextureMemoTable& table, unsigned long gen, unsigned long handle, Slow slow)
{
    if (handle == GL_TEXTURE_MEMO_EMPTY)
    {
        return slow(handle);
    }
    if (table.liveGen != gen + 1)
    {
        for (int i = 0; i < glTextureMemoSize; ++i)
        {
            table.entries[i].handle = GL_TEXTURE_MEMO_EMPTY;
        }
        table.liveGen = gen + 1;
    }
    glTextureMemoEntry& e = table.entries[(handle ^ (handle >> 10) ^ (handle >> 20)) & (glTextureMemoSize - 1)];
    if (e.handle == handle)
    {
        return e.index;
    }
    const unsigned long index = slow(handle);
    e.handle = handle;
    e.index = index;
    return index;
}

#endif
