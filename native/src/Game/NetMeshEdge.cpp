#include "Game/Physics/NetMeshEdge.h"
#ifdef NP_NATIVE
#include "platform/assets/np_endian.h" /* GX vertex arrays stay big-endian (NPA) */
#endif

static inline bool HasStream(const glModelPacket* packet, int streamID)
{
    for (u8 i = 0; i < packet->numStreams; ++i)
    {
        if (packet->streams[i].id == streamID)
            return true;
    }
    return false;
}

static inline const glModelStream* GetStream(
    const glModelPacket* packet, int streamID)
{
    for (u8 i = 0; i < packet->numStreams; ++i)
    {
        if (packet->streams[i].id == streamID)
            return &packet->streams[i];
    }
    return packet->streams;
}

const nlVector3* NetMeshModelLoader::NetMeshVertex::GetPosition() const
{
    const glModelStream* layout = GetStream(mpPacket, 1);
    u8 stride = layout->stride;
    s8* base = (s8*)layout->address;
#ifdef NP_NATIVE
    /* the position array is a GX array (big-endian floats): decode into a small ring of scratch vectors,
       the callers copy the value out immediately */
    static nlVector3 sScratch[4];
    static unsigned sNext;
    nlVector3* out = &sScratch[sNext++ & 3];
    const u8* src = (const u8*)(base + mIndex * stride);
    out->x = npa_rdf32(src);
    out->y = npa_rdf32(src + 4);
    out->z = npa_rdf32(src + 8);
    return out;
#else
    return (const nlVector3*)(base + mIndex * stride);
#endif
}

void NetMeshModelLoader::NetMeshVertex::GetNormal(nlVector3& normal) const
{
    if (!HasStream(mpPacket, 2))
    {
        normal.x = 1.0f;
        normal.y = 0.0f;
        normal.z = 0.0f;
        return;
    }

    const glModelStream* layout = GetStream(mpPacket, 2);

    if (layout->stride == 3)
    {
        s8 normalX = ((const s8*)layout->address
                      + mIndex * layout->stride)[0];
        normal.x = (float)normalX;
        normal.x *= 0.015625f;
        s8 normalY = ((const s8*)layout->address
                      + mIndex * layout->stride)[1];
        normal.y = (float)normalY;
        normal.y *= 0.015625f;
        s8 normalZ = ((const s8*)layout->address
                      + mIndex * layout->stride)[2];
        normal.z = (float)normalZ;
        normal.z *= 0.015625f;
    }
    else if (layout->stride == 12)
    {
        float f = 1.0f;
#ifdef NP_NATIVE
        const u8* n = (const u8*)layout->address + mIndex * layout->stride;
        normal.x = npa_rdf32(n) / f;
        normal.y = npa_rdf32(n + 4) / f;
        normal.z = npa_rdf32(n + 8) / f;
#else
        normal.x = *(const float*)((const u8*)layout->address
                                   + mIndex * layout->stride)
                 / f;
        normal.y = *(const float*)((const u8*)layout->address
                                   + mIndex * layout->stride + 4)
                 / f;
        normal.z = *(const float*)((const u8*)layout->address
                                   + mIndex * layout->stride + 8)
                 / f;
#endif
    }
}

void NetMeshModelLoader::NetMeshVertex::GetTextureCoord(
    nlVector2& texCoord) const
{
    const glModelStream* layout = GetStream(mpPacket, 4);

    if (layout->stride == 4)
    {
#ifdef NP_NATIVE
        s16 texCoordX = (s16)npa_rd16((const u8*)layout->address + mIndex * layout->stride);
#else
        s16 texCoordX = *(const s16*)((const u8*)layout->address
                                      + mIndex * layout->stride);
#endif
        texCoord.x = (float)texCoordX;
        texCoord.x *= 0.0009765625f;
#ifdef NP_NATIVE
        s16 texCoordY = (s16)npa_rd16((const u8*)layout->address + mIndex * layout->stride + 2);
#else
        s16 texCoordY = *(const s16*)((const u8*)layout->address
                                      + mIndex * layout->stride + 2);
#endif
        texCoord.y = (float)texCoordY;
        texCoord.y *= 0.0009765625f;
    }
    else if (layout->stride == 8)
    {
        float f = 1.0f;
#ifdef NP_NATIVE
        texCoord.x = npa_rdf32((const u8*)layout->address + mIndex * layout->stride) / f;
        texCoord.y = npa_rdf32((const u8*)layout->address + mIndex * layout->stride + 4) / f;
#else
        texCoord.x = *(const float*)((const u8*)layout->address
                                     + mIndex * layout->stride)
                   / f;
        texCoord.y = *(const float*)((const u8*)layout->address
                                     + mIndex * layout->stride + 4)
                   / f;
#endif
    }
}
