#include "Game/Render/depthoffield.h"

#include "Game/SharedStaticStorage.h"
#include "Game/Render/RLView.h"

#include "NL/gl/glDraw2.h"
#include "NL/gl/glState.h"
#include "NL/gl/glView.h"
#include "NL/nlColour.h"
#include "NL/nlMath.h"
#ifdef NP_NATIVE
#include "np/framesend.h"
#include <stdio.h>
#include <stdlib.h>
#endif

typedef float Mtx[3][4];
typedef float Mtx44[4][4];

extern "C"
{
    void GXGetProjectionv(float* projection);
    void GXSetProjection(const Mtx44 projection, int type);
    void GXSetProjectionv(const float* projection);
    void GXGetViewportv(float* viewport);
    void GXProject(float x, float y, float z, const Mtx matrix,
        const float* projection, const float* viewport, float* screenX,
        float* screenY, float* screenZ);
    void PSMTXIdentity(Mtx matrix);
    void* memcpy(void* destination, const void* source, u32 size);
}

u8 lbl_806DCDE0 = true;

DepthOfFieldManager::DepthOfFieldManager()
    : m_bOn(false)
    , m_bDebugView(false)
    , m_fDistanceFromCamera(16.0f)
    , m_fIntensity(1.0f)
    , m_pad0C(0)
{
}

DepthOfFieldManager DepthOfFieldManager::instance;
static const u32 DOFTexture = glGetTexture("target/dof");

void DepthOfFieldManager::Initialize()
{
}

#ifdef NP_NATIVE
// NP: MSC_DOF (STR-081). The game's depth of field is a chain of three parts: the UnsortedPerspective layer copies the frame drawn so far to the
// half-size "dof" texture (every frame, whether the effect is on or not: Update below), the cut-scene script (NIS_TRIGGER_TYPE_DEPTH_OF_FIELD,
// NisPlayer) switches the effect on, and a full-screen quad textured with the half-size copy is drawn at the depth of the focal plane, so that
// only what lies behind it is replaced by the soft picture.
//   MSC_DOF=1 (default)  as the game
//   MSC_DOF=0            the effect never turns on: no quad, and the capture that only the quad would read is not made either (the whole chain,
//                        not just the last quad). The cut-scenes show the sharp picture. A drawing option: the simulation does not see it.
//   MSC_DOF=lazy         the same picture as the game, but the capture is made only on frames that draw the quad (a capture that no quad reads
//                        is dead work: the copy and the quad are both made in the same frame, nothing reads last frame's)
static int DepthOfFieldMode()
{
    static int mode = -1;
    if (mode < 0)
    {
        const char* e = getenv("MSC_DOF");
        mode = 1;
        if (e && *e)
        {
            if (e[0] == '0' || (e[0] == 'o' && e[1] == 'f')) mode = 0;
            else if (e[0] == 'l') mode = 2;
            fprintf(stderr, "[dof] MSC_DOF=%s: depth of field %s\n", e, mode == 0 ? "OFF" : mode == 2 ? "as the game, capture only when drawn" : "as the game");
        }
    }
    return mode;
}
#endif

void DepthOfFieldManager::TurnOn()
{
#ifdef NP_NATIVE
    if (DepthOfFieldMode() == 0)
    {
        m_bOn = false;
        return;
    }
#endif
    m_bOn = true;
}

void DepthOfFieldManager::TurnOff()
{
    m_bOn = false;
}

#ifdef NP_NATIVE
// what Update did to the recorder: the DoF projection in, the saved one back (see Update)
static void DepthOfFieldProjectionDance(const void* data)
{
    float saved[7];
    Mtx44 matrix;
    memcpy(matrix, data, sizeof(matrix));
    GXGetProjectionv(saved);
    GXSetProjection(matrix, 0);
    GXSetProjectionv(saved);
}
#endif

