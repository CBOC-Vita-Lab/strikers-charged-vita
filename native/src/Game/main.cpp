#include "Game/Task/GameTaskState.h"
#include "NL/nlDLListContainer.inl"
#include "Game/main.h"
#include "Game/TweakRegistry.h"
#include "Game/TweakConfig.h"

#include "Game/Task/FixedUpdateTask.h"
#include "Game/Audio/AudioGlobals.h"

#include "Game/Task/BeginFrameTask.h"
#include "Game/Task/ComUpdateTask.h"
#include "Game/DB/StadiumInfo.h"
#include "Game/DB/GameProgress.h"
#include "Game/DB/StatsTracker.h"
#include "Game/Debug/FrameCounter.h"
#include "Game/Debug/ShapeRender.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/Effects/ParticleSystem.h"
#ifdef NP_NATIVE
#include "np/fx.h"
#include "np/stadiumcut.h"
#include <stdio.h>
#include "np/fxtight.h"
#endif
#include "Game/GL/GLTexturedColourMeshWriter.h"
#include "Game/Task/FrontEndTask.h"
#include "Game/FE/feMusic.h"
#include "Game/FE/feInput.h"
#include "Game/FE/feResourceManager.h"
#include "Game/FE/feSceneManager.h"
#include "Game/FE/LidOpenMessage.h"
#include "Game/FE/feHelpFuncs_decl.h"
#include "Game/Task/ResetTask.h"
#include "Game/Render/Wiper.h"
#include "Game/Render/ImpostorManager.h"
#include "Game/Render/Presentation.h"
#include "Game/Render/FrontEndPresentation.h"
#include "Game/Render/RenderShadow.h"
#include "Game/Render/RLViewLayers.h"
#include "Game/Render/depthoffield.h"
#include "Game/Render/FlareHandler.h"
#include "Game/Render/Warble.h"
#include "Game/Sys/clock.h"
#include "Game/Sys/debug.h"
#include "Game/GameInfo.h"
#include "Game/GameObjectLighting.h"
#include "Game/AI/AIPad.h"
#include "Game/Physics/Physics.h"
#include "Game/NisPlayer.h"
#include "Game/ReplayManager.h"
#include "Game/ReplayChoreo.h"
#include "Game/ExcitementSystem.h"
#include "Game/Transitions/ModelTransition.h"
#include "Game/Sys/audio.h"
#include "Game/Sys/simpleparser.h"
#include "Game/Task/DispatchEventsTask.h"
#include "Game/Task/EndFrameTask.h"
#include "Game/Task/GameRenderTask.h"
#include "Game/Task/LoadingTask.h"
#include "Game/Task/MovieRenderTask.h"
#include "Game/Task/NetworkUpdateTask.h"
#include "Game/Task/ParticleUpdateCallbacks.h"
#include "Game/Task/ParticleUpdateTask.h"
#include "Game/Task/PlatPadUpdateTask.h"
#include "Game/Task/ProfilerTask.h"
#include "Game/Task/TextWindowTask.h"
#include "Game/Task/TransitionTask.h"
#include "Game/Task/TweakerTask.h"
#include "Game/Task/WorldUpdateTask.h"
#include "Game/TweakValue.h"
#include "Game/PadActions.h"
#include "Game/Pad/FlickDetection.h"

#include "NL/MemAlloc.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/globalpad.h"
#include "NL/plat/WiiPad.h"
#include "NL/nlBind.h"
#include "NL/nlConfig.h"
#include "NL/nlDebug.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "NL/nlFunction.h"
#include "NL/nlFunction.inl"
#include "NL/nlMemory.h"
#include "NL/nlDebugViews.h"
#include "NL/nlMain.h"
#include "NL/nlMath.h"
#include "NL/nlString.h"
#include "NL/nlTask.h"
#include "NL/gl/glState.h"
#include "NL/gl/glShadowedTexturedColourModelWriter.h"
#include "NL/glx/GXShadowedDiffuseMaterialProgram.h"
#include "Game/FE/feDPD.h"
#include "NL/plat/nlFlash.h"
#include "NL/plat/nlFileCache.h"
#include "NL/plat/SwappablePad.h"
#include "NL/glx/glxSwap.h"

#include <revolution/os/OSTime_fwd.h>

#include <string.h>
#include "NL/nlstring_tmpl.h"
#include "NL/gl/glPlat.h"
#include "Game/Audio/RegistryPools.h"
#include "NL/nlDebugFile.h"
#include "NL/nlFileGC.h"
#include <revolution/sc_fwd.h>
#include <revolution/os/OSThread_fwd.h>
#include "Game/SharedStaticStorage.h"

class AudioUpdateTask : public nlTask
{
public:
    virtual const char* GetName() { return "Audio"; }
    virtual void Run(float dt)
    {
        static_cast<GameAudio*>(g_pAudioSystem)->Update(dt);
    }
};

