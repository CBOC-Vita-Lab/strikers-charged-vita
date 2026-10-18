#include <revolution/gx/GXPixel.h>
#include <revolution/gx/GXFrameBuf.h>
#include <revolution/vi/vi_fwd.h>
#include <revolution/gx/GXMisc_fwd.h>
#include <revolution/os/OSError_fwd.h>
#include <revolution/gx/GXTransform.h>

#include "Game/TweakQuery.h"
#include "NL/nlDebug.h"
#include "NL/gl/glPlat.h"
#include "Game/SharedStaticStorage.h"
#include "NL/gl/glMaterialProgram.h"
#include "Game/Sys/debug.h"
#include "NL/gl/glView.h"
#include "NL/gl/glRenderList.h"
#include "NL/gl/glDrawSyncLog.h"

#include "NL/glx/glxGX.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/glxMemory.h"
#include "NL/glx/glxSend.h"
#include "NL/glx/glxSwap.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlFunction.h"
#include "NL/nlMath.h"
#include "NL/nlMemory.h"
#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "np/framesend.h"
#include "np/fps.h"
#include "Game/Effects/EmissionManager.h"
#include "Game/GameObjectLighting.h"
#include "Game/HBMManager.h"
#include "Game/Render/RLView.h"
#include "NL/nlTask.h"
#include "Game/Camera/CameraMan.h"
#include "Game/Render/StadiumLoading.h"
#endif

extern "C"
{
    void DCFlushRange(void* address, u32 length);
    void GXSetMisc(s32 token, s32 value);
    void* GXInit(void* fifo, u32 size);

    void SCInit();
    u32 SCCheckStatus();
    u8 SCGetProgressiveMode();
    u8 SCGetEuRgb60Mode();

    void GXInitFifoLimits(void* fifo, u32 highWatermark, u32 lowWatermark);
}

static GXRenderModeObj glPal480IntDf = {
    4,
    640,
    480,
    542,
    40,
    16,
    640,
    542,
    1,
    0,
    0,
    { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 },
    { 8, 8, 10, 12, 10, 8, 8 }
};

GXRenderModeObj glx_rmode;
static PlatformViewport glx_viewport;

static f32 glx_CopyDispScaleFactor = 1.0f;
static u32 glx_TargetFPS = 60;
static s32 glx_VIWidth = 680;
static bool glx_Widescreen = true;
static u32 glx_FIFOSize = 0x80000;
static u32 glx_ClearPixel = 0x10801080;

static s32 glx_VideoMode;
static s32 prev_VIWidth = glx_VIWidth;
static void* glx_FIFOMem;
static void* glx_FIFO;
static void* glx_FrameBuffer[2];
static s32 glx_FBSize;

u32 glplatGetFrameBufferWidth()
{
    return glx_rmode.fbWidth;
}

u32 glplatGetFrameBufferHeight()
{
    return glx_rmode.efbHeight;
}

s32 glx_GetVideoMode()
{
    return glx_VideoMode;
}

u32 glx_GetScaledXFBWidth()
{
    return glx_VIWidth;
}

void glx_ClearXFB(void* xfb)
{
    for (int i = 0; i < glx_FBSize; i += 4)
    {
        *(u32*)((u32)xfb + i) = glx_ClearPixel;
    }
    DCFlushRange(xfb, glx_FBSize);
}

static void glx_InitGX()
{
    gxInit();
    GXSetMisc(1, 8);
    GXRenderModeObj& mode = glx_rmode;
    GXSetViewport(0.0f, 0.0f, (f32)mode.fbWidth, (f32)mode.efbHeight, 0.0f, 1.0f);
    GXSetScissor(0, 0, mode.fbWidth, mode.efbHeight);
    GXSetDispCopySrc(0, 0, mode.fbWidth, mode.efbHeight);
    GXSetDispCopyDst(mode.fbWidth, mode.xfbHeight);
    GXSetDispCopyYScale(glx_CopyDispScaleFactor);
    GXSetCopyFilter(mode.aa, mode.sample_pattern, true, mode.vfilter);
    GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    gxSetDither(true);
    gxSetColourUpdate(true);
    gxSetAlphaUpdate(true);
    GXSetDispCopyGamma(GX_GM_1_0);

    for (int stage = 0; stage < 16; stage++)
    {
        gxSetTevColourOp(stage, 0, 0, 0, true, 0);
        gxSetTevAlphaOp(stage, 0, 0, 0, true, 0);
    }
    GXFlush();
}

