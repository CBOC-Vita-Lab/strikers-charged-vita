// The packet copies of the two-stage pipeline with MSC_FS_SNAPONCE on and off, on the real src/NL/gl/glRenderList.cpp: what the send reads after the refresh
// at the fork must be byte for byte the same, whatever the build did to the persistent packet after the attach. Built by run_tests.sh for both defaults.
#include <revolution/types.h>
#include "NL/gl/glRenderList.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/glx/glxMemory.h"
#include "NL/glx/glxSwap.h"
#include "np/framesend.h"
#include "np/overlap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- the frame arena and the pipeline flag, as the game has them
volatile int NpFsPipeOn = 1;
volatile int NpFsBusy = 0;
static unsigned char sArena[1 << 20];
static unsigned long sUsed, sGeneration, sCap = sizeof(sArena);
void* glplatFrameAllocTry(unsigned long size, eGLMemory)
{
    unsigned long top = (sUsed + 31) & ~31ul;
    if (top + size > sCap)
        return 0;
    sUsed = top + size;
    return sArena + top;
}
unsigned long glplatFrameGeneration() { return sGeneration; }
s32 glxGetFrameCount() { return 0; }

static unsigned long sMemcpyBytes;   // the bytes the attach did not copy
static int fails;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static unsigned long sSkipped;
extern "C" void NpOverlapSnapSkipped(unsigned long bytes) { ++sSkipped; sMemcpyBytes += bytes; }
extern "C" int NpFsSnapOnce(void) { return std::getenv("T_ONCE") != 0; }

static GXMaterialParameter sInfo = {0xFB3B01EC, 0, 8};   // the skin matrix buffer of a packet: {pointer, size} at offset 8
struct Program : GLMaterialProgram
{
    void Activate(GLView*) {}
    void Deactivate() {}
    void Configure(glModelPacket*) {}
    void Prepare(glModelPacket*) {}
    void Draw(const glModelPacket*) {}
    const GXMaterialParameter* GetParameters() { return &sInfo; }
    void Initialize() {}
};

int main()
{
    // one packet with a parameter block that holds a skin buffer {pointer, size} at offset 8, and one stream
    static unsigned long skin[12], skinB[12];
    for (int i = 0; i < 12; ++i) { skin[i] = 100 + i; skinB[i] = 200 + i; }
    alignas(8) static unsigned char paramBlock[8 + 2 * sizeof(unsigned long) + 8];
    unsigned long* buffer = (unsigned long*)(paramBlock + 8);
    buffer[0] = (unsigned long)skin;
    buffer[1] = sizeof(skin);
    static glModelStream streams[2];
    std::memset(streams, 0x11, sizeof(streams));
    static Program program;
    program.parameterCount = 1;
    program.parameterDataSize = sizeof(paramBlock);
    static glModelPacket src;
    std::memset(&src, 0, sizeof(src));
    src.materialProgram = (decltype(src.materialProgram))&program;
    src.materialParameters = paramBlock;
    src.streams = streams;
    src.numStreams = 2;
    src.rasterState = 7;

    const glModelPacket* copy = GLSnapshotPacket(&src);
    CHECK(copy != &src);
    // before the fork a copy reads as a packet in every mode: the fields, and pointers that lead somewhere
    CHECK(copy->rasterState == 7 && copy->numStreams == 2 && copy->materialProgram == src.materialProgram);
    CHECK(((unsigned long*)copy->materialParameters)[0] == *(unsigned long*)paramBlock);

    // what the build does after the attach
    src.rasterState = 9;
    std::memset(paramBlock, 0x5A, 8);
    buffer[0] = (unsigned long)skinB;
    std::memset(streams, 0x22, sizeof(streams));
    for (int i = 0; i < 12; ++i) skinB[i] = 300 + i;

    GLRefreshPacketSnapshots();

    CHECK(copy->rasterState == 9);
    CHECK(copy->streams != streams && std::memcmp(copy->streams, streams, sizeof(streams)) == 0);
    CHECK(copy->materialParameters != paramBlock);
    const unsigned char* cp = (const unsigned char*)copy->materialParameters;
    CHECK(std::memcmp(cp, paramBlock, 8) == 0);
    const unsigned long* cb = (const unsigned long*)(cp + 8);
    CHECK(cb[0] != (unsigned long)skinB && cb[1] == sizeof(skinB));
    CHECK(std::memcmp((const void*)cb[0], skinB, sizeof(skinB)) == 0);
    // the job's copy no longer depends on the persistent memory
    std::memset(paramBlock, 0, 8);
    for (int i = 0; i < 12; ++i) skinB[i] = 0;
    CHECK(((const unsigned long*)cb[0])[3] == 303);
    CHECK(GLSnapshotFailures() == 0);
    const bool once = std::getenv("T_ONCE") != 0;
    CHECK(once ? sSkipped == 1 && sMemcpyBytes == 2 * sizeof(glModelStream) + sizeof(paramBlock) + sizeof(skin) : sSkipped == 0);

    // an arena too small for the copy: the persistent packet is attached, as before, and counted as a failure of the frame
    sCap = sUsed;
    CHECK(GLSnapshotPacket(&src) == &src);
    CHECK(GLSnapshotFailures() == 1);

    std::printf(fails ? "snaponce %s: %d FAILED\n" : "snaponce %s: ok\n", once ? "on" : "off", fails);
    return fails != 0;
}