class MemCheckTask : public nlTask
{
public:
    MemCheckTask()
        : mAccumulatedDelta(0)
        , mSampleCount(0)
    {
    }

    virtual void Run(float dt);
    virtual const char* GetName() { return "Mem Check"; }

private:
    s32 mAccumulatedDelta;
    s32 mSampleCount;
}; // size 0x28


volatile int g_Region = GAME_REGION_DEFAULT;

static u32 sCountryCode;
GameAudio* g_pGameAudio;
bool g_e3_Build;
bool lbl_806E1091;
nlLocalization::nlLanguage g_Language;
static s32 sLastVirtualFreeDelta;
static float sAverageVirtualFreeDelta;
static float sVirtualFreeMiB;
static float sVirtualLargestFreeMiB;
static float sStandardFreeMiB;
static float sStandardLargestFreeMiB;
static float sVirtualUsedMiB;
static float sStandardUsedMiB;
static float sSubsystemFreeMiB;
static float sSubsystemUsedMiB;
static float sSubsystemLargestFreeMiB;
static float sStandardM10MiB;
static float sAudioM14MiB;
static float sAudioM10MiB;
int g_BuildNumber;

FrameCounter g_FrameCounter("frame", "send");

static TweakBoolBinding sDisableWriteOutTweak(
    "g_bDisableWriteOut", "/General", &g_bDisableWriteOut, true);
static TweakBoolBinding sMemoryLowWaterMarkCheckingTweak(
    "g_bActivateMemoryLowWaterMarkChecking", gLastTweakCategory,
    &g_bActivateMemoryLowWaterMarkChecking, true);
static TweakBoolBinding sPrintMemoryLowWaterMarksTweak(
    "g_bPrintMemoryNewLowWaterMarks", gLastTweakCategory,
    &g_bPrintMemoryNewLowWaterMarks, true);

static ComUpdateTask comUpdateTask;
static PingerUpdateTask pingerUpdateTask;
static NetworkUpdateTask networkUpdateTask;
static PlatPadUpdateTask platPadUpdateTask;
static FrontEndTask frontEndTask;
static WorldUpdateTask worldUpdateTask;
static GameRenderTask gameRenderTask;
static MovieRenderTask movieRenderTask;
static ParticleUpdateTask particleUpdateTask;
static BeginFrameTask beginFrameTask;
static AudioUpdateTask audioUpdateTask;
static EndFrameTask endFrameTask;
static TweakerTask tweakerTask;
static ProfilerTask profilerTask;
static ResetTask resetTask;
static MemCheckTask memCheckTask;
static TextWindowTask textWindowTask;
static FEDPDTask feDPDTask;
static FlashMemoryTask flashMemoryTask;

static GLMemoryRequirement sGlobalResourceRequirements[3] = {
    { GLM_TextureData, MB(3) },
    { GLM_VertexData, MB(2) },
    { GLM_Header, MB(2) + KB(256) },
};

static TweakValueBool sAllowWarble(
    "sbAllowWarble", "/Rendering/Effects/Warble", true);
static TweakValueBool sRenderWarbleToParticleView(
    "sbRenderWarbleToParticleView", gLastTweakCategory, false);
static TweakValueBool sUseCheckerTextureForWarble(
    "sbUseCheckerTextureForWarble", gLastTweakCategory, false);

static void PreInitFS();
static void Initialize();
static void AddTasks();
bool RenderParticleSystem(ParticleSystem*, GLView*,
    nlDLListSlotPool<Particle*>*, const nlVector3&, const nlVector3&,
    const nlMatrix4*);
void BuildParticleQuads(GLTexturedColourMeshWriter*, ParticleSystem*,
    nlDLListSlotPool<Particle*>*, const nlVector3&, const nlVector3&,
    const nlMatrix4*
#ifdef NP_NATIVE
    , const NpFxTight&
#endif
    );
void BuildParticleQuads(glShadowedTexturedColourModelWriter*, ParticleSystem*,
    nlDLListSlotPool<Particle*>*, const nlVector3&, const nlVector3&,
    const nlMatrix4*
#ifdef NP_NATIVE
    , const NpFxTight&
#endif
    );

int GetRegion()
{
    int region = GAME_REGION_US;
    if (g_Region != GAME_REGION_DEFAULT)
    {
        region = g_Region;
    }
    return region;
}

int GetOnlineRegion()
{
    int region = GAME_REGION_US;
    if (g_Region != GAME_REGION_DEFAULT)
    {
        region = g_Region;
    }
    return region + 1;
}

bool IsAlternateOnlineCountryGroup()
{
    int country;
    if (sCountryCode != 0)
    {
        country = sCountryCode;
    }
    else
    {
        country = SCGetSimpleAddressID();
        country &= 0xFF000000;
        if (country == 0 || country == 0xFF000000)
        {
            country = 0;
        }
        else
        {
            country = (u32)country >> 24;
        }
    }

    if (country == 'A' || country == '_')
    {
        return true;
    }
    return false;
}

