#include "NL/nlDLListContainer.inl"
#include "Game/Render/WorldNPC.h"

#include "Game/SAnim/pnSAnimController.h"
#include "Game/TweakRegistry.h"
#include "Game/TweakConfig.h"
#include "NL/gl/glMemory.h"
#include "NL/nlList.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"
#include "NL/platvmath.h"
#include "np/overlap.h"
#include "np/crowdcut.h"
#include "port/parallel.h"
#include "Game/PoseAccumulator.h"
#include "Game/Render/ImpostorModel.h"
#include "np/perfmode.h"
#include "np/stadiumcut.h"
#include "np/scenecut.h"
#include "Game/Render/CrowdImpostors.h"
#include "Game/SharedStaticStorage.h"
#include "Game/World/WorldObject.h"

bool gDisableWorldNPCs;
WorldNPCManager* gpWorldNPCManager;

static const char* sWorldNPCTweakPath = "/Render/WorldNPCs";

inline void WorldNPCManager::RegisterObject(WorldNPC* npc)
{
    unsigned long templateHash;
    bool found;
    int index;

    found = false;
    index = 0;
    templateHash = npc->mTemplateHash;
    for (; index < mNumTemplates; ++index)
    {
        if (templateHash
            == nlStringLowerHash(mTemplates[index].mName))
        {
            found = true;
            break;
        }
    }
    if (found)
    {
        mSelectedTemplates[index] = true;
    }
    mPendingWorldNPCs.AddEnd(npc);
}

struct TemplateLookupResult
{
    int index;
    bool found;
};

static inline void FindLoadedTemplate(const WorldNPCManager& manager,
    unsigned long templateHash, TemplateLookupResult& result)
{
    result.found = false;
    result.index = 0;
    for (; result.index < manager.mNumLoadTemplates; ++result.index)
    {
        if (templateHash
            == nlStringLowerHash(manager.mLoadTemplates[result.index].mName))
        {
            result.found = true;
            break;
        }
    }
}

WorldNPCManager::WorldNPCManager()
    : mPadding004(false)
    , mNumTemplates(0)
    , mNumLoadTemplates(0)
    , mNumLoadedModels(0)
    , mTemplatesLoaded(false)
    , mModelsLoaded(false)
    , mModelCallback(0)
{
    mModelCollection = new (8, false) CrowdModelCollection;
    gpWorldNPCManager = this;
}

WorldNPCManager::~WorldNPCManager()
{
    delete mModelCollection;

    for (int i = 0; i < mNumLoadedModels; ++i)
    {
        delete mLoadedModels[i];
    }

    mWorldNPCs.Clear();
    mPendingWorldNPCs.Clear();
    gpWorldNPCManager = 0;
}

void WorldNPCManager::LoadTemplates(const char*)
{
    LoadTweakConfigFile("ini/WorldNPCs.ini", sWorldNPCTweakPath, false);
    TweakEntry* entry
        = FindOrCreateTweakPath(GetTweakRoot(), sWorldNPCTweakPath, true);

    for (TweakNode* child = entry->m_ChildHead; child != 0;
        child = child->m_Next)
    {
        AddTemplate(child, GetTweakNodeName(child));
    }
    mTemplatesLoaded = true;
}

static char sWorldNPCAnimationFileKey[] = "AnimationFile";
static char sWorldNPCHierarchyFileKey[] = "HierarchyFile";
static char sWorldNPCHierarchyNameKey[] = "HierarchyName";
static char sWorldNPCTextureBundleFileKey[] = "TextureBundleFile";
static char sWorldNPCModelFileKey[] = "ModelFile";

