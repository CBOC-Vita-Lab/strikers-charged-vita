// The game's movie player (THPSimple.cpp of the decomp, with patches/thp/THPSimple.cpp.sed) running on
// the SDK layer: THPSimpleInit(1) shares the AI DMA with AX the way the game does, a real movie of the disc
// dump is opened through a fake nlFile, decoded frame by frame into the "movie" texture planes while its audio
// is mixed into the AX output. Checked: the frames decode, the planes are written, the movie's audio reaches the
// sink, the player closes cleanly. Skipped when the disc dump is not present.
#include <revolution/ai.h>
#include <revolution/ax.h>
#include <revolution/mix.h>
#include <revolution/os.h>

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "NL/gl/glState.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include "RVL_SDK/thp/THPSimple.h"

#include "np/np_plat.h"
#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

// ---- fake NL file layer ---------------------------------------------------------------------------
nlFile::nlFile() {}
nlFile::~nlFile() {}

struct FakeFile : public nlFile {
    FILE* f;
    long pos;
    FakeFile() : f(0), pos(0) {}
    virtual u32 FileSize(unsigned int* size) {
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        *size = (unsigned int)ftell(f);
        fseek(f, cur, SEEK_SET);
        return *size;
    }
    virtual void Read(void* buffer, unsigned int size, unsigned long) {
        fseek(f, pos, SEEK_SET);
        size_t n = fread(buffer, 1, size, f);
        (void)n;
        pos += size;
    }
};

struct Pending {
    FakeFile* file;
    void* buffer;
    unsigned int size;
    ReadAsyncCallback cb;
    unsigned long user;
    long at;
    bool used;
};
static Pending g_pending[16];

nlFile* nlOpen(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return 0;
    FakeFile* ff = new FakeFile;
    ff->f = f;
    return ff;
}
void nlClose(nlFile* file) {
    FakeFile* ff = (FakeFile*)file;
    if (ff) {
        fclose(ff->f);
        delete ff;
    }
}
void nlRead(nlFile* file, void* buffer, unsigned int size, unsigned long bs) { file->Read(buffer, size, bs); }
void nlSeek(nlFile* file, unsigned int offset, unsigned long) { ((FakeFile*)file)->pos = (long)offset; }
AsyncEntry* nlReadAsync(nlFile* file, void* buffer, unsigned int size, ReadAsyncCallback cb, unsigned long user, unsigned long) {
    for (Pending& p : g_pending) {
        if (!p.used) {
            p.used = true;
            p.file = (FakeFile*)file;
            p.buffer = buffer;
            p.size = size;
            p.cb = cb;
            p.user = user;
            p.at = ((FakeFile*)file)->pos;
            return 0;
        }
    }
    printf("FAIL: too many pending reads\n");
    g_fail++;
    return 0;
}
bool nlAsyncReadsPending(nlFile*) {
    for (Pending& p : g_pending)
        if (p.used) return true;
    return false;
}
void nlCancelPendingAsyncReads(nlFile*, void (*)(nlFile*, void*, unsigned int, unsigned long, void (*)(nlFile*, void*, unsigned int, unsigned long))) {
    for (Pending& p : g_pending) p.used = false;
}
// what the game's file service does once per frame: complete the reads, in "interrupt" context
void nlServiceFileSystem(void) {
    for (Pending& p : g_pending) {
        if (!p.used) continue;
        Pending q = p;
        p.used = false;
        fseek(q.file->f, q.at, SEEK_SET);
        size_t n = fread(q.buffer, 1, q.size, q.file->f);
        (void)n;
        BOOL en = OSDisableInterrupts();
        q.cb(q.file, q.buffer, q.size, q.user);
        OSRestoreInterrupts(en);
    }
}

// ---- fake texture layer -----------------------------------------------------------------------------
static unsigned char* g_planes[3];
static unsigned char g_blob[3][256];
u32 glGetTexture(const char* name) {
    if (!strcmp(name, "movie")) return 1;
    if (!strcmp(name, "movie_u")) return 2;
    return 3;
}
PlatTexture* glx_GetTex(unsigned long handle) {
    PlatTexture* t = (PlatTexture*)g_blob[handle - 1];
    t->m_SwizzledData = g_planes[handle - 1];
    return t;
}

