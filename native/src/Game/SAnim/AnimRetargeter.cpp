#include "Game/SAnim/AnimRetargeter.h"
#include "port/chunk_endian.h"

AnimRetargetList* AnimRetargetList::Initialize(nlChunk* chunkData)
{
    // NP: big-endian on disc. Chunk headers first; a tree whose sizes do not fit is refused rather
    // than walked out of bounds.
    if (port_disc_chunk_swap_tree(chunkData, port_disc32(&chunkData->m_Size) + sizeof(nlChunk)) == 0)
    {
        return 0;
    }

    nlChunk* chunk = chunkData->GetFirstChunk();
    AnimRetargetList* data = (AnimRetargetList*)chunk->GetData();
    // NP: the list record's own words: hash (+4) and retarget count (+8); +0xC is a pointer filled in below.
    port_disc32_array(&data->m_uHashID, 1);
    port_disc32_array(&data->m_NumAnimRetargets, 1);

    nlChunk* nextChunk = chunk->GetNextChunk();
    nextChunk = nextChunk->GetFirstChunk();
    data->m_pAnimRetarget = (AnimRetarget*)nextChunk->GetData();

    for (int i = 0; i < data->m_NumAnimRetargets; i++)
    {
        // NP: three words (target signature, bone count, unknown) then the map pointer set below.
        port_disc32_array(&data->m_pAnimRetarget[i], 3);
        nextChunk = nextChunk->GetNextChunk();
        data->m_pAnimRetarget[i].m_pMap
            = (signed short*)nextChunk->GetData();
        // NP: the bone map is s16 entries.
        port_disc16_array(data->m_pAnimRetarget[i].m_pMap, nextChunk->GetDataSize() / 2);
    }

    return data;
}

AnimRetarget* AnimRetargetList::GetAnimRetargetWithSignature(
    const cSAnim* anim)
{
    return GetAnimRetargetWithSignature(anim->m_nHierarchySignature);
}

AnimRetarget* AnimRetargetList::GetAnimRetargetWithSignature(
    unsigned long signature)
{
    AnimRetarget* p;
    long offset;
    AnimRetarget* result = 0;
    offset = (long)result;

    for (long i = m_NumAnimRetargets; i > 0; i--)
    {
        p = (AnimRetarget*)((char*)m_pAnimRetarget + offset);
        if (signature == p->m_TargetHierarchySignature)
        {
            result = p;
            break;
        }
        offset += sizeof(AnimRetarget);
    }

    return result;
}
