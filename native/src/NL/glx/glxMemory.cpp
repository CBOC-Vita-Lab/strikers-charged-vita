#include <revolution/gx.h>
#include "Game/Sys/debug.h"
#include <revolution/os.h>

#include "NL/glx/glxMemory.h"

#include "Game/GL/GLInventory.h"
#include "NL/MemAlloc.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/nlDebug.h"
#include "NL/nlMemory.h"
#include "NL/nlPrint.h"
#include "NL/nlString.h"
#include "NL/nlstring_tmpl.h"
#include "Game/SharedStaticStorage.h"
#ifdef NP_NATIVE
#include "np/framesend.h"
#include <stdio.h>
#endif

class GLXResourcePool
    : public GLResourcePool
{
public:
    GLXResourcePool(
        unsigned long sizes[2], const char* name);

    virtual void* Allocate(unsigned long size, eGLMemory memType);
    virtual unsigned long MarkResource();
    virtual void ReleaseResource(unsigned long marker);
    virtual unsigned long GetFreeMemory();
    virtual unsigned long GetTotalMemory();
    virtual unsigned long GetPeakMemoryUsage();
    virtual bool GetPoolMemoryInfo(unsigned long, const char**, unsigned long*,
        unsigned long*, unsigned long*, const char**);
    virtual ~GLXResourcePool();

    char mName[32];
    unsigned long mPoolMemory[2];
    unsigned long mPoolSizes[2];
    unsigned long mUsedMemory[2];
    unsigned long mPeakMemory[2];
}; // size: 0x54

static char sOutOfFrameMemoryFormat[] = "out of frame memory (%s)\n";
static char sFrameMemoryUsedFormat[] =
    "memory used: %uKB frame MEM1, %uKB frame MEM2\n";
static char sFrameMemoryFreeFormat[] =
    "       free: %uKB frame MEM1, %uKB frame MEM2\n";
static char sOutOfResourceMemoryFormat[] =
    "Out of RL resource memory (%s) in pool %s\n";
static char sOutOfMemoryName[] = "OutOfMem";

static char sMEM2Name[] = "MEM2";
static char sMEM1Name[] = "MEM1";

static unsigned char glx_MemoryDump;
static unsigned long FrameMemSizes[2];
static int i_frame;
static bool sFrameMemoryInitialized;
static GLInventory::ModelReleaseCallback sModelReleaseCallback;

static unsigned long p_frame[2][2];
static unsigned long n_frame[2][2];

static inline int RealOrVirtual(eGLMemory memType)
{
    switch (memType)
    {
    case GLM_Header:
    case GLM_Matrix:
    case GLM_IndexData:
    case GLM_Target:
        return 0;
    case GLM_VertexData:
    case GLM_TextureData:
        return 1;
    default:
        nlBreak();
        return -1;
    }
}

bool glxInitMemory(
    unsigned long frameMemSize1, unsigned long frameMemSize2)
{
    MemoryAllocator* allocator = CurrentAllocator;
    CurrentAllocator = &StandardAllocator;
    unsigned long pMem = (unsigned long)nlMalloc(
        frameMemSize1 * 2, 32, false);
    if (pMem == 0)
    {
        return false;
    }

    p_frame[0][0] = pMem;
    p_frame[1][0] = pMem + frameMemSize1;

    CurrentAllocator = &VirtualAllocator;
    pMem = (unsigned long)nlMalloc(frameMemSize2 * 2, 32, false);
    if (pMem == 0)
    {
        return false;
    }

    p_frame[0][1] = pMem;
    p_frame[1][1] = pMem + frameMemSize2;
    CurrentAllocator = allocator;

    i_frame = 0;
    n_frame[1][0] = 0;
    n_frame[0][0] = 0;
    n_frame[1][1] = 0;
    n_frame[0][1] = 0;
    FrameMemSizes[0] = frameMemSize1;
    FrameMemSizes[1] = frameMemSize2;
    sFrameMemoryInitialized = true;
    return true;
}

#ifdef NP_NATIVE
// NP (two-stage pipeline, W5 PIPE): the frame arena is flipped at the FORK, on the game thread: the job goes on reading (and allocating in) the
// arena of the frame just built, the next frame builds in the other one, which the previous job finished with long ago (it was joined before this
// fork). The GX half of glplatFrameAllocNextFrame (the cache invalidations are records in the stream) stays at the end of the job, in stream order.
static int sJobFrame = -1;           // the arena the running job uses; -1 when the job allocates from the live index (no pipeline)
static unsigned long sJobAllocs;     // statistics: frame allocations made by a job
static unsigned long sFlips;

