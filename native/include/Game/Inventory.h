#ifndef GAME_INVENTORY_H
#define GAME_INVENTORY_H

#include "NL/nlPrint.h"

#include "Game/SAnim.h"
#include "port/endian.h"
#include "NL/nlList.h"
#include "NL/nlString.h"
#include "NL/nlPrint.h"


template <typename T>
class cInventory
{
public:
    cInventory()
        : m_nItemCount(0)
    {
    }

    void ParseChunk(nlChunk* chunk)
    {
        if (T::IsValidChunkID(port_disc32(&chunk->m_ID) & 0x80FFFFFF))
        {
            T* item = T::Initialize(chunk);
            // NP: an item whose tree is not well formed is refused (null) rather than listed.
            if (item == 0)
            {
                nlPrintf("Warning: inventory refused a malformed chunk tree\n");
                return;
            }
            m_lItemList.AddStart(item);
            m_nItemCount++;
        }
        else
        {
            nlPrintf(
                "Warning: inventory encountered an unknown chunk type\n");
        }
    }

    void ParseChunks(nlChunk* chunk, nlChunk* end)
    {
        // NP: the file is big-endian and each item converts its own subtree (T::Initialize), so
        // the walk reads the top-level header through the swap instead of trusting it, and finds
        // the next one before the item has touched the header.
        while (chunk != end && chunk != 0)
        {
            const unsigned long uSize = port_disc32(&chunk->m_Size);
            nlChunk* next = (nlChunk*)((unsigned char*)chunk + sizeof(nlChunk) + ((uSize + 3) & ~3UL));
            ParseChunk(chunk);
            chunk = next;
        }
    }

    void AddFile(char* memory, unsigned long length)
    {
        m_lMemList.AddStart(memory);
        ParseChunks((nlChunk*)memory, (nlChunk*)(memory + length));
    }

    void AddFile(void* memory, unsigned long length)
    {
        AddFile((char*)memory, length);
    }

    nlListIterator<T*> Begin()
    {
        return m_lItemList.Begin();
    }

    T* Find(unsigned int hashID)
    {
        for (nlListIterator<T*> iterator = Begin(); iterator.IsValid();
            iterator.Next())
        {
            if (hashID == iterator.Current()->GetHashID())
            {
                return iterator.Current();
            }
        }
        return 0;
    }

    T* Find(char* name)
    {
        return Find(nlStringHash(name));
    }

    T* Find(int index)
    {
        int i = 0;
        for (nlListIterator<T*> iterator = Begin(); iterator.IsValid();
            iterator.Next())
        {
            if (i == index)
            {
                return iterator.Current();
            }
            ++i;
        }
        return 0;
    }

    ~cInventory();

    void Clear();

private:
    nlListContainer<T*> m_lItemList;
    nlListContainer<typename T::MemType> m_lMemList;
    int m_nItemCount;
};

template <typename T>
inline cInventory<T>::~cInventory()
{
    Clear();
}

template <typename T>
inline void cInventory<T>::Clear()
{
    for (nlListIterator<T*> iterator = m_lItemList.Begin();
        iterator.IsValid();
        iterator.Next())
    {
        iterator.Current()->Destroy();
    }

    m_lItemList.Clear();

    while (m_lMemList.m_Head != 0)
    {
        typename T::MemType mesh;
        m_lMemList.RemoveStart(&mesh);
        ::operator delete(mesh);
    }

    m_nItemCount = 0;
}

#endif // GAME_INVENTORY_H
