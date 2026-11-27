#include "NL/nlDLListContainer.inl"
#include "Game/BasicStadium.h"
#include "Game/AI/Powerups.h"
#include "Game/Drawable/DrawablePowerup.h"
#include "Game/Render/RLView.h"
#include "NL/gl/glDraw3.h"
#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/nlMath.h"
#include "NL/nlString.h"
#include "Game/Render/RenderShadow.h"
#include "Game/Render/StadiumLoading.h"
#include "Game/SharedStaticStorage.h"
#ifdef NP_NATIVE
#include <string.h>
#include "np/overlap.h"
#include "np/cpu22.h"
#endif

u8 sDrawPowerupShadows = 1;
u8 sUseModelPowerupShadows = 1;

namespace
{
inline char* GetName(int idx)
{
    static char powerupLookup[] = "powerup_generated_xxx";

    if (idx < 10)
    {
        powerupLookup[18] = idx + '0';
        powerupLookup[19] = '\0';
        powerupLookup[20] = '\0';
    }
    else if (idx < 100)
    {
        powerupLookup[18] = (idx / 10) + '0';
        powerupLookup[19] = (idx % 10) + '0';
        powerupLookup[20] = '\0';
    }
    else
    {
        powerupLookup[18] = (idx / 100) + '0';
        powerupLookup[19] = ((idx % 100) / 10) + '0';
        powerupLookup[20] = (idx % 10) + '0';
    }

    return powerupLookup;
}

#ifdef NP_NATIVE
// NP (NPP): the hash of "powerup_generated_<idx>" never changes, but Grab (150 powerups per simulation step) and Render (150 per frame)
// rebuilt the name and hashed 21 characters every time (about 250 instructions per call, with the static name buffer shared). Cached per index.
inline unsigned long PowerupNameHash(int idx)
{
    static unsigned long sHash[256];
    if (idx < 0 || idx >= 256)
    {
        return nlStringLowerHash(GetName(idx));
    }
    unsigned long h = sHash[idx];
    if (h == 0)
    {
        h = nlStringLowerHash(GetName(idx));
        sHash[idx] = h;
    }
    return h;
}
#define NP_POWERUP_HASH(idx) PowerupNameHash(idx)
#else
#define NP_POWERUP_HASH(idx) nlStringLowerHash(GetName(idx))
#endif
} // namespace

static void DrawShadow(float radius, float x, float y, float z)
{
    int maxHeight = 10;
    float frac = z / maxHeight;

    if (frac < 0.0f)
    {
        frac = 0.0f;
    }

    if (frac > 1.0f)
    {
        frac = 1.0f;
    }

    float shadowRadius = (float)((1.0f - frac) * (1.75 * radius) + frac * (4.0f * radius));
    int alpha = (int)(150.0f * (1.0f - frac) + 48.0f * frac);

    if (alpha < 0)
    {
        alpha = 0;
    }

    if (alpha > 0xFF)
    {
        alpha = 0xFF;
    }

    BasicStadium* stadium = BasicStadium::GetCurrentStadium();
    float height = 0.0f;
    if (stadium != 0)
    {
        height = stadium->m_shadowHeight;
    }
    height = 0.015625f + height;

    nlVector3 position;
    position.x = x;
    position.y = y;
    position.z = height;

    float minX;
    float minY = position.y - shadowRadius;
    minX = position.x - shadowRadius;
    float maxY = position.y + shadowRadius;
    float maxX = position.x + shadowRadius;

    glQuad3 quad;
    quad.m_pos[0].x = minX;
    quad.m_pos[0].y = minY;
    quad.m_pos[0].z = position.z;
    quad.m_pos[1].x = minX;
    quad.m_pos[1].y = maxY;
    quad.m_pos[1].z = position.z;
    quad.m_pos[2].x = maxX;
    quad.m_pos[2].y = maxY;
    quad.m_pos[2].z = position.z;
    quad.m_pos[3].x = maxX;
    quad.m_pos[3].y = minY;
    quad.m_pos[3].z = position.z;

    quad.m_uv[0].x = 1.0f;
    quad.m_uv[0].y = 1.0f;
    quad.m_uv[1].x = 0.0f;
    quad.m_uv[1].y = 1.0f;
    quad.m_uv[2].x = 0.0f;
    quad.m_uv[2].y = 0.0f;
    quad.m_uv[3].x = 1.0f;
    quad.m_uv[3].y = 0.0f;

    u8 colour[4];
    colour[0] = 0xFF;
    colour[1] = 0xFF;
    colour[2] = 0xFF;
    colour[3] = (u8)alpha;

    u32 packed = *(u32*)colour;
    *(u32*)&quad.m_colour[3] = packed;
    *(u32*)&quad.m_colour[2] = packed;
    *(u32*)&quad.m_colour[1] = packed;
    *(u32*)&quad.m_colour[0] = packed;

    glSetDefaultState(true);
    glSetRasterState(GLS_AlphaBlend, 1);
    glSetRasterState(GLS_Culling, 0);
    glSetRasterState(GLS_DepthWrite, 0);
    glSetCurrentRasterState(glHandleizeRasterState());

    glSetCurrentTexture(glGetTexture("global/ball_shadow"), GLTT_Diffuse);
    glSetTextureState(GLTS_DiffuseWrap, 3);
    glSetCurrentTextureState(glHandleizeTextureState());

    quad.Attach((eGLView)(u32)GetUnshadowedView(), 0);
}

