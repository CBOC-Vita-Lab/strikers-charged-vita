#include "Game/GL/GLVertexAnim.h"

#include "Game/GL/glModelBuilder.h"
#include "NL/nlMemory.h"

#include <string.h>

struct GLVertexAnimHeader
{
    u32 hashID;
    u32 numFrames;
    u32 numVertices;
    u32 vertexStride;
    u32 unknown10;
    u32 numAnimatedStreams;
};

GLVertexAnim::GLVertexAnim(const void* data, const void* extraData)
{
    const GLVertexAnimHeader* header =
        (const GLVertexAnimHeader*)data;
    m_uHashID = header->hashID;
    m_nNumFrames = header->numFrames;
    m_nNumVertices = header->numVertices;
    m_nVertexStride = header->vertexStride;
    m_Unknown10 = header->unknown10;
    m_nNumAnimatedStreams = header->numAnimatedStreams;

    m_eMode = GLVAnimMode_Loop;
    m_bDone = false;
    m_fTimeScale = 1.0f;
    m_fFrame = 0.0f;
    m_fFrameRate = 30.0f;
    m_pVertices = 0;
    m_pModel = 0;

    m_pAnimatedStreamIDs = (s32*)nlMalloc(m_nNumAnimatedStreams * sizeof(s32), 8, false);
    memcpy(m_pAnimatedStreamIDs, extraData, m_nNumAnimatedStreams * sizeof(s32));
}

GLVertexAnim::~GLVertexAnim()
{
    delete m_pAnimatedStreamIDs;
}

glModel* GLVertexAnim::GetModel(int frame)
{
    int actualFrame = (frame < 0) ? (int)m_fFrame : frame;

    glModel* model = glModelDupNoStreams(m_pModel, false, 0);
    u8* vertices = m_pVertices
                 + actualFrame * m_nVertexStride * m_nNumVertices;

    for (glModelPacket* packet = model->packets;
         packet < model->packets + model->numPackets; packet++)
    {
        glModelStream* streams = packet->streams;
        glModelStream* endVertexData = streams + packet->numStreams;
        u32 offset = 0;
        for (int i = 0; streams + i < endVertexData; i++)
        {
            for (int j = 0; j < m_nNumAnimatedStreams; j++)
            {
                if (m_pAnimatedStreamIDs[j] == streams[i].id)
                {
                    glSetStreamAddress(&streams[i],
                        vertices + offset * packet->numUniqueVertices);
                    offset += streams[i].stride;
                    break;
                }
            }
        }
        vertices += offset * packet->numUniqueVertices;
    }

    return model;
}

// The stream patching of GetModel without the model copy. A copy made by glModelDupNoStreams shares its streams array with m_pModel (only the packets and the material
// parameters are copied), so GetModel(frame) leaves the SOURCE streams pointing at that frame's vertices: the last call wins for every drop that reads them later. A caller
// that leaves out a GetModel whose copy it throws away (MSC_FX_RAIN_FAST, MSC_FX_MODEL_OFFVIEW) calls this once with the frame of the last call it left out, so that the state
// the picture is drawn from is the same one.
void GLVertexAnim::PatchSharedStreams(int frame)
{
    int actualFrame = (frame < 0) ? (int)m_fFrame : frame;
    u8* vertices = m_pVertices
                 + actualFrame * m_nVertexStride * m_nNumVertices;

    for (glModelPacket* packet = m_pModel->packets;
         packet < m_pModel->packets + m_pModel->numPackets; packet++)
    {
        glModelStream* streams = packet->streams;
        glModelStream* endVertexData = streams + packet->numStreams;
        u32 offset = 0;
        for (int i = 0; streams + i < endVertexData; i++)
        {
            for (int j = 0; j < m_nNumAnimatedStreams; j++)
            {
                if (m_pAnimatedStreamIDs[j] == streams[i].id)
                {
                    glSetStreamAddress(&streams[i],
                        vertices + offset * packet->numUniqueVertices);
                    offset += streams[i].stride;
                    break;
                }
            }
        }
        vertices += offset * packet->numUniqueVertices;
    }
}

void GLVertexAnim::Update(float dt)
{
    if (m_bDone)
    {
        return;
    }

    m_fFrame += m_fFrameRate * (m_fTimeScale * dt);

    if (m_fFrame >= m_nNumFrames)
    {
        if (m_eMode == GLVAnimMode_Hold)
        {
            m_bDone = true;
            m_fFrame = m_nNumFrames - 1;
        }
        else
        {
            m_fFrame = 0.0f;
        }
    }
}