bool glplatPreStartup()
{
    return true;
}

static bool InitializeMaterialProgram(unsigned long&, void*& program)
{
    ((GLMaterialProgram*)program)->Initialize();
    return true;
}

void glplatInitializeMaterialPrograms()
{
    MaterialProgramCallback callback(InitializeMaterialProgram);
    glForEachMaterialProgram(&callback);
}

bool glplatStartup(gl_ScreenInfo* screenInfo)
{
    SCInit();
    while (SCCheckStatus() == 1)
    {
    }

    if (TweakExists("/user/gpu fifo"))
    {
        glx_FIFOSize = (u32)GetTweakInt("/user/gpu fifo", 0) << 10;
    }

    screenInfo->ScreenWidth = 640;
    screenInfo->ScreenHeight = 448;
    screenInfo->ColourDepth[0] = 6;
    screenInfo->ColourDepth[1] = 6;
    screenInfo->ColourDepth[2] = 6;
    screenInfo->ColourDepth[3] = 6;
    screenInfo->ZDepth = 24;
    screenInfo->StencilDepth = 0;
    screenInfo->PixelCentre = 0.0f;
    screenInfo->FSAA = false;
    glx_CopyDispScaleFactor = 1.0f;

    GXRenderModeObj* renderMode;
    u32 tvFormat = VIGetTvFormat();
    switch (tvFormat)
    {
    case 0:
        glx_VideoMode = 0;
        renderMode = &GXNtsc480IntDf;
        break;
    case 1:
        glx_VideoMode = 1;
        renderMode = &glPal480IntDf;
        break;
    case 2:
        glx_VideoMode = 2;
        renderMode = &GXMpal480IntDf;
        break;
    case 5:
        glx_VideoMode = 4;
        renderMode = &GXEurgb60Hz480IntDf;
        break;
    default:
        nlBreak();
        break;
    }

    tvFormat = VIGetTvFormat();
    if (SCGetProgressiveMode() == 1 && VIGetDTVStatus() == 1)
    {
        if (glx_VideoMode == 0)
        {
            renderMode = glx_Widescreen ? &GXNtsc480ProgSoft : &GXNtsc480Prog;
            OSReport("Setting Progressive NTSC Mode\n");
        }
        if (glx_VideoMode == 1 || tvFormat == 5)
        {
            renderMode = glx_Widescreen ? &GXEurgb60Hz480ProgSoft : &GXEurgb60Hz480Prog;
            OSReport("Setting Progressive EURGB60 Mode\n");
        }
    }
    else if (tvFormat == 5 || SCGetEuRgb60Mode() == 1)
    {
        renderMode = &GXEurgb60Hz480IntDf;
        OSReport("Setting Interlaced EURGB60 Mode\n");
    }

    GXAdjustForOverscan(renderMode, &glx_rmode, 0, 16);
    if (glx_VideoMode == 1)
    {
        glx_rmode.efbHeight = 448;
        glx_CopyDispScaleFactor = GXGetYScaleFactor(448, glx_rmode.xfbHeight);
        glx_TargetFPS = 50;
    }
    else
    {
        glx_TargetFPS = 60;
        glx_CopyDispScaleFactor = 1.0f;
    }

    glx_rmode.viWidth = (u16)glx_VIWidth;
    glx_rmode.viXOrigin = (u16)((720 - glx_VIWidth) / 2);
    VIConfigure(&glx_rmode);
    VIFlush();
    VIConfigure(&glx_rmode);

    glx_FIFOMem = nlAllocateAlignedMemory(glx_FIFOSize, 0);
    if (glx_FIFOMem == 0)
    {
        return false;
    }
    glx_FIFO = GXInit(glx_FIFOMem, glx_FIFOSize);
    GXInitFifoLimits(glx_FIFO, glx_FIFOSize - 0x10000, glx_FIFOSize - 0x40000);

    u32 fbSize = ((glx_rmode.fbWidth + 15) & 0xFFF0) * glx_rmode.xfbHeight * 2;
    if (fbSize < 0x9F600)
    {
        fbSize = 0x9F600;
    }
    u32 totalSize = fbSize * 2;
    void* framebufferMemory = nlMalloc(totalSize, 32, false);
    glx_FrameBuffer[0] = framebufferMemory;
    glx_FrameBuffer[1] = (u8*)framebufferMemory + fbSize;
    glx_FBSize = fbSize;
    glx_ClearXFB(framebufferMemory);
    glx_ClearXFB(glx_FrameBuffer[1]);

    tDebugPrintManager::Print(DC_GL, "%uKB used for FB and FIFO\n", totalSize >> 10, glx_FIFOSize >> 10);
    glx_InitGX();
    VISetNextFrameBuffer(glx_FrameBuffer[0]);
    glxSwapSetBlack(true);
    VIFlush();
    VIWaitForRetrace();
    if ((glx_rmode.tvInfo & 1) != 0)
    {
        VIWaitForRetrace();
    }
    glxInitSwap(glx_FrameBuffer[0], glx_FrameBuffer[1]);
    glxInitTex();
    glxInitTargets();
    return true;
}