void DrawablePowerup::Grab(int idx)
{
    PowerupBase* powerup = FindPowerUp(NP_POWERUP_HASH(idx));
    if (powerup != 0)
    {
        mType = powerup->m_eType;
        mVisible = powerup->m_unk20;
        mRadius = powerup->GetRadius();
        mOrientation = powerup->m_aOrientation;
        mPosition = powerup->m_v3Position;
        mScale = powerup->m_scale;
    }
    else
    {
        mVisible = false;
    }
}

#ifdef NP_NATIVE
// MSC_CPU22_POWERUP (include/np/cpu22.h): FindStadiumDrawableObject(hash of "powerup_generated_<idx>") is two AVL walks (the stadium's registered drawables, then the world's
// map) for each of the 150 slots every picture, and its answer only changes when a drawable is added to or removed from the stadium (NpCpu22DrawEpochBump at each writer: fn_8027876C,
// World::AddDrawableObject / RemoveDrawableObject, the World and BasicStadium constructors and destructors) or the current stadium is another one. Answer per slot kept for that state.
static DrawableObject* NpPowerupObject(int idx)
{
    static DrawableObject* sObj[150];
    static unsigned char sKnown[150];
    static unsigned sEpoch = ~0u;
    static const void* sStadium;
    const int mode = NpCpu22Mode(NP_C22_POWERUP);
    if (mode == 0 || idx < 0 || idx >= 150 || !NpMemGetGameThread())
        return FindStadiumDrawableObject(NP_POWERUP_HASH(idx));
    const void* stadium = BasicStadium::GetCurrentStadium();
    if (sEpoch != NpCpu22DrawEpochV || sStadium != stadium)
    {
        memset(sKnown, 0, sizeof sKnown);
        sEpoch = NpCpu22DrawEpochV;
        sStadium = stadium;
    }
    NpCpu22Note(NP_C22_POWERUP, 0);
    if (sKnown[idx])
    {
        NpCpu22Note(NP_C22_POWERUP, 1);
        if (mode == 1)
            return sObj[idx];
        DrawableObject* real = FindStadiumDrawableObject(NP_POWERUP_HASH(idx));
        if (real != sObj[idx])
            NpCpu22Note(NP_C22_POWERUP, 2);
        return real;
    }
    DrawableObject* object = FindStadiumDrawableObject(NP_POWERUP_HASH(idx));
    sObj[idx] = (NpCpu22Break() == 2 && (idx & 7) == 3) ? 0 : object;   // (proof mutant: every 8th slot is kept as "no object")
    sKnown[idx] = 1;
    return object;
}
#endif

void DrawablePowerup::Render(int idx) const
{
#ifndef NP_NATIVE
    FindPowerUp(nlStringLowerHash(GetName(idx)));   // (result unused: a pure lookup; dropped natively)
#endif
#ifdef NP_NATIVE
    DrawableObject* object = NpPowerupObject(idx);
#else
    DrawableObject* object
        = FindStadiumDrawableObject(NP_POWERUP_HASH(idx));
#endif

    if (object == 0)
    {
        return;
    }

#ifdef NP_NATIVE
    // MSC_POWERUP_HIDDEN: a hidden slot whose object is already hidden would only get a new world matrix, which nothing reads until the slot is shown (and then this
    // function sets it before the draw)
    if (!mVisible && (object->m_uObjectFlags & 1) == 0 && NpPowerupHidden())
    {
        return;
    }
#endif

    nlQuaternion orientation;
    float angle = 0.0000958738f * (float)mOrientation;
    fn_802B549C(orientation, (u16)(int)(10430.378f * angle));

    if (mVisible)
    {
        object->m_uObjectFlags |= 1;
    }
    else
    {
        object->m_uObjectFlags &= ~1;
    }

    nlMatrix4 transform;
    nlQuatToMatrix(transform, orientation, true);
    transform.e2[3][0] = mPosition.x;
    transform.e2[3][1] = mPosition.y;
    transform.e2[3][2] = mPosition.z;
    transform.e2[3][3] = 1.0f;

    nlMatrix4 scale;
    nlMakeScaleMatrix(scale, mScale, mScale, mScale);

    nlMatrix4 world;
    nlMultMatrices(world, scale, transform);
    object->SetWorldMatrix(world);

    if (!mVisible)
    {
        return;
    }

    object->Draw();
    if (sDrawPowerupShadows == 0)
    {
        return;
    }

    if (sUseModelPowerupShadows != 0)
    {
        glModel* model = object->m_pModel;
        const nlMatrix4* source = object->GetWorldMatrix();
        glModel* geometry = glModelDupNoStreams(model, false, 0);
        DrawPlanarShadow(geometry, *source, 1, 0, this, 0.5f);
    }
    else
    {
        DrawShadow(mRadius, mPosition.x, mPosition.y, mPosition.z);
    }
}

void DrawablePowerup::Blend(const float* blendFactors, const DrawablePowerup& lhs, const DrawablePowerup& rhs)
{
    mVisible = lhs.mVisible && rhs.mVisible;
    if (!mVisible)
    {
        return;
    }

    float factor = blendFactors[2];
    mType = lhs.mType;
    mScale = (1.0f - factor) * lhs.mScale + factor * rhs.mScale;
    mRadius = lhs.mRadius;
    int orientationDelta = (int)(factor * (s16)(rhs.mOrientation - lhs.mOrientation));
    mOrientation = lhs.mOrientation + (s16)orientationDelta;
    nlVecLerp(mPosition, lhs.mPosition, rhs.mPosition, factor);
}