void MemCheckTask::Run(float)
{
    static u32 sPreviousVirtualFree;
    static u32 sPreviousTaskState = TASK_PAUSED;

    const float bytesPerMiB = 1048576.0f;
    const u32 virtualAllocationCount = VirtualAllocator.m_allocation_count;

    sLastVirtualFreeDelta = virtualAllocationCount - sPreviousVirtualFree;
    sPreviousVirtualFree = virtualAllocationCount;
    sVirtualFreeMiB = VirtualAllocator.TotalFreeMemory() / bytesPerMiB;
    sVirtualLargestFreeMiB =
        VirtualAllocator.LargestFreeBlock() / bytesPerMiB;
    sStandardFreeMiB = StandardAllocator.TotalFreeMemory() / bytesPerMiB;
    sStandardLargestFreeMiB =
        StandardAllocator.LargestFreeBlock() / bytesPerMiB;

    // AudioBackend::m_AudioAllocator.
    MemoryAllocator* audioAllocator =
        reinterpret_cast<MemoryAllocator*>(
            reinterpret_cast<u8*>(g_pAudioBackend) + 0x434);
    sVirtualUsedMiB = audioAllocator->TotalFreeMemory() / bytesPerMiB;
    sStandardUsedMiB =
        audioAllocator->LargestFreeBlock() / bytesPerMiB;

    sSubsystemFreeMiB = VirtualAllocator.m_14 / bytesPerMiB;
    sSubsystemUsedMiB = VirtualAllocator.m_10 / bytesPerMiB;
    sSubsystemLargestFreeMiB = StandardAllocator.m_14 / bytesPerMiB;
    sStandardM10MiB = StandardAllocator.m_10 / bytesPerMiB;
    sAudioM14MiB = audioAllocator->m_14 / bytesPerMiB;
    sAudioM10MiB = audioAllocator->m_10 / bytesPerMiB;

    if (nlTaskManager::m_pInstance->mCurrentState == TASK_GAMEPLAY &&
        sPreviousTaskState == TASK_GAMEPLAY && !g_bTweaking)
    {
        mAccumulatedDelta += sLastVirtualFreeDelta;
        ++mSampleCount;
        sAverageVirtualFreeDelta =
            static_cast<float>(mAccumulatedDelta / mSampleCount);
    }

    sPreviousTaskState = nlTaskManager::m_pInstance->mCurrentState;
}

static bool sDateTimeLoaded;
static u32 sWarbleTexture;
static char sWarbleTextureCached;

static void PreInitFS()
{
    GLMemoryConfig config;
    config.mFrameMemSize1 = 0x80000;
    config.mFrameMemSize2 = 0x233333;
    config.mResourceRequirements = sGlobalResourceRequirements;
    config.mNumResourceRequirements = 3;
    config.mMaxTextures = 1000;
    if (!glInitMemory(&config))
    {
        nlBreak();
    }
}

void OnSwappablePadChanged(int)
{
    const u32 state = nlTaskManager::m_pInstance->mCurrentState;
    if (state == TASK_FRONTEND || state == TASK_PAUSED)
    {
        EnableAutoPressed();
    }
}

void ConfigureTweakerButtons(int)
{
    cGlobalPad* pad = 0;
    for (int i = 0; i < 4; ++i)
    {
        pad = g_pPadManager->GetPad(i);
        if (pad->IsConnected())
        {
            break;
        }
    }

    const int classID = pad->mBackend->GetClassID();
    const bool justZButton =
        GetTweakBool("/user/Tweaker with just Z Button", false);
    const bool isWiiRemote =
        classID == gWiiRemotePadClassID || classID == gWiiFreestylePadClassID;

    gTweakerAccelButton = 9;
    gTweakerBackButton = 10;
    gTweakerToggleButton = 8;
    gTweakerLeftButton = 11;
    gTweakerRightButton = 12;
    gTweakerUpButton = 13;
    gTweakerDownButton = 14;
    if (!justZButton && !isWiiRemote)
    {
        gTweakerModifierButton = 23;
    }
    else
    {
        gTweakerModifierButton = -1;
    }
}

void ParseBuildNumber(const char* buildInfo)
{
    char* copy;
    const u32 length = nlStrLen(buildInfo) + 1;
    copy = static_cast<char*>(nlMalloc(length, 8, false));
    memcpy(copy, buildInfo, nlStrLen(buildInfo) + 1);

    SimpleParser parser;
    parser.StartParsing(copy, length, " ");
    parser.NextToken(false);
    const char* buildNumber = parser.NextToken(false);
    g_BuildNumber = (int)atof(buildNumber);
    nlFree(copy);
}

void OnDateTimeLoaded(
    void* data, unsigned long size, void* destination)
{
    void* destinationCopy = destination;
    unsigned long sizeCopy = size;
    void* dataCopy = data;
    LoadTweakConfigBuffer(destinationCopy, static_cast<char*>(dataCopy), sizeCopy,
        static_cast<const char*>(destinationCopy));
    sDateTimeLoaded = true;
}