static unsigned long sPeak[2];   // (declared again below with the report)
static unsigned long sArenaGeneration;   // counts the arena resets (the packet snapshot records live in the arena)
unsigned long glplatFrameGeneration() { return sArenaGeneration; }

void glplatFrameAllocForkFlip()
{
    ++sArenaGeneration;
    for (int pool = 0; pool < 2; ++pool)
        if (n_frame[i_frame][pool] > sPeak[pool])
            sPeak[pool] = n_frame[i_frame][pool];
    __atomic_store_n(&sJobFrame, i_frame, __ATOMIC_RELAXED);
    const int newFrame = i_frame ^ 1;
    i_frame = newFrame;
    n_frame[newFrame][0] = 0;
    n_frame[newFrame][1] = 0;
    ++sFlips;
}

void glplatFrameAllocJobEnd()
{
    GXInvalidateVtxCache();
    GXInvalidateTexAll();
    __atomic_store_n(&sJobFrame, -1, __ATOMIC_RELAXED);
}

extern "C" void NpGlSnapStats(void);
extern "C" void NpGlxFrameStats(void)
{
    fprintf(stderr, "[fs]   frame arena: %lu fork flips, %lu frame allocations made by the job; peak use %lu KiB of %lu KiB (header pool), %lu KiB of %lu KiB (data pool)\n",
        sFlips, sJobAllocs, sPeak[0] >> 10, FrameMemSizes[0] >> 10, sPeak[1] >> 10, FrameMemSizes[1] >> 10);
    NpGlSnapStats();
}
#endif

void glplatFrameAllocNextFrame()
{
    if (glx_MemoryDump)
    {
        tDebugPrintManager::Print(DC_GLPLAT, sFrameMemoryUsedFormat,
            n_frame[i_frame][0] >> 10, n_frame[i_frame][1] >> 10);
        tDebugPrintManager::Print(DC_GLPLAT, sFrameMemoryFreeFormat,
            (FrameMemSizes[0] - n_frame[i_frame][0]) >> 10,
            (FrameMemSizes[1] - n_frame[i_frame][1]) >> 10);
        glx_MemoryDump = false;
    }

    int newFrame = i_frame ^ 1;
    i_frame = newFrame;
    n_frame[newFrame][0] = 0;
    n_frame[newFrame][1] = 0;
#ifdef NP_NATIVE
    ++sArenaGeneration;
#endif
    GXInvalidateVtxCache();
    GXInvalidateTexAll();
}

GLXResourcePool::
    GLXResourcePool(
        unsigned long sizes[2], const char* name)
{
    nlStrNCpy(mName, name, 32);

    m_inventory->SetModelReleaseCallback(sModelReleaseCallback);

    MemoryAllocator* allocator = CurrentAllocator;
    CurrentAllocator = &StandardAllocator;
    mPoolMemory[0] = (unsigned long)nlMalloc(sizes[0], 32, false);
    CurrentAllocator = &VirtualAllocator;
    mPoolMemory[1] = (unsigned long)nlMalloc(sizes[1], 32, false);
    CurrentAllocator = allocator;

    mPoolSizes[0] = sizes[0];
    mUsedMemory[0] = 0;
    mPeakMemory[0] = 0;
    mPoolSizes[1] = sizes[1];
    mUsedMemory[1] = 0;
    mPeakMemory[1] = 0;
}

GLXResourcePool::
    ~GLXResourcePool()
{
    for (int i = 0; i < 2; ++i)
    {
        operator delete((void*)mPoolMemory[i]);
        mUsedMemory[i] = 0;
        mPoolSizes[i] = 0;
    }
}

void* GLXResourcePool::Allocate(
    unsigned long allocationSize, eGLMemory memType)
{
    int pool = RealOrVirtual(memType);
    unsigned long aligned = (mPoolMemory[pool] + mUsedMemory[pool] + 31) & ~31u;
    mUsedMemory[pool] = allocationSize + (aligned - mPoolMemory[pool]);
    mPeakMemory[pool] = mUsedMemory[pool] > mPeakMemory[pool]
        ? mUsedMemory[pool]
        : mPeakMemory[pool];

    if (mUsedMemory[pool] > mPoolSizes[pool])
    {
        OSReport(sOutOfResourceMemoryFormat, szMemoryNames[memType], mName);
        glResourceAllocationFailed();
        glDumpResources(sOutOfMemoryName, this);
        nlBreak();
    }
    return (void*)aligned;
}

unsigned long GLXResourcePool::MarkResource()
{
    unsigned long value0 = mUsedMemory[0];
    unsigned long value1 = mUsedMemory[1];
    GLResourceMarker* marker
        = (GLResourceMarker*)glResourceAlloc(
            sizeof(GLResourceMarker), GLM_Header, this);
    marker->mUsedMemory[0] = value0;
    marker->mUsedMemory[1] = value1;
    marker->mLevel = m_level;
    marker->mPool = this;
    m_inventory->ResourceMark();
    m_level++;
    return (unsigned long)marker;
}