bool glplatPostStartup()
{
    return true;
}

void glplatBeginFrame()
{
    if (glx_VIWidth != prev_VIWidth)
    {
        const s32 widthDifference = 720 - glx_VIWidth;
        prev_VIWidth = glx_VIWidth;
        glx_rmode.viWidth = (u16)glx_VIWidth;
        glx_rmode.viXOrigin = (u16)(widthDifference / 2);
        VIConfigure(&glx_rmode);
        VIFlush();
    }
}

void glplatEndFrame()
{
}

PlatformViewport* glplatGetViewport()
{
    return &glx_viewport;
}

// The prologue and the epilogue of a frame's send, and the send of one view: shared by the plain path (glx_SendViews walks the live
// views) and the frame-send job (NP_NATIVE: glx_SendViewsSnapshot walks shadows of the views as they were at the fork).
static void glx_SendBegin()
{
    glx_viewport.x = 0;
    glx_viewport.y = 0;
    glx_viewport.width = 640;
    glx_viewport.height = 448;
    GXSetViewport((f32)glx_viewport.x,
        (f32)glx_viewport.y,
        (f32)glx_viewport.width,
        (f32)glx_viewport.height,
        0.0f,
        1.0f);
    GXSetScissor(0, 0, 640, 448);
}

static void glx_SendView(GLView* view)
{
    GLXTarget* target;
    if (view->m_Viewport.width != 0 && view->m_Viewport.height != 0)
    {
        GLRenderPair renderPair = view->GetRenderPair();
        target = renderPair.target;
        target->Activate(0);

        glx_viewport.x = view->m_Viewport.x;
        glx_viewport.y = view->m_Viewport.y;
        glx_viewport.width = view->m_Viewport.width;
        glx_viewport.height = view->m_Viewport.height;
        const s32 viewportHeight = view->m_Viewport.height;
        const s32 viewportWidth = view->m_Viewport.width;
        const s32 viewportY = view->m_Viewport.y;
        const s32 viewportX = view->m_Viewport.x;
        GXSetViewport((f32)viewportX, (f32)viewportY, (f32)viewportWidth, (f32)viewportHeight, 0.0f, 1.0f);
        GXSetScissor(viewportX, viewportY, viewportWidth, viewportHeight);
        glGetDrawSyncLog()->SetCurrentView(view->m_Name);

        if (view->m_ClearDepth || view->m_ClearColour || view->m_Unknown32)
        {
            bool hasRenderTarget = false;
            if (renderPair.hash != 0)
            {
                if (target != 0)
                {
                    hasRenderTarget = true;
                }
            }
            if (hasRenderTarget)
            {
                target->ClearBuffers(view->m_ClearColour, view->m_ClearDepth, view->m_Unknown32);
            }
        }

        if (view->m_Visible)
        {
            view->Iterate(glx_SendFrame_cb);
            const s32 targetMode = view->m_Target;
            if (targetMode != 8)
            {
                if (targetMode != 9)
                {
                    if (targetMode != 10)
                    {
                        return;
                    }
                }
            }
            glplatCopyTargetToTexture(target, targetMode != 8, targetMode == 10);
        }
    }
}