class Config;

void OnCommonConfigLoaded(Config*)
{
}

static void Initialize()
{
#ifndef NP_NATIVE
    // GQR setup follows the platform OSInitFastCast implementation.
    // clang-format off
    asm {
        li r3, 4
        oris r3, r3, 4
        mtspr 0x392, r3
        li r3, 5
        oris r3, r3, 5
        mtspr 0x393, r3
        li r3, 6
        oris r3, r3, 6
        mtspr 0x394, r3
        li r3, 7
        oris r3, r3, 7
        mtspr 0x395, r3
    }
    // clang-format on
#endif // NP_NATIVE: GQR registers do not exist natively

    switch (GetRegion())
    {
    case GAME_REGION_US:
        switch (SCGetLanguage())
        {
        case 3:
            g_Language = nlLocalization::LangNAFrench;
            break;
        case 4:
            g_Language = nlLocalization::LangNASpanish;
            break;
        default:
            g_Language = nlLocalization::LangEnglish;
            break;
        }
        break;
    case GAME_REGION_EU:
        switch (SCGetLanguage())
        {
        case 2:
            g_Language = nlLocalization::LangGerman;
            break;
        case 3:
            g_Language = nlLocalization::LangFrench;
            break;
        case 4:
            g_Language = nlLocalization::LangSpanish;
            break;
        case 5:
            g_Language = nlLocalization::LangItalian;
            break;
        default:
            g_Language = nlLocalization::LangUKEnglish;
            break;
        }
        break;
    case GAME_REGION_JAPAN:
        g_Language = nlLocalization::LangJapanese;
        break;
    }

    nlInit();

    if (!glStartup(PreInitFS))
    {
        nlBreak();
    }

    nlRegHandleDVDMessageCB(Function<void(int)>(DisplayDVDMessageSebring));
    nlRegHandleDVDRetryingCB(Function<void(int)>(DisplayDVDMessageSebring));
    nlRegHandleDVDAllClearCB(Function<void(int)>(DVDAllClearSebring));
    nlRegCheckForResetFromFSCB(Function<FnVoidVoid>(
        Bind<void>(MemFun<ResetTask, void>(&ResetTask::FSCheckForReset),
            &resetTask)));

    DisplayLoadingMessageFast();
    InitializeODEAllocators();
    RegisterUserGeomClasses();
    InitPads();

    unsigned int stringSizes[4];
    stringSizes[1] = 0;
    stringSizes[3] = 0;
    stringSizes[2] = 0x3000;
    stringSizes[0] = 0xA000;
    gResetTweakValueStrings = true;
    InitializeTweakRegistry(0, 1, stringSizes);
    glplatInitializeMaterialPrograms();
    nlSetRandomSeed(OSGetTick(), &nlDefaultSeed);
    if (!glLoadTextureBundle("art/global.rlt", glGetCurrentResourcePool()))
    {
        nlBreak();
    }

    u8 temporaryState = lbl_806E1458;
    ParticleUpdateNoOp(&temporaryState);

    sDateTimeLoaded = false;
    Config::Global().LoadFromFileAsync(
        "ini/common.ini", Function<Config*>(OnCommonConfigLoaded));
    nlLoadEntireFileAsync("ini/datetime.ini", OnDateTimeLoaded,
        const_cast<char*>("/General/Build Info"), 0x20, AllocateStart, 0, 0,
        0);
    while (!sDateTimeLoaded)
    {
        nlServiceFileSystem();
        OSYieldThread();
    }

    EnableAllStadiums();
    const char* buildInfo =
        GetTweakString("/General/Build Info/BuildNumber", 0);
    if (buildInfo != 0)
    {
        ParseBuildNumber(buildInfo);
    }

    glxSetDrawSyncTimeout(1000.0f);
    InitializeDispatchEventsTask();
    nlTaskManager::Startup(0x10000);
    sLoadingTask.Start();
    GetEmissionManager();
    EmissionManager::SetResourceBudget(1, 250);
    EmissionManager::ConfigureResource(3, "Character", 250);
    EmissionManager::ConfigureResource(2, "StadiumEffects", 250);
    ImpostorManager::GetInstance();
    ClockManager::Initialize();
    InstallImageRenderCallback();
    GLResourcePool* resourcePool = glGetCurrentResourcePool();
    resourcePool->MarkResource();
    InitPlatPad();
    g_pGameAudio = new (nlMalloc(sizeof(GameAudio), 8, false))
        GameAudio;
    g_pGameAudio->Initialize();
    gSwappablePadChanged.Add(Function<void(int)>(ConfigureTweakerButtons), 0, -1);
    g_pPadManager->Update(0.0f);
    FEInput::Initialize();
    gSwappablePadChanged.Add(Function<void(int)>(OnSwappablePadChanged), 0, -1);
    FlickDetection::Initialize();
    networkUpdateTask.Initialize();
    StartupAIPads();
    gTransitionTask.Initialize();
    nlLocalization::Initialize();

    if (FEResourceManager::s_pInstance == 0)
    {
        FEResourceManager::s_pInstance = new (8, false) FEResourceManager;
    }
    FEResourceManager::s_pInstance->SetResourcePool(0);
    if (FESceneManager::s_pInstance == 0)
    {
        FESceneManager::s_pInstance = new (8, false) FESceneManager;
    }

    int countryCode;
    if (sCountryCode != 0)
    {
        countryCode = sCountryCode;
    }
    else
    {
        countryCode = SCGetSimpleAddressID();
        countryCode &= 0xFF000000;
        if (countryCode == 0 || countryCode == 0xFF000000)
        {
            countryCode = 0;
        }
        else
        {
            countryCode = (u32)countryCode >> 24;
        }
    }
    tDebugPrintManager::Print(DC_NETWORK, "CountryCode = %d\n", countryCode);

    if (GameInfoManager::s_pInstance == 0)
    {
        GameInfoManager::s_pInstance = new (8, false) GameInfoManager;
    }
    if (CupManager::s_pInstance == 0)
    {
        CupManager::s_pInstance = new (8, false) CupManager;
    }
    if (StatsTracker::s_pInstance == 0)
    {
        StatsTracker::s_pInstance = new (8, false) StatsTracker;
    }
    if (g_pStrikerChallenge == 0)
    {
        g_pStrikerChallenge = new (8, false) StrikerChallenge;
    }

    nlFlashInitialize();
    nlInitFileCache();
    ReplayChoreo::Instance();
    GetPresentation();
    FrontEndPresentation::GetInstance();
    ExcitementSystem::Instance();
    AddTasks();
    SetupViews();
    HideLayerView(eCLV_ScreenBlur);
    HideLayerView(eCLV_ScreenBlur2);
    HideLayerView(eCLV_ShadowVolume);
    HideLayerView(eCLV_ShadowVolumeBlend);
    ParticleSystem::m_Callback = RenderParticleSystem;
    ModeledScreenTransition::s_3DView = GetLayerView(eCLV_Transitions3D);
    SetDebugFontView(GetLayerView(eCLV_Debug));
    SetDebugSquareView(GetLayerView(eCLV_DebugSquare));
    bool widescreen = true;
    if (SCGetAspectRatio() != 1)
    {
        widescreen = false;
    }
    rlSetWidescreen(widescreen);
    g_ShapeRenderer.Initialize(resourcePool);
    g_ShapeRenderer.m_eView = GetLayerView(eCLV_Characters);
    InitMaxProjectedShadows();
    SetCharacterShadowView(GetLayerView(eCLV_Characters));
    FESceneManager::s_pInstance->m_uDefaultRenderView =
        (unsigned long)GetLayerView(eCLV_Anark);
    NisPlayer::Instance();
    ReplayManager::Instance();
    Wiper::Instance().Initialize();
    DepthOfFieldManager::instance.Initialize();
    FlareHandler::instance.Initialize(GetLayerView(eCLV_Particles));
    BeginFrameTask::s_GameplaySkin = eModelSkin_Both;
    BeginFrameTask::s_ReplaySkin = eModelSkin_Blend;
    InitializeParticleUpdateCallbacks();
    Detail::sTempStringAllocatorPool.allocator.pool.PushState();
}

