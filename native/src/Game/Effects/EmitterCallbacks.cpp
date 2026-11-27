#include "Game/Task/GameTaskState.h"
#include "NL/nlDLListContainer.inl"
#include "Game/Effects/EmitterCallbacks.h"
#include "Game/Ball.h"
#include "Game/BallTrail.h"
#include "Game/Character.h"
#include "Game/Drawable/DrawableCharacter.h"
#include "Game/Effects/EmissionController.h"
#include "Game/Effects/ParticleSystem.h"
#include "Game/Game.h"
#include "Game/Player.h"
#include "Game/PoseAccumulator.h"
#include "Game/Render/ImpostorCharacter.h"
#include "Game/ReplayManager.h"
#include "Game/SHierarchy.h"
#include "Game/Team.h"
#include "NL/nlTask.h"
#include "Game/SharedStaticStorage.h"

static const nlVector3 sZeroVelocity = { 0.0f, 0.0f, 0.0f };

DrawableCharacter* GetReplayDrawableCharacter(cCharacter* character)
{
    int index;
    if (character->m_eClassType == FIELDER)
    {
        cPlayer* player = (cPlayer*)character;
        index = player->m_DetPlayer.m_ID + 4 * player->m_pTeam->m_nSide;
    }
    else
    {
        cPlayer* player = (cPlayer*)character;
        index = player->m_pTeam->m_nSide + 8;
    }
    ReplayManager* manager = ReplayManager::Instance();
    DrawableCharacter* drawable = &manager->mRender->mCharacters[index];
#ifdef NP_NATIVE
    if (drawable->poseCulled)   // (MSC_POSE_CULL=2) its blended pose is old: the tick's own pose, at most a tick ahead of the blended one, for a player off the screen
    {
        return &manager->mCurrent->mCharacters[index];
    }
#endif
    return drawable;
}

void UpdateEmitterFromCharacterUnculled(EmissionController& controller)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (ReplayManager::Instance()->mRender != 0)
    {
        cCharacter* character = (cCharacter*)controller.m_uUserData;
        DrawableCharacter* drawableCharacter = GetReplayDrawableCharacter(character);
        controller.SetPosition(drawableCharacter->position);
        controller.SetVelocity(drawableCharacter->velocity);
        controller.SetPoseAccumulator(
            *drawableCharacter->poseAccumulator);
        controller.SetAnimController(
            drawableCharacter->GetAnimController());
    }
}

void UpdateEmitterFromCharacterWithoutAnimController(
    EmissionController& controller, void* context)
{
    cCharacter* character = (cCharacter*)context;
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (ReplayManager::Instance()->mRender != 0)
    {
        DrawableCharacter* drawableCharacter = GetReplayDrawableCharacter(character);
        controller.SetPosition(drawableCharacter->position);
        controller.SetVelocity(drawableCharacter->velocity);
        controller.SetPoseAccumulator(
            *drawableCharacter->poseAccumulator);
    }
}

void UpdateEmitterFromCharacter(EmissionController& ec)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (ReplayManager::Instance()->mRender != 0)
    {
        UpdateEmitterFromCharacterUnculled(ec);

        cCharacter* pCharacter
            = DrawableCharacter::OnlyRenderingOneCharacter();
        if (pCharacter != 0
            && (cCharacter*)ec.m_uUserData != pCharacter)
        {
            ec.Die();
        }
    }
}

void UpdateEmitterPoseFromCharacter(EmissionController& emitter)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (ReplayManager::Instance()->mRender != 0)
    {
        cCharacter* character = (cCharacter*)emitter.m_uUserData;
        DrawableCharacter* pChar = GetReplayDrawableCharacter(character);
        emitter.SetPoseAccumulator(
            *pChar->poseAccumulator);
        emitter.SetAnimController(
            pChar->GetAnimController());
    }
}

void UpdateEmitterFromBall(EmissionController& emitter)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (ReplayManager::Instance()->mRender != 0)
    {
        if (!g_pGame->IsGameplayOrOvertime()
            && nlTaskManager::m_pInstance->mCurrentState != TASK_REPLAY)
        {
            emitter.m_bVisible = ReplayManager::Instance()
                                     ->mRender->mBall.mFlags.bits.visible;
        }

        ReplayManager* manager = ReplayManager::Instance();
        emitter.SetPosition(manager->mRender->mBall.mPosition);
        manager = ReplayManager::Instance();
        emitter.SetVelocity(manager->mRender->mBall.mVelocity);
    }
}