void GLXResourcePool::ReleaseResource(
    unsigned long value)
{
    GLResourceMarker* marker
        = (GLResourceMarker*)value;
    int level = marker->mLevel;
    m_inventory->ResourceRelease(level);
    mUsedMemory[0] = marker->mUsedMemory[0];
    mUsedMemory[1] = marker->mUsedMemory[1];
    m_level = level;
}

unsigned long GLXResourcePool::GetFreeMemory()
{
    unsigned long value0 = mPoolSizes[0] - mUsedMemory[0];
    unsigned long value1 = mPoolSizes[1] - mUsedMemory[1];
    return value0 + value1;
}

unsigned long GLXResourcePool::GetTotalMemory()
{
    return mPoolSizes[0] + mPoolSizes[1];
}

unsigned long GLXResourcePool::GetPeakMemoryUsage()
{
    return mPeakMemory[0] + mPeakMemory[1];
}

bool GLXResourcePool::GetPoolMemoryInfo(
    unsigned long arg0, const char** arg1, unsigned long* arg2,
    unsigned long* arg3, unsigned long* arg4, const char** arg5)
{
    if (arg0 > 1)
    {
        return false;
    }
    *arg1 = arg0 != 0 ? sMEM2Name : sMEM1Name;
    *arg2 = mPoolSizes[arg0];
    *arg3 = mPoolSizes[arg0] - mUsedMemory[arg0];
    *arg4 = mPeakMemory[arg0];
    if (arg5 != 0)
    {
        *arg5 = "";
    }
    return true;
}

GLResourcePool* glplatCreateResourcePool(
    const GLMemoryRequirement* requirements,
    unsigned long count, const char* name)
{
    unsigned long sizes[2] = { 0, 0 };
    for (unsigned long i = 0; i < count; ++i)
    {
        int pool = RealOrVirtual(requirements[i].mType);
        sizes[pool] += requirements[i].mSize;
    }

    return new (8, false)
        GLXResourcePool(sizes, name);
}

void* glplatResourceAlloc(
    unsigned long size, eGLMemory memType, void* resource)
{
    return ((GLResourcePool*)resource)->Allocate(
        size, memType);
}

void* glplatFrameAlloc(unsigned long size, eGLMemory memType)
{
    // NP (frame-send worker): the game thread allocating from the frame arena while a send is pending would race the job's own
    // allocations (skinned vertices) and the arena flip at its end: join first. (On the worker this is a call that returns.)
    NpFsGateNotPipe("framealloc");   // (the pipeline double buffers the arena at the fork: the game thread builds in the other half)
    int frame = i_frame;
    int jobFrame;
    if (&NpFsOnWorker != 0 && NpFsOnWorker() != 0 && (jobFrame = __atomic_load_n(&sJobFrame, __ATOMIC_RELAXED)) >= 0)   // (the worker's variable: the game thread does not read it)
    {
        frame = jobFrame;
        ++sJobAllocs;
    }
    unsigned long isLow = RealOrVirtual(memType);
    unsigned long newTop
        = (p_frame[frame][isLow] + n_frame[frame][isLow] + 31)
        & ~31u;
    unsigned long newSize = size + (newTop - p_frame[frame][isLow]);
    if (newSize > FrameMemSizes[isLow])
    {
        OSReport(sOutOfFrameMemoryFormat, szMemoryNames[memType]);
        nlBreak();
        return 0;
    }
    n_frame[frame][isLow] = newSize;
    return (void*)newTop;
}

#ifdef NP_NATIVE
void* glplatFrameAllocTry(unsigned long size, eGLMemory memType)
{
    NpFsGateNotPipe("framealloc");
    int frame = i_frame;
    int jobFrame;
    if (&NpFsOnWorker != 0 && NpFsOnWorker() != 0 && (jobFrame = __atomic_load_n(&sJobFrame, __ATOMIC_RELAXED)) >= 0)
    {
        frame = jobFrame;
    }
    unsigned long isLow = RealOrVirtual(memType);
    unsigned long newTop
        = (p_frame[frame][isLow] + n_frame[frame][isLow] + 31)
        & ~31u;
    unsigned long newSize = size + (newTop - p_frame[frame][isLow]);
    if (newSize > FrameMemSizes[isLow])
    {
        return 0;
    }
    n_frame[frame][isLow] = newSize;
    return (void*)newTop;
}
#endif

unsigned long glx_GetFreeMemory()
{
    return glGetCurrentResourcePool()->GetFreeMemory();
}
