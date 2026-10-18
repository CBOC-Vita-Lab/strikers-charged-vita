#include "Game/Task/GameTaskState.h"
#include "Game/Camera/GameplayCam.h"
#include "Game/Camera/CameraDamping.h"
#include "Game/Camera/GameplayCameraEffects.h"
#include "Game/Render/RLViewLayers.h"

#include "Game/Ball.h"
#include "Game/AI/AiUtil.h"
#include "Game/GameInfo.h"
#include "Game/MathHelpers.h"
#include "Game/Net.h"
#include "Game/Player.h"
#include "Game/ReplayManager.h"
#include "Game/Team.h"
#include "Game/SharedStaticStorage.h"
#include "NL/nlTask.h"
#include "NL/gl/glMatrix.h"
#ifdef NP_NATIVE
#include "Game/Camera/CameraMan.h"
#include "Game/Field.h"
#include "Game/Task/FixedUpdateTask.h"
#include "np/camzoom.h"
#include <stdio.h>
#include <stdlib.h>
#endif


bool gGameplayCameraOffsetCameraDuringTransition = true;
float gGameplayCameraMaxZoom = 1.0f;
float gGameplayCameraMinZoom = 0.05f;
float gGameplayCameraDynamicZoomOffset = 0.5f;
float gGameplayCameraScorelessNeutralZoom = 0.2f;
float gGameplayCameraScoredNeutralZoom = 0.2f;
float gGameplayCameraNearPitch = 30.0f;
float gGameplayCameraFarPitch = 38.0f;

float gGameplayCameraZoomOverride;
bool gGameplayCameraZoomOverrideEnabled;
bool gGameplayCameraInReplay;

static const float nearZoomPositiveTargetKnotsY[3] = { -5.2f, 0.0f, 10.0f };
static const float nearZoomPositiveFieldKnotsY[3] = { -5.6f, 0.0f, 11.65f };
static const float farZoomPositiveTargetKnotsY[3] = { -2.6f, -2.6f, 2.0f };
static const float farZoomPositiveFieldKnotsY[3] = { 0.0f, 0.0f, 11.65f };

static const float nearZoomPositiveTargetKnotsX[2] = { -16.8f, 16.8f };
static const float nearZoomPositiveFieldKnotsX[2] = { -17.0f, 14.5f };
static const float nearZoomNegativeFieldKnotsX[2] = { -14.5f, 17.0f };
static const float nearZoomNeutralFieldKnotsX[2] = { -15.75f, 15.75f };
static const float farZoomPositiveTargetKnotsX[2] = { -13.3f, 13.3f };
static const float farZoomPositiveFieldKnotsX[2] = { -13.0f, 8.0f };
static const float farZoomNegativeFieldKnotsX[2] = { -8.0f, 13.0f };
static const float farZoomNeutralFieldKnotsX[2] = { -10.5f, 10.5f };
static const float nearZoomWidePositiveTargetKnotsX[2] = { -15.5f, 15.5f };
static const float farZoomWidePositiveTargetKnotsX[2] = { -10.1f, 10.1f };