static void AddTasks()
{
    nlTaskManager::AddTask(&resetTask, 0, (u32)-1);
    nlTaskManager::AddTask(&beginFrameTask, 4, (u32)-1);
    nlTaskManager::AddTask(&sLoadingTask, 2, 0x01F80000);
    nlTaskManager::AddTask(gDispatchEventsTask, 0x18, 0xFE07FFFF);
    nlTaskManager::AddTask(&platPadUpdateTask, 5, (u32)-1);
    nlTaskManager::AddTask(GetFixedUpdateTask(), 8, 0xFE07FFDF);
    nlTaskManager::AddTask(&worldUpdateTask, 9, 0x0002001B);
    nlTaskManager::AddTask(&gameRenderTask, 11, 0x0002001B);
    nlTaskManager::AddTask(&movieRenderTask, 11, (u32)-1);
    nlTaskManager::AddTask(&frontEndTask, 13, (u32)-1);
    nlTaskManager::AddTask(&particleUpdateTask, 12, 0x0002001F);
    nlTaskManager::AddTask(&audioUpdateTask, 15, (u32)-1);
    nlTaskManager::AddTask(&endFrameTask, 16, (u32)-1);
    nlTaskManager::AddTask(&gTransitionTask, 1, (u32)-1);
    nlTaskManager::AddTask(&networkUpdateTask, 17, (u32)-1);
    nlTaskManager::AddTask(&feDPDTask, 13, 5);
    nlTaskManager::AddTask(
        &flashMemoryTask, 13, (u32)-1);
    nlTaskManager::AddTask(nlGetFileCache(), 3, (u32)-1);
    nlTaskManager::AddTask(&Wiper::Instance(), 13, (u32)-1);
}