// ---- audio capture ----------------------------------------------------------------------------------
static int16_t* g_cap = 0;
static uint32_t g_cap_frames = 0;
static const uint32_t kCapMax = 32000 * 3;
static int sink_open(uint32_t) { return 0; }
static void sink_write(const int16_t* lr, uint32_t frames) {
    if (g_cap_frames + frames <= kCapMax) {
        memcpy(g_cap + g_cap_frames * 2, lr, frames * 4);
        g_cap_frames += frames;
    }
    np_sleep_ns((uint64_t)frames * 1000000000ull / 32000 - 100000);
}
static void sink_close(void) {}
static void ax_frame_cb(void) { MIXUpdateSettings(); }

int main() {
    const char* dir = getenv("MSC_DISC_FILES");
    if (!dir) dir = "/home/claude/msc/game/files";
    char path[512];
    snprintf(path, sizeof(path), "%s/Art/movies/stad_classroom_pal.thp", dir);
    FILE* probe = fopen(path, "rb");
    if (!probe) {
        printf("  (%s not present: skipped)\n", path);
        printf("test_thp_player: ok (skipped)\n");
        return 0;
    }
    fclose(probe);

    g_cap = (int16_t*)calloc(kCapMax * 2, sizeof(int16_t));
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    NpAudioSink sink = {sink_open, sink_write, sink_close};
    NpSdk_SetAudioSink(&sink);
    OSInit();
    AIInit(0);
    AXInit();
    MIXInit();
    AXRegisterCallback(ax_frame_cb);
    AXSetMode(0);

    g_planes[0] = (unsigned char*)calloc(1, 640 * 256);
    g_planes[1] = (unsigned char*)calloc(1, 640 * 256 / 4);
    g_planes[2] = (unsigned char*)calloc(1, 640 * 256 / 4);

    CHECK(THPSimpleInit(1) == 1);   // the game's call (movie.cpp): share the AI DMA with AX
    CHECK(THPSimpleOpen(path) == 1);
    THPVideoInfo vi;
    CHECK(THPSimpleGetVideoInfo(&vi) == 1);
    printf("  movie %ux%u, %d frames\n", vi.xSize, vi.ySize, (int)THPSimpleGetTotalFrame());
    CHECK(vi.xSize == 640 && vi.ySize == 256);
    unsigned long need = THPSimpleCalcNeedMemory();
    unsigned char* mem = (unsigned char*)memalign(32, need);
    CHECK(mem != 0);
    CHECK(THPSimpleSetBuffer(mem) == 1);
    CHECK(THPSimplePreLoad(1) == 1);
    THPSimpleAudioStart();

    // play 25 frames (one second at 25 fps) paced by the audio
    int decoded = 0, audioFull = 0, spins = 0;
    uint64_t t0 = np_time_real_ns();
    while (decoded < 25 && spins < 200000) {
        nlServiceFileSystem();
        long r = THPSimpleDecode(0);
        if (r == 0) ++decoded;
        else if (r == 3) ++audioFull;
        if (r != 0) np_sleep_ns(500000);
        ++spins;
    }
    double secs = (np_time_real_ns() - t0) / 1e9;
    printf("  decoded %d frames in %.2f s (audio buffers were full %d times)\n", decoded, secs, audioFull);
    CHECK(decoded == 25);
    // 25 frames of 1/25 s of movie audio: wall time is bounded below by the audio the player had to hand out
    CHECK(secs > 0.5 && secs < 3.0);

    // the planes were written (the last decoded frame: not all zero)
    uint64_t sum = 0;
    for (int i = 0; i < 640 * 256; ++i) sum += g_planes[0][i];
    CHECK(sum > 640ull * 256 * 8);

    // audio of the movie reached the sink: energy over the captured blocks
    np_sleep_ns(100000000);
    uint64_t energy = 0;
    uint32_t nz = 0;
    for (uint32_t i = 0; i < g_cap_frames * 2; ++i) {
        energy += (uint64_t)((int64_t)g_cap[i] * g_cap[i]) >> 8;
        nz += g_cap[i] != 0;
    }
    double rms = g_cap_frames ? sqrt((double)energy * 256.0 / (g_cap_frames * 2)) : 0.0;
    printf("  sink got %u frames, %u non-zero samples, rms %.1f\n", g_cap_frames, nz, rms);
    CHECK(g_cap_frames > 16000);
    CHECK(rms > 200.0);

    THPSimpleAudioStop();
    THPSimpleLoadStop();
    while (nlAsyncReadsPending(0)) nlServiceFileSystem();
    CHECK(THPSimpleClose() == 1);
    THPSimpleQuit();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_thp_player: ok\n");
    return 0;
}