static const CameraData gCameraData[4] = {
    {
        2,
        3,
        { nearZoomPositiveTargetKnotsX, nearZoomPositiveTargetKnotsX, nearZoomPositiveTargetKnotsX },
        { nearZoomNeutralFieldKnotsX, nearZoomPositiveFieldKnotsX, nearZoomNegativeFieldKnotsX },
        { nearZoomPositiveTargetKnotsY, nearZoomPositiveTargetKnotsY, nearZoomPositiveTargetKnotsY },
        { nearZoomPositiveFieldKnotsY, nearZoomPositiveFieldKnotsY, nearZoomPositiveFieldKnotsY },
        27.3f,
        20.0f,
        gGameplayCameraNearPitch,
        270.0f,
    },
    {
        2,
        3,
        { farZoomPositiveTargetKnotsX, farZoomPositiveTargetKnotsX, farZoomPositiveTargetKnotsX },
        { farZoomNeutralFieldKnotsX, farZoomPositiveFieldKnotsX, farZoomNegativeFieldKnotsX },
        { farZoomPositiveTargetKnotsY, farZoomPositiveTargetKnotsY, farZoomPositiveTargetKnotsY },
        { farZoomPositiveFieldKnotsY, farZoomPositiveFieldKnotsY, farZoomPositiveFieldKnotsY },
        27.3f,
        35.0f,
        gGameplayCameraFarPitch,
        270.0f,
    },
    {
        2,
        3,
        { nearZoomWidePositiveTargetKnotsX, nearZoomWidePositiveTargetKnotsX, nearZoomWidePositiveTargetKnotsX },
        { nearZoomNeutralFieldKnotsX, nearZoomPositiveFieldKnotsX, nearZoomNegativeFieldKnotsX },
        { nearZoomPositiveTargetKnotsY, nearZoomPositiveTargetKnotsY, nearZoomPositiveTargetKnotsY },
        { nearZoomPositiveFieldKnotsY, nearZoomPositiveFieldKnotsY, nearZoomPositiveFieldKnotsY },
        27.5f,
        20.0f,
        gGameplayCameraNearPitch,
        270.0f,
    },
    {
        2,
        3,
        { farZoomWidePositiveTargetKnotsX, farZoomWidePositiveTargetKnotsX, farZoomWidePositiveTargetKnotsX },
        { farZoomNeutralFieldKnotsX, farZoomPositiveFieldKnotsX, farZoomNegativeFieldKnotsX },
        { farZoomPositiveTargetKnotsY, farZoomPositiveTargetKnotsY, farZoomPositiveTargetKnotsY },
        { farZoomPositiveFieldKnotsY, farZoomPositiveFieldKnotsY, farZoomPositiveFieldKnotsY },
        27.5f,
        35.0f,
        gGameplayCameraFarPitch,
        270.0f,
    },
};

static inline float MapFromFieldPosToTargetPos(float fPos, const float* pFieldKnots, const float* pTargetKnots, int nNumKnots)
{
    float fMin = pFieldKnots[0];
    float fMax = pFieldKnots[nNumKnots - 1] - 0.001f;

    float lowerClampedPosition;
    if (fPos >= fMin)
    {
        lowerClampedPosition = fPos;
    }
    else
    {
        lowerClampedPosition = fMin;
    }

    float clampedPosition;
    if (lowerClampedPosition <= fMax)
    {
        clampedPosition = lowerClampedPosition;
    }
    else
    {
        clampedPosition = fMax;
    }

    int nKnot;
    for (nKnot = 0; nKnot < nNumKnots - 1; nKnot++)
    {
        if (clampedPosition < pFieldKnots[nKnot + 1])
        {
            break;
        }
    }

    float fKnotPercent;
    if (pFieldKnots[nKnot] == pFieldKnots[nKnot + 1])
    {
        fKnotPercent = 0.0f;
    }
    else
    {
        fKnotPercent = (clampedPosition - pFieldKnots[nKnot]) / (pFieldKnots[nKnot + 1] - pFieldKnots[nKnot]);
    }

    return Interpolate(pTargetKnots[nKnot], pTargetKnots[nKnot + 1], fKnotPercent);
}

#ifdef NP_NATIVE
// NP (17 Oct, np/camzoom.h): zoom levels 6 and 7 (option 1.25, 1.5) go past the game's near camera along the line from the far camera through
// the near one: the floor of the camera's zoom becomes 1 - option (-0.25, -0.5) instead of gGameplayCameraMinZoom. Up to level 5 the floor
// is the game's own, so levels 1..5 are unchanged.
static float NpZoomFloor()
{
    const float option = GameInfoManager::Instance()->mUserInfo.mVisualOptions.mCameraZoomLevel;
    return option > 1.0f ? 1.0f - option : gGameplayCameraMinZoom;
}
#endif