#ifdef NP_NATIVE
// One sprite (np/fx.h, np/fxtight.h): the quad, a polygon on the quad's own texture map, or nothing (one point). t.verts vertices whichever it is: a mesh has one form, and in a
// triangle list what a sprite does not need is written as empty triangles. A corner is given as a point of the texture: its place on the sprite follows from the quad's corners and
// the quad's own numbers on the lattice, so the map from position to texture coordinate is the quad's. Corners are stored with positive area; a quad that is not mirrored runs the
// other way, so they are written backwards, which keeps the front of the sprite for culling.
template <class W>
static inline void NpFxWriteSprite(W* w, const NpFxTight& t, const ParticleReturn& ret, const NpTightPoly* p, bool collapse)
{
    const int n = t.verts;
    if (collapse)
    {
        for (int i = 0; i < n; ++i)   // collapsed to one point: no fragments (np/fx.h)
        {
            w->Texcoord(ret.texcoord[0]);
            w->Colour(ret.c);
            w->Vertex(ret.position[0]);
        }
        return;
    }
    if (!p)
    {
        static const unsigned char kTri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < n; ++i)
        {
            const int c = n == 4 ? i : kTri[i < 6 ? i : 5];
            w->Texcoord(ret.texcoord[c]);
            w->Colour(ret.c);
            w->Vertex(ret.position[c]);
        }
        return;
    }
    const int q0u = (short)(ret.texcoord[0].x * 1024.0f), q0v = (short)(ret.texcoord[0].y * 1024.0f);
    const int du = (short)(ret.texcoord[1].x * 1024.0f) - q0u, dv = (short)(ret.texcoord[3].y * 1024.0f) - q0v;
    const float su = 1.0f / (float)du, sv = 1.0f / (float)dv;
    const nlVector3& p0 = ret.position[0];
    const float e1x = ret.position[1].x - p0.x, e1y = ret.position[1].y - p0.y, e1z = ret.position[1].z - p0.z;
    const float e2x = ret.position[3].x - p0.x, e2y = ret.position[3].y - p0.y, e2z = ret.position[3].z - p0.z;
    const int c = p->n;
    const int order = n == 4 ? (c < 4 ? 4 : c) : 3 * (c - 2);
    for (int i = 0; i < n; ++i)
    {
        int corner;   // (of the fan, then what is left is the last corner again)
        if (i >= order)
            corner = c - 1;
        else if (n == 4)
            corner = i < c ? i : c - 1;
        else
            corner = i % 3 == 0 ? 0 : i / 3 + i % 3;
        if (du < 0)
            corner = corner == 0 ? 0 : c - corner;
        const int u = p->uv[corner][0], v = p->uv[corner][1];
        const float a = (float)(u - q0u) * su, b = (float)(v - q0v) * sv;
        w->Texcoord(u, v);
        w->Colour(ret.c);
        w->Vertex(p0.x + a * e1x + b * e2x, p0.y + a * e1y + b * e2y, p0.z + a * e1z + b * e2z);
    }
}
#endif