void WorldNPCManager::AddTemplate(
    TweakNode* entry, const char* name)
{
    const char* hierarchyFile = 0;
    const char* hierarchyName = 0;
    const char* animationFile = 0;
    const char* modelFile = 0;
    const char* textureBundleFile = 0;

    for (TweakNode* child
         = ((TweakEntry*)entry)->m_ChildHead;
         child != 0; child = child->m_Next)
    {
        if (nlStrICmp(GetTweakNodeName(child), sWorldNPCAnimationFileKey) == 0)
        {
            animationFile
                = static_cast<TweakValueString*>(child->m_Value)->m_Value;
        }
        if (nlStrICmp(GetTweakNodeName(child), sWorldNPCHierarchyFileKey) == 0)
        {
            hierarchyFile
                = static_cast<TweakValueString*>(child->m_Value)->m_Value;
        }
        if (nlStrICmp(GetTweakNodeName(child), sWorldNPCHierarchyNameKey) == 0)
        {
            hierarchyName
                = static_cast<TweakValueString*>(child->m_Value)->m_Value;
        }
        if (nlStrICmp(GetTweakNodeName(child), sWorldNPCTextureBundleFileKey) == 0)
        {
            textureBundleFile
                = static_cast<TweakValueString*>(child->m_Value)->m_Value;
        }
        if (nlStrICmp(GetTweakNodeName(child), sWorldNPCModelFileKey) == 0)
        {
            modelFile
                = static_cast<TweakValueString*>(child->m_Value)->m_Value;
        }
    }

    if (hierarchyFile != 0 && hierarchyName != 0 && animationFile != 0
        && modelFile != 0 && textureBundleFile != 0)
    {
        CrowdCharacterDefinition& definition
            = mTemplates[mNumTemplates];
        definition.mName = name;
        definition.mAnimationFile = animationFile;
        definition.mHierarchyFile = hierarchyFile;
        definition.mHierarchyName = hierarchyName;
        definition.mTextureBundleFile = textureBundleFile;
        definition.mModelFile = modelFile;
        mSelectedTemplates[mNumTemplates] = false;
        ++mNumTemplates;
    }
}

inline void WorldNPCManager::CollectSelectedTemplates()
{
    for (int i = 0; i < mNumTemplates; ++i)
    {
        if (mSelectedTemplates[i])
        {
            mLoadTemplates[mNumLoadTemplates] = mTemplates[i];
            ++mNumLoadTemplates;
        }
    }
}

void WorldNPCManager::BeginModelLoading()
{
    CollectSelectedTemplates();
    mModelCollection->Initialize(mLoadTemplates, mNumLoadTemplates);
    if (mModelCollection->HasMoreModels())
    {
        mModelCollection->BeginNextModelLoad();
    }
}

bool WorldNPCManager::UpdateModelLoading()
{
    if (mNumLoadTemplates == 0)
    {
        return true;
    }
    if (mModelCollection->UpdateModelLoad())
    {
        mModelCollection->CreateLoadedModel();
        mLoadedModels[mNumLoadedModels] = mModelCollection->mModels[mNumLoadedModels];
        ++mNumLoadedModels;

        if (mModelCollection->HasMoreModels())
        {
            mModelCollection->BeginNextModelLoad();
            return false;
        }

        mModelsLoaded = true;
        for (ListEntry<WorldNPC*>* entry = mPendingWorldNPCs.m_Head;
            entry != 0; entry = entry->next)
        {
            WorldNPC* npc = entry->entry;
            nlMatrix4 transform = npc->mTransform;
            CreateNPC(npc->mTemplateHash, transform);
        }
        return true;
    }
    return false;
}

ImpostorModel* WorldNPCManager::CreateNPC(
    unsigned long templateHash, const nlMatrix4& transform)
{
    TemplateLookupResult lookup;
    FindLoadedTemplate(*this, templateHash, lookup);
    ImpostorModel* source
        = lookup.found ? mLoadedModels[lookup.index] : 0;
    ImpostorModel* model
        = source->Clone(glGetCurrentResourcePool());
    if (mModelCallback != 0)
    {
        model->mModelCallback = GetModelCallback();
    }

    model->PlayAnimation("idle", 0.0f, PM_CYCLIC);
    model->SetAnimationTime(nlRandomf(0.0f, 1.0f, &nlDefaultSeed));
    model->mWorldMatrix = transform;
    mWorldNPCs.AddEnd(model);
    return model;
}

namespace
{
// MSC_NPC_EVAL_LAZY: the models whose evaluate Update left out on this picture (the render filter hid them)
const ImpostorModel* sSkippedEval[128];
int sNumSkippedEval;
void NoteSkippedEvaluate(const ImpostorModel* model)
{
    if (sNumSkippedEval < 128) sSkippedEval[sNumSkippedEval++] = model;
}
bool WasSkippedEvaluate(const ImpostorModel* model)
{
    for (int i = 0; i < sNumSkippedEval; ++i)
    {
        if (sSkippedEval[i] == model) return true;
    }
    return false;
}

struct NpcPoseJob
{
    ImpostorModel* models[128];
    bool evaluate[128];
    int count;
};
int NpcPoseRange(void* opaque, size_t begin, size_t end, uint32_t lane)
{
    NpcPoseJob& job = *static_cast<NpcPoseJob*>(opaque);
    (void)lane;
    for (size_t i = begin; i < end; ++i)
    {
        if (NpPoseJobsBreak() && (i % 3) == 2) continue;   // (proof mutant)
        job.models[i]->PoseForJob(job.evaluate[i]);
    }
    return 1;
}
} // namespace