GameplayCamera::GameplayCamera()
{
    m_bDynamicZoom = true;
    m_fZoom = 0.0f;
    m_fDesiredZoom = 0.0f;
    m_fZoomSeekSpeed = 0.0f;
    m_ForceNeutralAndNearZoom = false;
    m_fZoomOverride = 0.0f;
    m_matView.SetIdentity();
}

void GameplayCamera::Update(float deltaTime)
{
    bool gamePaused = (nlTaskManager::m_pInstance->mCurrentState == TASK_PAUSED)
                    | (nlTaskManager::m_pInstance->mCurrentState == 32);

    m_bDynamicZoom = GameInfoManager::Instance()->mUserInfo.mVisualOptions.mIsAutoZoomCamera;
    m_fDesiredZoom = 1.0f - GameInfoManager::Instance()->mUserInfo.mVisualOptions.mCameraZoomLevel;

    if (IsWidescreen())
    {
        m_nearZoom.m_CameraData = gCameraData + 2;
        m_farZoom.m_CameraData = gCameraData + 3;
    }
    else
    {
        m_nearZoom.m_CameraData = gCameraData;
        m_farZoom.m_CameraData = gCameraData + 1;
    }

    m_nearZoom.Update(deltaTime, m_ForceNeutralAndNearZoom);
    m_farZoom.Update(deltaTime, m_ForceNeutralAndNearZoom);

    if (m_fZoomOverride != 0.0f || gGameplayCameraZoomOverrideEnabled)
    {
        m_fZoom = m_fZoomOverride;
        if (gGameplayCameraZoomOverrideEnabled)
        {
            m_fZoom = gGameplayCameraZoomOverride;
        }
    }
    else if (m_ForceNeutralAndNearZoom)
    {
        if (g_pTeams[0]->m_nScore == 0 && g_pTeams[1]->m_nScore == 0)
        {
            m_fZoom = gGameplayCameraScorelessNeutralZoom;
        }
        else
        {
            m_fZoom = gGameplayCameraScoredNeutralZoom;
        }
    }
    else
    {
        if (m_bDynamicZoom && !gGameplayCameraInReplay
            && !GameplayCameraEffects::Instance()->IsTransitionActive())
        {
            m_fDesiredZoom = 1.0f - GameInfoManager::Instance()->mUserInfo.mVisualOptions.mCameraZoomLevel;
            m_fDesiredZoom = m_fDesiredZoom - gGameplayCameraDynamicZoomOffset;
            m_fDesiredZoom = m_fDesiredZoom + GameplayCameraEffects::Instance()->GetZoomScale();
#ifdef NP_NATIVE
            m_fDesiredZoom = nlMinEquals(nlMaxEquals(m_fDesiredZoom, NpZoomFloor()), 1.2f * gGameplayCameraMaxZoom);
#else
            m_fDesiredZoom = nlMinEquals(nlMaxEquals(m_fDesiredZoom, gGameplayCameraMinZoom), 1.2f * gGameplayCameraMaxZoom);
#endif
        }

        m_fDesiredZoom = Interpolate(gGameplayCameraMinZoom, gGameplayCameraMaxZoom, m_fDesiredZoom);
        float smoothTime;
        if (gamePaused)
        {
            smoothTime = 0.1f;
        }
        else
        {
            smoothTime = 0.75f;
        }

        float change;
        float x;
        float omega = 2.0f / smoothTime;
        x = omega * deltaTime;
        float exp = 1.0f / (((0.48f * x * x) + (1.0f + x)) + (x * (0.235f * x * x)));
        change = m_fZoom - m_fDesiredZoom;
        float temp = deltaTime * ((omega * change) + m_fZoomSeekSpeed);

        m_fZoomSeekSpeed = exp * (m_fZoomSeekSpeed - (omega * temp));
        m_fZoom = (exp * (change + temp)) + m_fDesiredZoom;
    }

    float inverseZoom = 1.0f - m_fZoom;
    float zoom = m_fZoom;

    m_v3Target.x = (inverseZoom * m_nearZoom.m_v3Target.x) + (zoom * m_farZoom.m_v3Target.x);
    m_v3Target.y = (inverseZoom * m_nearZoom.m_v3Target.y) + (zoom * m_farZoom.m_v3Target.y);
    m_v3Target.z = (inverseZoom * m_nearZoom.m_v3Target.z) + (zoom * m_farZoom.m_v3Target.z);

    m_v3Camera.x = (inverseZoom * m_nearZoom.m_v3Camera.x) + (zoom * m_farZoom.m_v3Camera.x);
    m_v3Camera.y = (inverseZoom * m_nearZoom.m_v3Camera.y) + (zoom * m_farZoom.m_v3Camera.y);
    m_v3Camera.z = (inverseZoom * m_nearZoom.m_v3Camera.z) + (zoom * m_farZoom.m_v3Camera.z);

    m_fFOV = Interpolate(m_nearZoom.GetFOV(), m_farZoom.GetFOV(), m_fZoom);

#ifdef NP_NATIVE
    // (levels 6 and 7: the positions follow the zoom down to the closest the level goes, the floor above in camera units; up to level 5 the game's clamp)
    const float npFloor = NpZoomFloor();
    const float npPositionFloor = npFloor < gGameplayCameraMinZoom ? Interpolate(gGameplayCameraMinZoom, gGameplayCameraMaxZoom, npFloor) : gGameplayCameraMinZoom;
    float clampedZoom = nlMinEquals(nlMaxEquals(m_fZoom, npPositionFloor), gGameplayCameraMaxZoom);
#else
    float clampedZoom = nlMinEquals(nlMaxEquals(m_fZoom, gGameplayCameraMinZoom), gGameplayCameraMaxZoom);
#endif
    m_v3Camera.x = Interpolate(m_nearZoom.m_v3Camera.x, m_farZoom.m_v3Camera.x, clampedZoom);
    m_v3Target.x = Interpolate(m_nearZoom.m_v3Target.x, m_farZoom.m_v3Target.x, clampedZoom);
    m_v3Camera.y = Interpolate(m_nearZoom.m_v3Camera.y, m_farZoom.m_v3Camera.y, clampedZoom);
    m_v3Target.y = Interpolate(m_nearZoom.m_v3Target.y, m_farZoom.m_v3Target.y, clampedZoom);

    GameplayCameraEffects::Instance()->AdjustCameraVectors(m_fZoom, &m_v3Camera, &m_v3Target);

    nlVector3 up = mUpVector;
    nlVector3 camera = m_v3Camera;
    nlVector3 target = m_v3Target;

    if (GameplayCameraEffects::Instance()->IsTransitionActive() == true
        && !gamePaused && !gGameplayCameraInReplay
        && ReplayManager::Instance()->mRender != NULL)
    {
        up = GameplayCameraEffects::Instance()->RotateCameraVector(mUpVector);
        nlVector3 targetOffset = GameplayCameraEffects::Instance()->CalculateTargetOffset(this);
        if (gGameplayCameraOffsetCameraDuringTransition == true)
        {
            nlVec3Add(camera, camera, targetOffset);
        }
        nlVec3Add(target, target, targetOffset);
        float fovIncrease = GameplayCameraEffects::Instance()->mTransitionScale;
        m_fFOV += fovIncrease;
    }

    glMatrixLookAt(m_matView, camera, target, up);
}