static void glx_SendEndFrame()
{
    glGetDrawSyncLog()->EndFrame();
    glx_SendEnd();
}

static void glx_SendViews()
{
#ifdef NP_NATIVE
    GLRefreshPacketSnapshots();
#endif
    glx_SendBegin();

    GLViewIterator iterator(&gRootView);
    for (; !iterator.IsDone(); iterator.Next())
    {
        glx_SendView(iterator.Current());
    }

    glx_SendEndFrame();
}

#ifdef NP_NATIVE
// ---------------------------------------------------------------------------------------------------------------------------
// The frame send as a job on a second core (platform/vita/src/np_framesend.cpp; np/docs/wave3-NPP2.md).
//
// At the fork (the game thread, right after glxSwapPost, where the serial order starts glx_SendViews) every view that the send would
// visit is copied: the object itself (flags, viewport, render pair, name, fog / co-planar flags of an RLView, and the vtable pointer, so
// BeginRender / BeginPacket dispatch as before) and the view and projection matrices of its camera interface, which the job reads through a
// GLViewInterface of its own. The job then sends the SHADOWS: what the game thread does to the live views and cameras from here on
// (BeginFrameTask rewrites the cameras and hides layers; the simulation does not touch them) cannot change the frame that was
// already finished. This is the recompiled build's "copy cameras and view flags at the fork" (split/NOTES.md, the conflict list).
// What the shadows still share with the game thread is the packet trees (GLView::m_Sorters: attaching is gated, glView.cpp), the
// frame allocator (gated, glxMemory.cpp), the render targets and the models and textures the packets point to.
// ---------------------------------------------------------------------------------------------------------------------------
int glGetCurrentFrame();
namespace
{
class SnapshotInterface : public GLViewInterface
{
public:
    virtual void GetViewMatrix(nlMatrix4& m) const { m = mView; }
    virtual void GetProjectionMatrix(nlMatrix4& m) const { m = mProjection; }
    // (nothing on the send path asks for these two: computed from the snapshot, not captured, so that the fork does not run the
    // lazily updated caches of the live interfaces)
    virtual void GetInverseViewMatrix(nlMatrix4& m) const { nlInvertMatrix(m, mView); }
    virtual void GetViewProjectionMatrix(nlMatrix4& m) const { nlMultMatrices(m, mView, mProjection); }
    virtual const nlMatrix4* GetViewMatrix() const { return &mView; }
    virtual const nlMatrix4* GetProjectionMatrix() const { return &mProjection; }