void WorldNPCManager::Render(GLView* view)
{
    if (gDisableWorldNPCs)
    {
        return;
    }

    // wave 12, MSC_POSE_JOBS bit 2: the pose of the drawn models (node matrices, skin pose) is made on the job lanes first, then the loop below only builds the packets. A pose is the same function of the same inputs wherever
    // it runs. MSC_NPC_EVAL_LAZY: a model that Update did not evaluate (its filter hid it then) and that the filter lets through now is evaluated here.
    bool posed = false;
    NpcPoseJob job;
    job.count = 0;
    if ((NpPoseJobs() & 2) != 0 && PortParallelWorkers() != 0)
    {
        bool fits = true;
        for (ListEntry<ImpostorModel*>* e = mWorldNPCs.m_Head; e != 0; e = e->next)
        {
            ImpostorModel* model = e->entry;
            if (mRenderFilter != 0 && !mRenderFilter(model)) continue;
            if (job.count == 128 || model->mPoseAccumulator == 0)
            {
                fits = false;
                break;
            }
            job.evaluate[job.count] = sNumSkippedEval != 0 && WasSkippedEvaluate(model);
            job.models[job.count++] = model;
        }
        if (fits && job.count != 0)
        {
            PortParallelFor((size_t)job.count, 1, NpcPoseRange, &job);
            for (int i = 0; i < job.count; ++i) ImpostorModel::MarkPosed(job.models[i]);
            NpPoseJobsCount(2, job.count);
            NpPoseJobsCount(3, 1);
            posed = true;
        }
    }

    ListEntry<ImpostorModel*>* entry = mWorldNPCs.m_Head;
    while (entry != 0)
    {
        ImpostorModel* model = entry->entry;
        if (mRenderFilter != 0 && !mRenderFilter(model))
        {
            entry = entry->next;
            continue;
        }
        if (posed && ImpostorModel::IsPosed(model))
        {
            model->RenderSamePose(view);   // (Render(view, 0) with the node matrices and skin pose already made)
        }
        else
        {
            if (sNumSkippedEval != 0 && WasSkippedEvaluate(model))
            {
                model->EvaluatePose();
                NpPoseJobsCount(5, 1);
            }
            model->Render(view, 0);
        }
        entry = entry->next;
    }
    if (posed) ImpostorModel::ClearPosed();
}

void WorldNPCManager::Update(float dt)
{
    // MSC_NPC_LAZY (np/overlap.h): Stadium Crowd HIDE leaves out the one Render of these models (GameRenderTask::RenderFrame), the only reader of their pose (MSC_STADIUM_HEAVY does the same);
    // the animation controllers, whose events and time are not visual only, go on
    const bool lazy = (NpPerformanceMode() || NpStadiumHeavyCrowd() || NpStadiumCutHas(NP_CUT_WORLDNPC)) && NpNpcLazy();   // (nxvis25: MSC_STADIUM_CUT bit 16 leaves out the same one Render, GameRenderTask NpWin17Stage 0)
    sNumSkippedEval = 0;   // (MSC_NPC_EVAL_LAZY: this picture's skips)
    for (ListEntry<ImpostorModel*>* entry = mWorldNPCs.m_Head;
        entry != 0; entry = entry->next)
    {
        ImpostorModel* model = entry->entry;
        model->mAnimController->Update(dt);
        if (!lazy && NpNpcEvalLazy() && mRenderFilter != 0 && !mRenderFilter(model))
        {
            // MSC_NPC_EVAL_LAZY: the render filter hides this model, so nothing reads its pose; if the filter lets it through at the draw (the camera type changed in between) the draw evaluates it
            NpPoseJobsCount(4, 1);
            NoteSkippedEvaluate(model);
        }
        else if (!lazy)
        {
            model->EvaluatePose();
        }
    }
}

void WorldNPC::Initialize(WorldObjectLoadContext*)
{
    WorldNPCManager* manager = gpWorldNPCManager;
    manager->RegisterObject(this);
}

void WorldNPC::ReleaseResources()
{
}

template void ListContainerBase<WorldNPC*,
    NewAdapter<ListEntry<WorldNPC*> > >::DeleteEntry(ListEntry<WorldNPC*>*);

inline void WorldNPCModelList::DeleteModelEntry(
    ListEntry<ImpostorModel*>* entry)
{
    delete entry->entry;
    ListContainerBase<ImpostorModel*,
        NewAdapter<ListEntry<ImpostorModel*> > >::DeleteEntry(entry);
}

WorldNPC::~WorldNPC()
{
}

nlMatrix4* WorldNPC::GetWorldMatrix()
{
    return &mTransform;
}

void WorldNPC::SetWorldMatrix(const nlMatrix4& transform)
{
    mTransform = transform;
}
