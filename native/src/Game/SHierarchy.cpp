#include "Game/SHierarchy.h"
#include "port/chunk_endian.h"

bool cSHierarchy::PreserveBoneLength(int i) const
{
    return m_pPreserveBoneLength[i];
}

nlVector3& cSHierarchy::GetTranslationOffset(int i) const
{
    return m_pV3TranslationOffset[i];
}

int cSHierarchy::GetParent(int i) const
{
    return m_pParent[i];
}

int cSHierarchy::GetPushPop(int i) const
{
    return m_pPushPop[i];
}

int cSHierarchy::GetMirroredNode(int i) const
{
    return m_pMirrorTable[i];
}

int cSHierarchy::GetNumChildren(int i) const
{
    return m_pNumChildren[i];
}

u32 cSHierarchy::GetNodeID(int i) const
{
    return m_pNodeID[i];
}

int cSHierarchy::GetNodeIndexByID(unsigned int id) const
{
    for (int i = 0; i < m_nNumNodes; i++)
    {
        if (id == m_pNodeID[i])
        {
            return i;
        }
    }
    return -1;
}

int cSHierarchy::GetChild(int i, int j) const
{
    return m_pChildren[i][j];
}

void cSHierarchy::BuildPushPopFlags(
    int nNode, int nParentDepth, int& nCurrentDepth)
{
    int nNumChildren;
    int i;

    if (nParentDepth != nCurrentDepth)
    {
        m_pPushPop[nNode - 1] = nParentDepth - nCurrentDepth;
        nCurrentDepth = nParentDepth;
    }

    nNumChildren = m_pNumChildren[nNode];
    if (nNumChildren != 0)
    {
        m_pPushPop[nNode] = 1;
        nCurrentDepth = nCurrentDepth + 1;
        nParentDepth = nCurrentDepth;

        for (i = 0; i < nNumChildren; i++)
        {
            BuildPushPopFlags(GetChild(nNode, i), nParentDepth, nCurrentDepth);
        }
    }
    else
    {
        m_pPushPop[nNode] = 0;
    }
}

cSHierarchy* cSHierarchy::Initialize(nlChunk* pChunk)
{
    // NP: big-endian on disc. Chunk headers first, then each payload as it is reached. A tree whose
    // sizes do not fit is refused rather than walked out of bounds.
    if (port_disc_chunk_swap_tree(pChunk, port_disc32(&pChunk->m_Size) + sizeof(nlChunk)) == 0)
    {
        return 0;
    }

    nlChunk* pFirst = (nlChunk*)pChunk->GetUnalignedData();
    if (pChunk->IsAlignedChunk())
    {
        pFirst = (nlChunk*)pChunk->GetAlignedData();
    }

    pChunk = pFirst;
    cSHierarchy* pRetval = (cSHierarchy*)pChunk->GetData();
    // NP: the record's own words from the file: hash (+4), node count (+8), pelvis and spine nodes
    // (+0x24, +0x28); the rest are pointers this function fills in.
    port_disc32_array(&pRetval->m_uHashID, 2);
    port_disc32_array(&pRetval->m_nPelvisNodeIndex, 2);

    pChunk = pChunk->GetNextChunk();
    pRetval->m_szName = (const char*)pChunk->GetData();

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pNodeID = (u32*)pChunk->GetData();
    port_disc32_array(pRetval->m_pNodeID, pChunk->GetDataSize() / 4);

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pParent = (int*)pChunk->GetData();
    port_disc32_array(pRetval->m_pParent, pChunk->GetDataSize() / 4);

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pNumChildren = (int*)pChunk->GetData();
    port_disc32_array(pRetval->m_pNumChildren, pChunk->GetDataSize() / 4);

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pChildren = (int**)pChunk->GetData();

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pPushPop = (int*)pChunk->GetData();

    pChunk = pChunk->GetNextChunk();
    int* pChild = (int*)pChunk->GetData();
    port_disc32_array(pChild, pChunk->GetDataSize() / 4);

    for (int i = 0; i < pRetval->m_nNumNodes; i++)
    {
        if (pRetval->m_pNumChildren[i] > 0)
        {
            pRetval->m_pChildren[i] = pChild;
        }
        else
        {
            pRetval->m_pChildren[i] = 0;
        }
        pChild += pRetval->m_pNumChildren[i];
    }

    int nCurrentDepth = 0;
    pRetval->BuildPushPopFlags(0, 0, nCurrentDepth);

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pMirrorTable = (int*)pChunk->GetData();
    port_disc32_array(pRetval->m_pMirrorTable, pChunk->GetDataSize() / 4);

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pV3TranslationOffset = (nlVector3*)pChunk->GetData();
    port_disc32_array(pRetval->m_pV3TranslationOffset, pChunk->GetDataSize() / 4);

    pChunk = pChunk->GetNextChunk();
    pRetval->m_pPreserveBoneLength = (u8*)pChunk->GetData();

    return pRetval;
}
