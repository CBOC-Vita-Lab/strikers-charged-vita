#include "Game/Physics/CharacterPhysicsElement.h"

#include "NL/nlChunk.h"
#include "NL/nlMemory.h"
#include "port/chunk_endian.h"

bool LoadCharacterPhysicsElements(void* pFileData, unsigned long nFileSize,
    CharacterPhysicsData* pPhysicsData, bool arg3)
{
    nlChunk* outerChunk;
    nlChunk* endChunk;
    unsigned long i;

    if (pFileData == 0)
    {
        return false;
    }

    // NP: big-endian on disc. Chunk headers first, over the extent the root header declares (an empty
    // set's last chunk is stored without its alignment padding, so the file itself can end a few bytes
    // before it); a tree whose sizes do not fit is refused rather than walked out of bounds.
    if (port_disc_chunk_swap_tree(pFileData, port_disc32(&((nlChunk*)pFileData)->m_Size) + sizeof(nlChunk)) == 0)
    {
        if (arg3)
        {
            delete (u8*)pFileData;
        }
        return false;
    }

    endChunk = ((nlChunk*)pFileData)->GetLastChunk();
    outerChunk = ((nlChunk*)pFileData)->GetFirstChunk();

    while (outerChunk < endChunk)
    {
        switch ((s32)outerChunk->GetID())
        {
        case 0x0001D001:
        {
            port_disc32_array(outerChunk->GetData(), 1);
            pPhysicsData->physicsElementCount = *(u32*)outerChunk->GetData();
            pPhysicsData->pPhysicsElements = (CharacterPhysicsElement*)nlMalloc(
                pPhysicsData->physicsElementCount * sizeof(CharacterPhysicsElement), 8, false);
            break;
        }
        case 0x0001D002:
        {
            CharacterPhysicsElement* pPhysicsElements = (CharacterPhysicsElement*)outerChunk->GetData();
            // NP: no more elements than the chunk holds.
            unsigned long numAvailable = outerChunk->GetDataSize() / sizeof(CharacterPhysicsElement);
            for (i = 0; i < pPhysicsData->physicsElementCount && i < numAvailable; i++)
            {
                CharacterPhysicsElement& element = pPhysicsData->pPhysicsElements[i];
                element = pPhysicsElements[i];
                // NP: the matrix (16 floats), the name hash (the names are bytes), then the seven words
                // from the parent hash to the reserved word.
                port_disc32_array(&element.matLocalToParent, 16);
                port_disc32_array(&element.uHashID, 1);
                port_disc32_array(&element.uParentHashID, 7);
            }
            break;
        }
        }

        outerChunk = outerChunk->GetNextChunk();
    }

    if (arg3)
    {
        delete (u8*)pFileData;
    }
    return true;
}
