#include "NL/nlDLListContainer.inl"
#include "np/gsplit.h"
#include "Game/Task/ParticleUpdateTask.h"

#include "Game/Render/Amortize.h"

#include "Game/Effects/EmissionManager.h"
#include "Game/SharedStaticStorage.h"

ParticleUpdateTask* ParticleUpdateTask::sInstance;

ParticleUpdateTask::ParticleUpdateTask()
    : mTimeScale(1.0f)
    , mResetPending(false)
    , mContext(0)
    , mNumParticles(0)
    , mMaxRenderedParticles(0)
    , mRenderEnabled(true)
    , mUpdateEnabled(true)
{
    sInstance = this;
    mUnknownFlag = false;
}

void ParticleUpdateTask::SetTimeScale(float timeScale)
{
    mTimeScale = timeScale;
}

void ParticleUpdateTask::Run(float dt)
{
    if (mResetPending)
    {
        mResetPending = false;
    }

    if (mBeforeUpdate)
    {
        mBeforeUpdate();
    }

    bool update = true;
    if (mCanUpdate)
    {
        update = mCanUpdate();
    }
    if (mUpdateEnabled && update)
    {
        EmissionManager* manager = EmissionManager::Instance();
        NP_GS(NPGS_P_UPDATE);
        manager->Update(dt * mTimeScale);
    }

    bool render = true;
    if (mCanRender)
    {
        render = mCanRender();
    }
    if (mRenderEnabled && render)
    {
        EmissionManager* manager = EmissionManager::Instance();
#ifdef NP_NATIVE
        AmortizeHazeBeginFrame();   // MSC_FX_HAZE_CACHE
#endif
        NP_GS(NPGS_P_RENDER);
        manager->Render();
    }
}

void ParticleUpdateTask::Shutdown()
{
    if (EmissionManager::Instance() != 0)
    {
        EmissionManager::Instance()->Shutdown();
    }
}

void ParticleUpdateTask::Initialize(void* context, int numParticles, int maxRenderedParticles)
{
    mContext = context;
    mNumParticles = numParticles;
    mMaxRenderedParticles = maxRenderedParticles;
    EmissionManager::Instance();
    EmissionManager::Instance()->Startup(context, numParticles, maxRenderedParticles);
}

void ParticleUpdateTask::StartLoading(bool allocateAtStart, bool allocateNonResidentAtStart, bool third, bool compressedNonResident)
{
    EmissionManager::StartLoading(allocateAtStart, allocateNonResidentAtStart, third, compressedNonResident);
}

bool ParticleUpdateTask::FinishLoading(GLResourcePool* context)
{
    return EmissionManager::FinishLoading(context);
}
