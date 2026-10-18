#include "Game/Triggers/BinaryTriggerFile.h"
#include "port/endian.h"

BinaryTriggerFile::BinaryTriggerFile(
    void* pFileData, unsigned long FileSize)
{
    m_FileSize = FileSize;
    m_pFileData = (BinaryTriggerFileHeader*)pFileData;
    m_pCurrentAnim = 0;
    m_CurrentTrigger = 0;
    // NP: a missing file arrives as null, and the walk below would dereference it.
    if (m_pFileData == 0 || FileSize < sizeof(BinaryTriggerFileHeader))
    {
        m_pFileData = 0;
        m_pFirstAnim = 0;
        m_pFirstTrigger = 0;
        return;
    }

    // NP: the file is big-endian: version, anim count, bytecode offset and the reserved word are
    // four u16s behind the thumbprint, and the anim count sizes the table that follows.
    port_disc16_array(&m_pFileData->Version, 4);

    m_pFirstAnim
        = (ANIM_RECORD*)((u8*)m_pFileData + sizeof(BinaryTriggerFileHeader));
    for (u32 i = 0; i < m_pFileData->AnimCount; i++)
    {
        port_disc32_array(&m_pFirstAnim[i].hash, 1);
        port_disc16_array(&m_pFirstAnim[i].TriggerCount, 2);
    }

    m_pFirstTrigger = (TRIGGER_RECORD*)((u8*)m_pFirstAnim
        + (m_pFileData->AnimCount * sizeof(ANIM_RECORD)));
    {
        // The trigger records fill the gap between the anim table and the bytecode, and every field
        // of one (frame float, trigger id, script offset) is 32 bits.
        const u8* triggerEnd = (const u8*)m_pFileData + m_pFileData->BytecodeOffset;
        const u8* triggerStart = (const u8*)m_pFirstTrigger;
        if (triggerEnd > triggerStart)
        {
            port_disc32_array(m_pFirstTrigger, (unsigned long)(triggerEnd - triggerStart) / 4);
        }
    }
}