bool RenderParticleSystem(ParticleSystem* source, GLView* view,
    nlDLListSlotPool<Particle*>* vertices, const nlVector3& viewRight,
    const nlVector3& viewUp, const nlMatrix4* pCoordSys)
{
    if (!sWarbleTextureCached)
    {
        sWarbleTexture = glGetTexture("effects/fx_warble");
        sWarbleTextureCached = true;
    }

    bool isWarble = sWarbleTexture == source->m_pTemplate->m_hTexture;
#ifdef NP_NATIVE
    static const bool sCutLog3 = getenv("MSC_STADIUM_CUT_LOG") && getenv("MSC_STADIUM_CUT_LOG")[0] == '3';
    static const bool sCutLog1 = getenv("MSC_STADIUM_CUT_LOG") && getenv("MSC_STADIUM_CUT_LOG")[0] == '1';
    if (sCutLog3)   // one frame in 300: every system drawn
    {
        static int calls;
        static int frameCalls;
        if (NpStadiumCutFrameNo() >= 600 && NpStadiumCutFrameNo() < 1600 && NpStadiumCutNewMax((unsigned)source->m_pTemplate->m_uHashID, (int)source->m_NumParticles) && ++calls)
        {
            const EffectsTemplate* t = source->m_pTemplate;
            fprintf(stderr, "[stadiumcut-sys] template 0x%08x blend %d tex %u layer %d particles %d fountain %.1f life %.1f\n", (unsigned)t->m_uHashID, (int)t->m_eBlend, (unsigned)t->m_hTexture, (int)source->m_uLayer, (int)source->m_NumParticles, t->m_fFountainLife, t->m_rParticleLife.base);
            (void)frameCalls;
        }
    }
    if (sCutLog1 && source->m_NumParticles >= 6)   // (MSC_STADIUM_CUT_LOG=1: the templates that carry many particles; once each)
    {
        const EffectsTemplate* t = source->m_pTemplate;
        NpStadiumCutLogTemplate((unsigned)t->m_uHashID, (int)t->m_eBlend, (unsigned)t->m_hTexture, t->mProperties[1]->base * t->mProperties[2]->base, t->m_rParticleLife.base, t->m_fFountainLife, (int)source->m_uLayer, (int)source->m_NumParticles);
    }
    if (NpStadiumCutHas(NP_CUT_AMBIENTFX) && NpStadiumCutFxHashed((unsigned)source->m_pTemplate->m_uHashID)) return true;   // MSC_STADIUM_CUT bit 32
    if (isWarble && NpStadiumCutHas(NP_CUT_WARBLE))   // MSC_STADIUM_CUT bit 1: no heat shimmer, so gWarbleEnabled stays false and the pre-warble copy, offset pass and composite are not made
    {
        NpStadiumCutCount(NP_CUT_WARBLE, 1);
        return true;
    }
    NpFxTight tight;
    NpFxTightFor(&tight, source, view);
    if (isWarble)   // (it draws another texture: quads as ever)
    {
        tight.polys = 0;
        tight.tight = tight.drop = false;
        tight.verts = 4;
        tight.gov = 0;
        tight.offview = 0;   // (MSC_FX_OFFVIEW: it is drawn in the warble layer's own view, not the one the clip was made for)
    }
#endif
    if (IsShadowLookupActive() && !isWarble)
    {
        glShadowedTexturedColourModelWriter writer;
        BuildParticleQuads(&writer, source, vertices, viewRight, viewUp, pCoordSys
#ifdef NP_NATIVE
            , tight
#endif
            );

        GXShadowedDiffuseParameters* parameters =
            static_cast<GXShadowedDiffuseParameters*>(
                writer.GetModel()->packets->materialParameters);
        parameters->receiveShadows = source->m_pTemplate->m_eBlend == EfBlend_Normal;

        if (writer.End())
        {
#ifdef NP_NATIVE
            if (source->m_pTemplate->m_eBlend == EfBlend_Additive && tight.mark)   // FX (np/fx.h): the renderer may draw it at a lower resolution
                NpFxMarkPacket(writer.GetModel()->packets);
#endif
            view->AttachModel(writer.GetModel(), source->m_uLayer);
        }
    }
    else if (sAllowWarble)
    {
        GLTexturedColourMeshWriter writer;
        BuildParticleQuads(&writer, source, vertices, viewRight, viewUp, pCoordSys
#ifdef NP_NATIVE
            , tight
#endif
            );

        if (isWarble && IsWarbleParticleRenderingEnabled())
        {
            u32 texture = glGetTexture(sUseCheckerTextureForWarble
                    ? "global/checkers" : "target/warbletexture");
            glTextureBinding* textureState =
                static_cast<glTextureBinding*>(
                    writer.GetModel()->packets->materialParameters);
            textureState->texture = texture;
            textureState->textureIndex = 0xFFFF;
            textureState->SetWrapS(true);
            textureState->SetWrapT(true);
            textureState->unknown07 = 0;

            view = GetLayerView(sRenderWarbleToParticleView
                    ? eCLV_Particles : eCLV_Warble);
            gWarbleEnabled = true;
        }

        if (writer.End())
        {
#ifdef NP_NATIVE
            if (!isWarble && source->m_pTemplate->m_eBlend == EfBlend_Additive && tight.mark)   // FX (np/fx.h)
                NpFxMarkPacket(writer.GetModel()->packets);
#endif
            view->AttachModel(writer.GetModel(), source->m_uLayer);
        }
    }
    return true;
}