void GameplayCamera::SetForceNeutralAndNearZoom(bool forceNeutralAndNearZoom)
{
    if (forceNeutralAndNearZoom && !m_ForceNeutralAndNearZoom)
    {
        Reactivate();
        m_v3Target.x = 0.0f;
        m_v3Target.y = 0.0f;
        m_v3Target.z = 0.0f;
    }
    m_ForceNeutralAndNearZoom = forceNeutralAndNearZoom;
}

void GameplayCamera::Reactivate()
{
    m_nearZoom.CalcDesiredTarget();
    m_nearZoom.m_fDampenedTargetX = m_nearZoom.m_fDesiredTargetX;
    m_nearZoom.m_fDampenedTargetY = m_nearZoom.m_fDesiredTargetY;

    m_farZoom.CalcDesiredTarget();
    m_farZoom.m_fDampenedTargetX = m_farZoom.m_fDesiredTargetX;
    m_farZoom.m_fDampenedTargetY = m_farZoom.m_fDesiredTargetY;

    if (m_pFilter[1] != NULL)
    {
        m_pFilter[1]->Reset();
    }
}

void GameplayCameraZoomLevel::CalcDesiredTarget()
{
    nlVector3 v3OOIPos = { 0.0f, 0.0f, 0.0f };
    float fKnotTableBlendWeights[3];
    float fCurrWeight;
    float fDampenedBlendRiser;
    float fAccumulatedWeight;
    float fBlendPercent;
    int i;

    if (!gGameplayCameraInReplay)
    {
        if (g_pBall != NULL)
        {
            cPlayer* pBallOwner = g_pBall->m_pOwner;
            if (pBallOwner != NULL)
            {
                v3OOIPos = pBallOwner->m_DetChar.m_v3Position;
            }
            else if (ReplayManager::Instance()->mRender != NULL)
            {
                v3OOIPos = ReplayManager::Instance()->mRender->mBall.mPosition;
            }
            else
            {
                v3OOIPos = g_pBall->m_v3Position;
            }
        }
    }
    else if (ReplayManager::Instance()->mRender != NULL)
    {
        v3OOIPos = ReplayManager::Instance()->mRender->mBall.mPosition;
    }

    fKnotTableBlendWeights[0] = 0.0f;
    fKnotTableBlendWeights[1] = 0.0f;
    fKnotTableBlendWeights[2] = 0.0f;

    fCurrWeight = 1.0f;
    for (int j = 0; j < 5; j++)
    {
        fDampenedBlendRiser = m_KnotTableBlendQueue[j].fBlendRiser;
        fKnotTableBlendWeights[m_KnotTableBlendQueue[j].nKnotTable] += fCurrWeight * fDampenedBlendRiser;
        if (fDampenedBlendRiser == 1.0f)
        {
            break;
        }
        fCurrWeight *= 1.0f - fDampenedBlendRiser;
    }

    fAccumulatedWeight = 0.0f;
    m_fDesiredTargetX = 0.0f;
    m_fDesiredTargetY = 0.0f;

    i = 0;
    for (; i < 3; i++)
    {
        if (fKnotTableBlendWeights[i] > 0.0f)
        {
            fAccumulatedWeight += fKnotTableBlendWeights[i];
            float fMappedX = MapFromFieldPosToTargetPos(v3OOIPos.x, m_CameraData->fieldKnotsX[i], m_CameraData->targetKnotsX[i], m_CameraData->numKnotsX);
            float fMappedY = MapFromFieldPosToTargetPos(v3OOIPos.y, m_CameraData->fieldKnotsY[i], m_CameraData->targetKnotsY[i], m_CameraData->numKnotsY);
            fBlendPercent = fKnotTableBlendWeights[i] / fAccumulatedWeight;

            m_fDesiredTargetX = Interpolate(m_fDesiredTargetX, fMappedX, fBlendPercent);
            m_fDesiredTargetY = Interpolate(m_fDesiredTargetY, fMappedY, fBlendPercent);
        }
    }
}