    nlMatrix4 mView;
    nlMatrix4 mProjection;
};

struct ViewShadow
{
    GLView* live;
    SnapshotInterface iface;
    // the byte copy of the view object (an RLView, or a plain GLView for the root and any other direct instance)
    alignas(8) unsigned char object[sizeof(RLView)];
    GLView* View() { return reinterpret_cast<GLView*>(object); }
};

// The shadows of one frame, in the order of the view tree. A shadow stands for a live view through GLView::m_Unknown38 (a field nothing else
// uses): GLViewIdentity() gives the live view's address, which is what the game compares across frames (LoadGameObjectLights keeps the last
// view it loaded lights for). The shadow objects are made on demand (malloc, not the game heap) and reused from frame to frame.
struct SendSnapshot
{
    int count;
    int capacity;
    ViewShadow** items;
    // W5 PIPE (two-stage pipeline): the packet trees the live views handed over at this fork; the job clears them when it is done
    GLPacketSorterTree** trees;
    int treeCount;
    int treeCapacity;
    bool pipe;
};

SendSnapshot gSendSnapshot;
nlVector3 gSendCameraPosition;   // the position of the current camera at the fork (the camera overlay programs scroll by it)
bool gSendSnapshotHooked;
unsigned long long gSendForks;
unsigned long gSendSnapFailsSeen;      // GLSnapshotFailures() at the last fork
unsigned long long gSendSerialised;    // frames joined at once after a snapshot that did not fit
int gSendMaxViews;
unsigned long long gSendNotForked[4];   // frames of an enabled mode that were sent the plain way: state not forked, indicator / capture, HBM, border

void* RLViewVtable()
{
    static void* vtable = 0;
    if (vtable == 0)
    {
        RLView* any = GetLayerView(eCLV_Characters);
        if (any != 0)
        {
            vtable = *reinterpret_cast<void**>(any);
        }
    }
    return vtable;
}

ViewShadow* NextShadow(SendSnapshot& snap)
{
    if (snap.count == snap.capacity)
    {
        const int capacity = snap.capacity == 0 ? 128 : snap.capacity * 2;
        ViewShadow** items = static_cast<ViewShadow**>(realloc(snap.items, capacity * sizeof(ViewShadow*)));
        if (items == 0)
        {
            nlBreak();
        }
        snap.items = items;
        snap.capacity = capacity;
        for (int i = snap.count; i < capacity; ++i)
        {
            snap.items[i] = 0;
        }
    }
    if (snap.items[snap.count] == 0)
    {
        void* memory = malloc(sizeof(ViewShadow));
        if (memory == 0)
        {
            nlBreak();
        }
        // constructed (the SnapshotInterface inside has a vtable pointer: raw malloc memory would give the job a garbage one)
        snap.items[snap.count] = new (memory) ViewShadow;
    }
    return snap.items[snap.count++];
}

void AddTree(SendSnapshot& snap, GLPacketSorterTree* tree)
{
    if (snap.treeCount == snap.treeCapacity)
    {
        const int capacity = snap.treeCapacity == 0 ? 256 : snap.treeCapacity * 2;
        GLPacketSorterTree** trees = static_cast<GLPacketSorterTree**>(realloc(snap.trees, capacity * sizeof(GLPacketSorterTree*)));
        if (trees == 0)
        {
            nlBreak();
        }
        snap.trees = trees;
        snap.treeCapacity = capacity;
    }
    snap.trees[snap.treeCount++] = tree;
}

void SnapshotViews(SendSnapshot& snap, bool pipe)
{
    void* rlVtable = RLViewVtable();
    snap.count = 0;
    snap.treeCount = 0;
    snap.pipe = pipe;
    GLViewIterator iterator(&gRootView);
    for (; !iterator.IsDone(); iterator.Next())
    {
        GLView* live = iterator.Current();
        // the plain path visits only views with a viewport (glx_SendView tests it again on the shadow: the same values)
        if (live->m_Viewport.width == 0 || live->m_Viewport.height == 0)
        {
            if (pipe)
            {
                // (the plain path's gl_ViewReset clears every view's tree, sent or not: this view's packets go to the job to be cleared)
                GLPacketSorterTree* taken = NpViewTakeTree(live);
                if (taken != 0)
                {
                    AddTree(snap, taken);
                }
            }
            continue;
        }
        ViewShadow& shadow = *NextShadow(snap);
        shadow.live = live;
        const size_t bytes = (*reinterpret_cast<void**>(live) == rlVtable) ? sizeof(RLView) : sizeof(GLView);
        memcpy(static_cast<void*>(shadow.object), static_cast<const void*>(live), bytes);
        GLViewInterface* source = live->m_Interface;
        if (source != 0)
        {
            source->GetViewMatrix(shadow.iface.mView);
            source->GetProjectionMatrix(shadow.iface.mProjection);
            shadow.View()->m_Interface = &shadow.iface;
        }   // (a view without an interface keeps none: the plain path never asks a view without packets for its matrices)
        shadow.View()->m_Unknown38 = live;   // GLViewIdentity()
        if (pipe)
        {
            // the live view gets a clean tree for the next frame's packets; the job walks (and then clears) the old one
            GLPacketSorterTree* taken = NpViewTakeTree(live);
            if (taken != 0)
            {
                AddTree(snap, taken);
                shadow.View()->m_Sorters = taken;
            }
            else
            {
                shadow.View()->m_Sorters = NpViewEmptyTree();   // (nothing was attached: the shadow must not share the tree the next frame fills)
            }
        }
    }
    if (snap.count > gSendMaxViews)
    {
        gSendMaxViews = snap.count;
    }
}

// the job: everything glplatSendFrame does after glxSwapPost, over the shadows; ends with gl_ViewReset (the game thread does not
// attach to the views before it has joined)
void SendJob(void* arg)
{
    SendSnapshot* snap = static_cast<SendSnapshot*>(arg);
    static int sTrace = -1;   // MSC_FS_TRACE=1: one line per view before it is sent (where a crash in the job happens)
    if (sTrace < 0)
    {
        const char* e = getenv("MSC_FS_TRACE");
        sTrace = (e != 0 && *e != 0 && *e != '0') ? 1 : 0;
    }
    if (sTrace)
    {
        fprintf(stderr, "[fs] job begin: %d views, frame counter %lu\n", snap->count, (unsigned long)glGetCurrentFrame());
    }
    glx_SendBegin();
    for (int i = 0; i < snap->count; ++i)
    {
        GLView* view = snap->items[i]->View();
        if (sTrace)
        {
            fprintf(stderr, "[fs]   view %d/%d live=%p shadow=%p vtable=%p name=%s viewport=%lu,%lu,%lux%lu interface=%p sorters=%p\n", i, snap->count,
                (void*)snap->items[i]->live, (void*)view, *reinterpret_cast<void**>(view), view->m_Name != 0 ? view->m_Name : "-",
                view->m_Viewport.x, view->m_Viewport.y, view->m_Viewport.width, view->m_Viewport.height, (void*)view->m_Interface, (void*)view->m_Sorters);
        }
        glx_SendView(view);
    }
    if (sTrace)
    {
        fprintf(stderr, "[fs] job views sent\n");
    }
    glx_SendEndFrame();
    glxSwapPreJob();
    if (snap->pipe)
    {
        // the two-stage pipeline: the arena was flipped at the fork (the GX half is left), and the trees are the job's own
        glplatFrameAllocJobEnd();
        for (int i = 0; i < snap->treeCount; ++i)
        {
            NpViewReleaseTree(snap->trees[i]);
        }
        snap->treeCount = 0;
        return;
    }
    glplatFrameAllocNextFrame();
    gl_ViewReset();
}

// after every join, on the game thread: what the job computed for the live views
void SendAfterJoin()
{
    SendSnapshot& snap = gSendSnapshot;
    for (int i = 0; i < snap.count; ++i)
    {
        ViewShadow& shadow = *snap.items[i];
        shadow.live->m_TriangleCount = shadow.View()->m_TriangleCount;
    }
    snap.count = 0;
    GLApplyDrawWriteBacks();   // (wrap flags the job's Draw calls set in packet copies, into the persistent packets: before the deferred frees)
}
} // namespace