void UpdateEmitterFromBallTrail(EmissionController& controller)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (controller.m_Replaying == 0
        && ReplayManager::Instance()->mRender != 0)
    {
        LiveBallTrail* trail
            = (LiveBallTrail*)controller.m_uUserData;
        if (trail->visible)
        {
            controller.SetPosition(trail->position);
            controller.SetVelocity(trail->velocity);
#ifdef NP_NATIVE
            // MSC_FX_ATTACH (draw side only): the emitter is placed at the live trail's simulated position (the emission, its counts and rolls, are unchanged); the trail's model is
            // drawn from the blended snapshot (DrawableBall::Blend). The particles are drawn offset by the difference, newest particles fully (ParticleSystem::UpdateParticle).
            nlVec3Set(controller.m_vDrawOffset, 0.0f, 0.0f, 0.0f);
            if (NpFxAttachOn())
            {
                const unsigned int count = GetNumBallTrails();
                const DrawableBall& drawn = ReplayManager::Instance()->mRender->mBall;
                for (unsigned int i = 0; i < count && i < drawn.mTrailCount && i < 10; ++i)
                {
                    if (GetBallTrail(i) != trail)
                        continue;
                    if (drawn.mTrail[i].visible)
                    {
                        const float dx = drawn.mTrail[i].position.x - trail->position.x;
                        const float dy = drawn.mTrail[i].position.y - trail->position.y;
                        const float dz = drawn.mTrail[i].position.z - trail->position.z;
                        if (dx * dx + dy * dy + dz * dz < 2.25f)   // (a jump of 1.5 m or more is a reset or a teleport, not a blend: no streak)
                            nlVec3Set(controller.m_vDrawOffset, dx, dy, dz);
                    }
                    break;
                }
            }
#endif
        }
        else
        {
            controller.Die();
        }
    }
}

void UpdateEmitterFromBallLandingSpot(EmissionController& controller)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (controller.m_Replaying == 0)
    {
        cBall* ball = (cBall*)controller.m_uUserData;
        if (ball->m_v3Position.z > gHeaderTargetPredictionHeight)
        {
            nlVector3 position;
            ball->PredictLandingSpotAndTime(
                position, 0, 0, gHeaderTargetPredictionHeight);
            controller.SetPosition(position);
            controller.SetVelocity(sZeroVelocity);
        }
    }
}

void UpdateEmitterFromCharacterHead(EmissionController& controller)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (controller.m_Replaying == 0
        && ReplayManager::Instance()->mRender != 0)
    {
        cCharacter* character = (cCharacter*)controller.m_uUserData;
        DrawableCharacter* drawableCharacter = GetReplayDrawableCharacter(character);
        controller.SetPosition(drawableCharacter->position);
        controller.SetVelocity(drawableCharacter->velocity);
        controller.SetPoseAccumulator(
            *drawableCharacter->poseAccumulator);

        nlMatrix4& matrix
            = drawableCharacter->poseAccumulator->GetNodeMatrix(
                character->m_nHeadJointIndex);
        nlVector3 direction;
        nlVec3Set(direction, matrix.m21, matrix.m22, matrix.m23);
        nlVector3 adjustedDirection = direction;
        adjustedDirection.z = -0.01f;
        nlVec3Scale(adjustedDirection,
            nlRecipSqrt(
                nlVec3DotProduct(adjustedDirection, adjustedDirection),
                true));
        if (nlVec3DotProduct(direction, adjustedDirection) > 0.9f)
        {
            direction = adjustedDirection;
        }
        controller.SetDirection(direction);
    }
}

void UpdateEmitterFromCharacterBackward(EmissionController& controller)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (controller.m_Replaying == 0
        && ReplayManager::Instance()->mRender != 0)
    {
        cCharacter* character = (cCharacter*)controller.m_uUserData;
        DrawableCharacter* drawableCharacter = GetReplayDrawableCharacter(character);
        controller.SetPosition(drawableCharacter->position);
        controller.SetVelocity(drawableCharacter->velocity);
        controller.SetPoseAccumulator(
            *drawableCharacter->poseAccumulator);

        nlMatrix4& matrix
            = drawableCharacter->poseAccumulator->GetNodeMatrix(0);
        nlVector3 direction;
        nlVec3Set(direction, matrix.m11, matrix.m12, matrix.m13);
        direction.x *= -1.0f;
        direction.y *= -1.0f;
        controller.SetDirection(direction);
    }
}

void UpdateEmitterFromCharacterForward(EmissionController& controller)
{
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (controller.m_Replaying == 0
        && ReplayManager::Instance()->mRender != 0)
    {
        cCharacter* character = (cCharacter*)controller.m_uUserData;
        DrawableCharacter* drawableCharacter = GetReplayDrawableCharacter(character);
        controller.SetPosition(drawableCharacter->position);
        controller.SetVelocity(drawableCharacter->velocity);
        controller.SetPoseAccumulator(
            *drawableCharacter->poseAccumulator);

        drawableCharacter->poseAccumulator->m_BaseSHierarchy
            ->GetNodeIndexByID(character->m_nHeadJointIndex);
        nlMatrix4& matrix
            = drawableCharacter->poseAccumulator->GetNodeMatrix(0);
        nlVector3 direction;
        nlVec3Set(direction, matrix.m11, matrix.m12, matrix.m13);
        controller.SetDirection(direction);
    }
}

void UpdateEmitterFromImpostorModel(EmissionController& controller,
    void* context)
{
    ImpostorModel* model = (ImpostorModel*)context;
    if (g_pGame == 0 || g_pGame->m_eGameState == GS_UNLOADING)
    {
        return;
    }

    if (ReplayManager::Instance()->mRender != 0)
    {
        nlVector3 direction;
        nlVector3 velocity = { 0.0f, 0.0f, 0.0f };
        controller.SetPosition(model->mWorldMatrix.GetTranslation());
        controller.SetVelocity(velocity);
        controller.SetPoseAccumulator(*model->mPoseAccumulator);
        nlVec3Set(direction, model->mWorldMatrix.m11, model->mWorldMatrix.m12, model->mWorldMatrix.m13);
        controller.SetDirection(direction);
    }
}