static void CalcCurrentKnotTable(GameplayCameraZoomLevel* self, bool forceNeutral)
{
    cPlayer* pBallOwner;
    if (g_pBall != NULL)
    {
        pBallOwner = g_pBall->m_pOwner;
    }
    else
    {
        pBallOwner = NULL;
    }

    if (pBallOwner == NULL)
    {
        pBallOwner = g_pBall->GetPassTarget();
    }

    int nNewKnotTable;
    if (pBallOwner != NULL && !forceNeutral)
    {
        if (pBallOwner->m_pTeam->GetOtherNet()->m_v3NetLocation.x > 0.0f)
        {
            nNewKnotTable = 1;
        }
        else
        {
            nNewKnotTable = 2;
        }
    }
    else
    {
        nNewKnotTable = 0;
    }

    if (nNewKnotTable != self->m_KnotTableBlendQueue[0].nKnotTable)
    {
        for (int i = 4; i > 0; i--)
        {
            self->m_KnotTableBlendQueue[i] = self->m_KnotTableBlendQueue[i - 1];
        }
        self->m_KnotTableBlendQueue[0].nKnotTable = nNewKnotTable;
        self->m_KnotTableBlendQueue[0].fBlendRiser = 0.0f;
    }

    if (forceNeutral)
    {
        self->m_KnotTableBlendQueue[0].fBlendRiser = 1.0f;
    }
}