extern "C" void NpFramePresentedFromGame(void) __attribute__((weak));

// The camera position the material programs of the send see: on the worker (and in the job in every mode) the one taken at the fork,
// otherwise the live camera's (BeginFrameTask moves it while a send is pending).
const nlVector3& glxCameraPosition()
{
    if (&NpFsOnWorker != 0 && NpFsOnWorker() != 0)
    {
        return gSendCameraPosition;
    }
    return cCameraManager::PeekCamera()->GetCameraPosition();
}

extern "C" void NpGlxFrameStats(void);
extern "C" void NpViewTreeStats(void);
extern "C" void NpGlSendLogStats(void)
{
    NpGlxFrameStats();
    NpViewTreeStats();
    fprintf(stderr, "[fs]   views: up to %d sent per frame, %d shadow objects made\n", gSendMaxViews, gSendSnapshot.capacity);
    fprintf(stderr, "[fs]   frames joined at once because a packet copy did not fit the frame arena: %llu\n", gSendSerialised);
    fprintf(stderr, "[fs]   frames sent as a job: %llu; sent the plain way although the mode is on: state not forked %llu, indicator or capture %llu, HBM %llu, border %llu\n",
            gSendForks, gSendNotForked[0], gSendNotForked[1], gSendNotForked[2], gSendNotForked[3]);
}