void DepthOfFieldManager::Update()
{
    nlColour colour;
    float screenZ;
    float screenY;
    float screenX;
    nlVector3 position;

    GLView* view = GetLayerView(eCLV_UnsortedPerspective);
#ifdef NP_NATIVE
    // (the capture is made when the frame will draw the quad; MSC_DOF=1 keeps the game's capture on every frame)
    const bool quadFollows = m_bOn && lbl_806DCDE0;
    const int dofMode = DepthOfFieldMode();
    view->m_Target = (dofMode == 1 || (dofMode == 2 && quadFollows)) ? 8 : 0;
    {
        // MSC_DOF_LOG=1: one line each time the effect turns on or off (the cut-scenes that use it)
        static const bool log = getenv("MSC_DOF_LOG") != 0;
        static bool last = false;
        static unsigned frame = 0;
        ++frame;
        if (log && quadFollows != last)
        {
            fprintf(stderr, "[dof] frame %u: depth of field %s (focal distance %.2f)\n", frame, quadFollows ? "ON" : "off", m_fDistanceFromCamera);
            last = quadFollows;
        }
    }
#else
    view->m_Target = 8;
#endif

    if (!m_bOn)
    {
        return;
    }

    if (!lbl_806DCDE0)
    {
        return;
    }

    float value = m_fDistanceFromCamera;
    if (nlAbs(value) < 0.001f)
    {
        value = 0.001f;
    }

    position.x = 0.0f;
    position.y = 0.0f;
    position.z = -value;

    float viewport[6] = { 0.0f, 0.0f, 640.0f, 448.0f, 0.0f, 1.0f };
    float projection[7];
    float savedProjection[7];
    nlMatrix4 projectionMatrix;
    Mtx identity;
    glPoly2 poly;

#ifdef NP_NATIVE
    // NP (two-stage pipeline): the recorder calls (set the projection, set it back) are made after the join of the send that may still run; the depth
    // is computed from the matrix alone: the coefficients GXSetProjection(m, perspective) leaves for GXGetProjectionv, and a viewport whose depth range is 0..1
    // (every GXSetViewport of the game says so), so no GX state is read.
    (void)savedProjection;
    memcpy(&projectionMatrix,
        GetLayerView(eCLV_Shadowed)->m_Interface->GetProjectionMatrix(),
        sizeof(projectionMatrix));
    {
        const float* m = &projectionMatrix.e2[0][0];
        projection[0] = 0.0f;
        projection[1] = m[0];
        projection[2] = m[2];
        projection[3] = m[5];
        projection[4] = m[6];
        projection[5] = m[10];
        projection[6] = m[11];
    }
    PSMTXIdentity(identity);
    GXProject(position.x, position.y, position.z, identity, projection, viewport, &screenX, &screenY, &screenZ);
    float depth = -screenZ * 16777215.0f;
    NpFsDefer(DepthOfFieldProjectionDance, &projectionMatrix.e2[0][0], 16 * sizeof(float));
#else
    GXGetProjectionv(savedProjection);

    memcpy(&projectionMatrix,
        GetLayerView(eCLV_Shadowed)->m_Interface->GetProjectionMatrix(),
        sizeof(projectionMatrix));
    GXSetProjection(projectionMatrix.e2, 0);

    GXGetProjectionv(projection);
    GXGetViewportv(viewport);
    PSMTXIdentity(identity);
    GXProject(position.x, position.y, position.z, identity, projection, viewport, &screenX, &screenY, &screenZ);

    float depth = -screenZ * 16777215.0f;
    GXSetProjectionv(savedProjection);
#endif

    int alpha = (int)(255.5f * m_fIntensity);
    if (alpha < 0)
    {
        alpha = 0;
    }
    if (alpha > 255)
    {
        alpha = 255;
    }

    glSetDefaultState(false);
    glSetTextureState(GLTS_DiffuseWrap, 3);

    if (m_bDebugView)
    {
        unsigned long texture = glGetTexture("global/white");
        glSetCurrentTexture(texture, GLTT_Diffuse);
        glSetRasterState(GLS_AlphaBlend, 1);
        colour.c[0] = 0xFF;
        colour.c[1] = 0xC8;
        colour.c[2] = 0xC8;
        colour.c[3] = 0x80;
    }
    else
    {
        glSetCurrentTexture(DOFTexture, GLTT_Diffuse);
        colour.c[0] = 0xFF;
        colour.c[1] = 0xFF;
        colour.c[2] = 0xFF;
        colour.c[3] = (unsigned char)alpha;
        if (alpha != 0xFF)
        {
            glSetRasterState(GLS_AlphaBlend, 1);
        }
    }

    glSetRasterState(GLS_DepthTest, 1);
    glSetCurrentRasterState(glHandleizeRasterState());
    glSetCurrentTextureState(glHandleizeTextureState());

    poly.SetupRectangle(0.0f, 0.0f, 640.0f, 480.0f, depth);
    poly.SetColour(colour);
    poly.Attach(GetLayerView(eCLV_DepthOfField), 0, 0);
}