void GameplayCameraZoomLevel::Update(float fDeltaT, bool forceNeutral)
{
    float fSin;
    float fCos;
    float fOrientSin;
    float fOrientCos;
    float fXYDist;
    int i;

    if (gGameplayCameraInReplay)
    {
        forceNeutral = true;
    }

    CalcCurrentKnotTable(this, forceNeutral);

    float t = fDeltaT / 0.75f;
    for (i = 0; i < 5; i++)
    {
        m_KnotTableBlendQueue[i].fBlendRiser += t;
        if (m_KnotTableBlendQueue[i].fBlendRiser >= 1.0f)
        {
            m_KnotTableBlendQueue[i].fBlendRiser = 1.0f;
            break;
        }
    }

    CalcDesiredTarget();

    if (!forceNeutral)
    {
        m_fDampenedTargetX = Dampen(m_fDampenedTargetX, m_fDesiredTargetX, m_fTargetSeekSpeedX, m_fTargetSeekTime, fDeltaT);
        m_fDampenedTargetY = Dampen(m_fDampenedTargetY, m_fDesiredTargetY, m_fTargetSeekSpeedY, m_fTargetSeekTime, fDeltaT);
    }
    else
    {
        m_fDampenedTargetX = m_fDesiredTargetX;
        m_fDampenedTargetY = m_fDesiredTargetY;
    }

    nlSinCos(&fSin, &fCos, DegreesToAngle(m_CameraData->pitch));
    nlSinCos(&fOrientSin, &fOrientCos, DegreesToAngle(m_CameraData->orientation));

    fXYDist = fCos * m_CameraData->distance;
    m_v3Camera.x = (fOrientCos * fXYDist) + m_fDampenedTargetX;
    m_v3Camera.y = (fOrientSin * fXYDist) + m_fDampenedTargetY;
    m_v3Camera.z = fSin * m_CameraData->distance;

    m_v3Target.x = m_fDampenedTargetX;
    m_v3Target.y = m_fDampenedTargetY;
    m_v3Target.z = 0.0f;
}

#ifdef NP_NATIVE
extern "C" unsigned long long NpHarnessFrameCount(void) __attribute__((weak));