void BuildParticleQuads(GLTexturedColourMeshWriter* writer,
    ParticleSystem* source, nlDLListSlotPool<Particle*>* vertices,
    const nlVector3& viewRight, const nlVector3& viewUp,
    const nlMatrix4* pCoordSys
#ifdef NP_NATIVE
    , const NpFxTight& tight
#endif
    )
{
    ParticleReturn ret;
#ifdef NP_NATIVE
    const bool began = writer->Begin(source->m_NumParticles * tight.verts, tight.verts == 4 ? GLP_QuadList : GLP_TriList, 0);
#else
    const bool began = writer->Begin(source->m_NumParticles * 4, GLP_QuadList, 0);
#endif
    if (began)
    {
#ifdef NP_NATIVE
        const NpFxThin thin = NpFxThinFor(source->m_pTemplate, false);
#endif
        int written = 0;
        nlDLListIterator<Particle*> iterator;
        iterator = vertices->Begin();
        while (iterator.hasNext())
        {
            Particle* pPart = *iterator;
            source->UpdateParticle(&ret, pPart, source->m_pTemplate,
                viewRight, viewUp, pCoordSys);
#ifdef NP_NATIVE
            bool collapse = NpFxThinParticle(thin, pPart, &ret);
            const NpTightPoly* poly = 0;
            if (!collapse)
                poly = NpFxTightParticle(tight, pPart, &ret, &collapse);
            if (tight.offview && NpFxOffViewDrop(tight, ret, collapse))   // MSC_FX_OFFVIEW=1 (np/fx.h): not written; the mesh is made as short
            {
                iterator.Step();
                continue;
            }
            ++written;
            NpFxWriteSprite(writer, tight, ret, poly, collapse);
            iterator.Step();
            continue;
#else
            for (int i = 0; i < 4; ++i)
            {
                writer->Texcoord(ret.texcoord[i]);
                writer->Colour(ret.c);
                writer->Vertex(ret.position[i]);
            }
            iterator.Step();
#endif
        }

#ifdef NP_NATIVE
        if (tight.offview == 1 && written != source->m_NumParticles)
        {
            if (written == 0 && source->m_NumParticles > 0)   // (a mesh that draws nothing keeps one collapsed sprite: the packet, and the runs of effects draws around it, are as they were)
            {
                NpFxWriteSprite(writer, tight, ret, 0, true);
                written = 1;
            }
            NpFxTrimPacket(writer->GetModel()->packets, written * tight.verts);
            writer->count = written * tight.verts;
        }
#endif
        glTextureBinding* textureState =
            static_cast<glTextureBinding*>(
                writer->GetModel()->packets->materialParameters);
        textureState->textureIndex = source->m_uTextureIndex;
        textureState->SetWrapS(false);
        textureState->SetWrapT(false);
        textureState->unknown07 = 0;
    }
}

void BuildParticleQuads(glShadowedTexturedColourModelWriter* writer,
    ParticleSystem* source, nlDLListSlotPool<Particle*>* vertices,
    const nlVector3& viewRight, const nlVector3& viewUp,
    const nlMatrix4* pCoordSys
#ifdef NP_NATIVE
    , const NpFxTight& tight
#endif
    )
{
    ParticleReturn ret;
#ifdef NP_NATIVE
    const bool began = writer->Begin(source->m_NumParticles * tight.verts, tight.verts == 4 ? 3 : 0, 0);
#else
    const bool began = writer->Begin(source->m_NumParticles * 4, 3, 0);
#endif
    if (began)
    {
#ifdef NP_NATIVE
        const NpFxThin thin = NpFxThinFor(source->m_pTemplate, false);
#endif
        int written = 0;
        nlDLListIterator<Particle*> iterator;
        iterator = vertices->Begin();
        while (iterator.hasNext())
        {
            Particle* pPart = *iterator;
            source->UpdateParticle(&ret, pPart, source->m_pTemplate,
                viewRight, viewUp, pCoordSys);
#ifdef NP_NATIVE
            bool collapse = NpFxThinParticle(thin, pPart, &ret);
            const NpTightPoly* poly = 0;
            if (!collapse)
                poly = NpFxTightParticle(tight, pPart, &ret, &collapse);
            if (tight.offview && NpFxOffViewDrop(tight, ret, collapse))   // MSC_FX_OFFVIEW=1 (np/fx.h): not written; the mesh is made as short
            {
                iterator.Step();
                continue;
            }
            ++written;
            NpFxWriteSprite(writer, tight, ret, poly, collapse);
            iterator.Step();
            continue;
#else
            for (int i = 0; i < 4; ++i)
            {
                writer->Texcoord(ret.texcoord[i]);
                writer->Colour(ret.c);
                writer->Vertex(ret.position[i]);
            }
            iterator.Step();
#endif
        }

#ifdef NP_NATIVE
        if (tight.offview == 1 && written != source->m_NumParticles)
        {
            if (written == 0 && source->m_NumParticles > 0)   // (a mesh that draws nothing keeps one collapsed sprite: the packet, and the runs of effects draws around it, are as they were)
            {
                NpFxWriteSprite(writer, tight, ret, 0, true);
                written = 1;
            }
            NpFxTrimPacket(writer->GetModel()->packets, written * tight.verts);
            writer->count = written * tight.verts;
        }
#endif
        glTextureBinding* textureState =
            static_cast<glTextureBinding*>(
                writer->GetModel()->packets->materialParameters);
        textureState->textureIndex = source->m_uTextureIndex;
        textureState->SetWrapS(false);
        textureState->SetWrapT(false);
        textureState->unknown07 = 0;
    }
}

#ifdef NP_NATIVE
void NpMemMarkGameThread();   // NL/nlMemory.cpp: this thread's operator new comes from the game heaps
#endif

int main()
{
#ifdef NP_NATIVE
    NpMemMarkGameThread();
#endif
    Initialize();

    while (nlAsyncReadsPending(0))
    {
        nlServiceFileSystem();
    }

    nlTaskManager::SetNextState(TASK_BOOT_TO_FE);
    FEMusic::SetEnabled(true);

    for (;;)
    {
        nlTaskManager::RunAllTasks();
    }
}