// Tries to send the frame as a job. The caller (glSendFrame) has not yet counted the frame; returns false when the frame has to be
// sent the plain way (mode off, a state that is not forked, a debug path that touches the frame after the send).
bool glplatSendFrameForked()
{
    if (&NpFsMode == 0 || &NpFsFork == 0 || NpFsMode() == 0)
    {
        return false;
    }
    nlTaskManager* tasks = nlTaskManager::m_pInstance;
    if (tasks == 0 || (tasks->mCurrentState & (u32)NpFsStates()) == 0)
    {
        ++gSendNotForked[0];
        return false;
    }
    if (glxSwapLoadingIndicator() || glxSwapScreenCaptureWanted())
    {
        ++gSendNotForked[1];
        return false;
    }
    if (gpHBMManager != 0 && gpHBMManager->mActive && gpHBMManager->mReady)   // (EndFrameTask draws the home button menu right after the send)
    {
        ++gSendNotForked[2];
        return false;
    }
    extern unsigned char gDrawScreenBorder;
    if (gDrawScreenBorder)
    {
        ++gSendNotForked[3];
        return false;
    }

    if (!gSendSnapshotHooked)
    {
        gSendSnapshotHooked = true;
        NpFsAfterJoin(SendAfterJoin);
    }
    NpFsJoin();   // (a job from an earlier frame is long joined at the task boundary, or, in the two-stage pipeline, it is this join that ends it)
    glxSwapPost(true);
    const bool pipe = NpFsPipe() != 0;
    SnapshotViews(gSendSnapshot, pipe);
    glxSwapPreFork();
    StadiumTimeAtFork();
    EmissionLightsAtFork();
    GameObjectLightingAtFork();
    glx_FogAtFork();
    if (cCameraManager::PeekCamera() != 0)
    {
        gSendCameraPosition = cCameraManager::PeekCamera()->GetCameraPosition();
    }
    ++gSendForks;
    bool serialise = false;
    if (pipe)
    {
        GLRefreshPacketSnapshots();   // the copies the sorters hold: what the persistent packets hold at the end of the build
        // A packet or skin buffer the arena had no room to copy is sent from the persistent original: such a frame is joined right after
        // the fork, so the next build does not overlap it (a slow frame instead of a race).
        const unsigned long fails = GLSnapshotFailures();
        if (fails != gSendSnapFailsSeen)
        {
            gSendSnapFailsSeen = fails;
            serialise = true;
            ++gSendSerialised;
        }
        glplatFrameAllocForkFlip();   // the next frame builds in the other half of the frame arena
    }
    NpFsFork(SendJob, &gSendSnapshot);
    // The frame hook (nav script, state hash, frame limit) runs here, on the game thread, where the serial order runs it: the
    // script's pad state for the next frame has to be set before the next PlatPadUpdateTask. A frame limit that ends the run
    // drains the job first (np_harness.cpp Finish).
    if (&NpFramePresentedFromGame != 0)
    {
        NpFramePresentedFromGame();
    }
    if (serialise)
    {
        NpFsJoin();
    }
    NpFpsLateWaitAfterSend();   // W5 RT (MSC_LATE_WAIT=1, np/fps.h): the 30 fps wait, now that the frame is on its way
    return true;
}
#endif

void glplatSendFrame()
{
    glxSwapPost(true);
    glx_SendViews();
    glxSwapPre(true);
    glplatFrameAllocNextFrame();
#ifdef NP_NATIVE
    NpFpsLateWaitAfterSend();   // W5 RT (MSC_LATE_WAIT=1, np/fps.h): the 30 fps wait, now that the frame is on its way
#endif
}

void glplatAbortFrame()
{
    glplatFrameAllocNextFrame();
    glxSwapWaitDrawDone();
    VIWaitForRetrace();
}

void glplatFinish()
{
    glxSwapWaitDrawDone();
}

u32 glplatGetDefaultTargetWidth()
{
    return 640;
}

u32 glplatGetDefaultTargetHeight()
{
    return 448;
}

u32 glplatGetOrthographicWidth()
{
    return 640;
}

u32 glplatGetOrthographicHeight()
{
    return 480;
}

void glplatViewProjectPoint(GLView* view, const nlVector3& v3world, nlVector3& v3NDC)
{
    nlMatrix4 viewMatrix;
    nlMatrix4 projectionMatrix;
    nlVector3 v_out;

    view->m_Interface->GetViewMatrix(viewMatrix);
    view->m_Interface->GetProjectionMatrix(projectionMatrix);
    nlMultPosVectorMatrix(v_out, v3world, viewMatrix);
    nlMultPosVectorMatrix(v3NDC, v_out, projectionMatrix);

    const f32 wc = 1.0f / -v_out.z;
    v3NDC.x *= wc;
    v3NDC.y = -v3NDC.y * wc;
    v3NDC.z *= wc;
}