// W5-CAM: MSC_CAMERA_LOG=1 (or N: every Nth frame): one line per frame with the projection the frame used and what the match camera looks at
// (np/camzoom.h; read by tools/cam/). Reads only; draws no random number, changes nothing.
void NpCameraLogFrame(float fovDeg, float aspect)
{
    static int every = -1;
    if (every < 0)
    {
        const char* e = getenv("MSC_CAMERA_LOG");
        every = (e && *e && *e != '0') ? atoi(e) : 0;
        if (every < 0) every = 0;
    }
    if (every == 0) return;
    const unsigned long long frame = NpHarnessFrameCount ? NpHarnessFrameCount() : 0;
    if (frame % (unsigned long long)every != 0) return;

    const nlMatrix4& v = cCameraManager::m_matView;
    cBaseCamera* top = cCameraManager::PeekCamera();
    GameplayCamera* gc = cCameraManager::GetCamera<GameplayCamera>(eCameraType_Gameplay);
    GameplayCameraEffects* fx = GameplayCameraEffects::s_pInstance;
    const VisualSettings& vis = GameInfoManager::Instance()->mUserInfo.mVisualOptions;
    char buf[1400];
    int n = snprintf(buf, sizeof buf,
                     "[cam] f=%llu st=%d top=%d tr=%d fov=%.5f asp=%.4f rm=%u auto=%d lvl=%.3f ds=%08x",
                     frame, GetFixedUpdateTask() != NULL ? GetFixedUpdateTask()->GetFrame() : -1, top ? (int)top->GetType() : -1, (int)cCameraManager::m_transition, (double)fovDeg, (double)aspect,
                     (unsigned)cCameraManager::m_aJoystickRemap, (int)vis.mIsAutoZoomCamera, (double)vis.mCameraZoomLevel, nlDefaultSeed);
    if (gc != NULL)
        n += snprintf(buf + n, sizeof buf - n, " z=%.4f dz=%.4f gfov=%.4f force=%d", (double)gc->m_fZoom, (double)gc->m_fDesiredZoom,
                      (double)gc->m_fFOV, (int)gc->m_ForceNeutralAndNearZoom);
    if (fx != NULL)
        n += snprintf(buf + n, sizeof buf - n, " fl=%x zs=%.3f trn=%d", (unsigned)fx->mCameraFlags, (double)fx->mZoomScale,
                      (int)fx->IsTransitionActive());
    n += snprintf(buf + n, sizeof buf - n, " fd=%.3f,%.3f V=", (double)cField::mv3FieldPosition.x, (double)cField::mv3FieldPosition.y);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            n += snprintf(buf + n, sizeof buf - n, "%s%.6g", (r | c) ? "," : "", (double)v.e2[r][c]);
    if (g_pBall != NULL && g_pTeams[0] != NULL && g_pTeams[1] != NULL)
    {
        int owner = -1;
        n += snprintf(buf + n, sizeof buf - n, " b=%.3f,%.3f,%.3f P=", (double)g_pBall->m_v3Position.x, (double)g_pBall->m_v3Position.y,
                      (double)g_pBall->m_v3Position.z);
        char ctl[16];
        int nc = 0;
        for (int t = 0; t < 2; t++)
            for (int i = 0; i < 5; i++)
            {
                cPlayer* p = g_pTeams[t]->m_pPlayers[i];
                if (p == NULL)
                {
                    n += snprintf(buf + n, sizeof buf - n, "%s-", (t | i) ? ";" : "");
                    continue;
                }
                const nlVector3& pos = p->m_DetChar.m_v3Position;
                n += snprintf(buf + n, sizeof buf - n, "%s%.3f,%.3f,%.3f", (t | i) ? ";" : "", (double)pos.x, (double)pos.y, (double)pos.z);
                if (g_pBall->m_pOwner == p) owner = t * 5 + i;
                if (p->m_pController != NULL && nc < 15) ctl[nc++] = (char)('0' + t * 5 + i);
            }
        ctl[nc] = 0;
        // the players' on-screen flags: what the renderer saw (cCharacter::fn_8001E184, Indicators.cpp's projection) and what the simulation
        // holds (m_DetChar.m_bOnScreen, set from machine 0's DETERM_PLAYER_VISIBILITY message every 10 simulation frames); bit t * 5 + i
        unsigned vis = 0, det = 0;
        for (int t = 0; t < 2; t++)
            for (int i = 0; i < 5; i++)
            {
                cPlayer* p = g_pTeams[t]->m_pPlayers[i];
                if (p == NULL) continue;
                if (p->fn_8001E184()) vis |= 1u << (t * 5 + i);
                if (p->m_DetChar.m_bOnScreen) det |= 1u << (t * 5 + i);
            }
        n += snprintf(buf + n, sizeof buf - n, " own=%d ctl=%s vis=%03x dvis=%03x", owner, nc ? ctl : "-", vis, det);
    }
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    fputs(buf, stderr);
}
#endif
