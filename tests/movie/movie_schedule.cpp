// Exercise the real MoviePlay with deterministic clocks and decoder backpressure.
// Hardware drawing and compressed-data decoding are the only test doubles.
#include <stdio.h>
#include <string.h>
#include "src/Game/Sys/movie.cpp"

static unsigned clockTick = 100;
static int resultCode = 0;
static unsigned audioCapacity = 6;
static unsigned decodeCost = 0;
static int successfulPacketsLeft = -1;
static unsigned videoCalls, audioOnlyCalls, audioStarts, prepares;
static int order[256];
static unsigned orderCount;
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

s32 glxGetFrameCount() { return clockTick; }
void nlServiceFileSystem() {}
extern "C" void GXSetDrawDone() {}
extern "C" void GXFlush() {}
extern "C" void GXWaitDrawDone() {}
extern "C" void GXInvalidateTexAll() {}
extern "C" void OSYieldThread() {}
void PlatTexture::Prepare() { ++prepares; }
PlatTexture::~PlatTexture() {}
extern "C" void THPSimpleAudioStart() { ++audioStarts; }
extern "C" unsigned THPSimpleGetDecodeCapacity() { return audioCapacity; }
static long Decode(int video) {
    if (video) ++videoCalls; else ++audioOnlyCalls;
    if (orderCount < 256) order[orderCount++] = video;
    clockTick += video ? decodeCost : 0;
    if (successfulPacketsLeft == 0) return 3;
    if (successfulPacketsLeft > 0) --successfulPacketsLeft;
    return resultCode;
}
extern "C" long THPSimpleDecode(long) { return Decode(1); }
extern "C" long THPSimpleDecodeVideo(long, int video) { return Decode(video); }

int main() {
    const char* test = getenv("MSC_TEST_CASE");
    if (!test) return 2;
    static PlatTexture textures[3];
    for (int i = 0; i < 3; ++i) pTex[i] = &textures[i];
    g_bActive = true;
    g_bSyncedDecode = true;
    g_uNextDecodeFrame = 100;
    g_uRenderTickCount = 1;
    g_uLastDecodeRenderTick = ~0u;
    start = true;

    if (!strcmp(test, "backpressure") || !strcmp(test, "read_backpressure")) {
        resultCode = !strcmp(test, "backpressure") ? 3 : 5;
        CHECK(MoviePlay());
        CHECK(GetMovieFrame() == 0);
        CHECK(g_uDecodeCount == 0);
        CHECK(g_uNextDecodeFrame == 100);
        CHECK(audioStarts == 0);
        CHECK(prepares == 0);
        resultCode = 0;
        MovieRenderTick();
        CHECK(MoviePlay());
        CHECK(GetMovieFrame() == 1);
        CHECK(audioStarts == 1);
    } else if (!strcmp(test, "task_starvation")) {
        decodeCost = 3;
        for (int task = 0; task < 16; ++task) CHECK(MoviePlay());
        CHECK(videoCalls == 1);
        CHECK(GetMovieFrame() == 1);
        CHECK(prepares == 3);
    } else if (!strcmp(test, "late_frame")) {
        clockTick = 104;
        CHECK(MoviePlay());
        CHECK(videoCalls == 1);
        CHECK(audioOnlyCalls == 2);
        CHECK(GetMovieFrame() == 3);
        CHECK(orderCount == 3 && order[0] == 0 && order[1] == 0 && order[2] == 1);
    } else if (!strcmp(test, "last_audio_slot")) {
        clockTick = 120;
        audioCapacity = 1;
        CHECK(MoviePlay());
        CHECK(videoCalls == 1);
        CHECK(audioOnlyCalls == 0);
        CHECK(GetMovieFrame() == 1);
        CHECK(prepares == 3);
    } else if (!strcmp(test, "capacity_race")) {
        clockTick = 120;
        audioCapacity = 0;
        successfulPacketsLeft = 1; // The callback frees a slot after the snapshot.
        CHECK(MoviePlay());
        CHECK(videoCalls == 1);
        CHECK(audioOnlyCalls == 0);
        CHECK(GetMovieFrame() == 1);
        CHECK(prepares == 3);
        MovieRenderTick();
        CHECK(MoviePlay());
        CHECK(GetMovieFrame() == 1);
        CHECK(prepares == 3);
    } else if (!strcmp(test, "preview")) {
        SetSyncedDecode(false);
        CHECK(MoviePlay());
        MovieRenderTick();
        clockTick = 104;
        CHECK(MoviePlay());
        CHECK(videoCalls == 2);
        CHECK(audioOnlyCalls == 1);
        CHECK(GetMovieFrame() == 3);
    } else if (!strcmp(test, "steady")) {
        for (unsigned i = 0; i < 30; ++i) {
            clockTick = 100 + i * 2;
            MovieRenderTick();
            CHECK(MoviePlay());
        }
        CHECK(videoCalls == 30);
        CHECK(audioOnlyCalls == 0);
        CHECK(GetMovieFrame() == 30);
    } else if (!strcmp(test, "not_due")) {
        CHECK(MoviePlay());
        MovieRenderTick();
        clockTick = 101;
        CHECK(MoviePlay());
        CHECK(GetMovieFrame() == 1);
        CHECK(videoCalls == 1);
    } else if (!strcmp(test, "legacy")) {
        clockTick = 104;
        CHECK(MoviePlay());
        CHECK(videoCalls == 1);
        CHECK(audioOnlyCalls == 0);
        CHECK(GetMovieFrame() == 1);
        CHECK(g_uNextDecodeFrame == 106);
    } else if (!strcmp(test, "end")) {
        resultCode = 2;
        CHECK(!MoviePlay());
        CHECK(IsMovieFinished());
        CHECK(GetMovieFrame() == 0);
        CHECK(g_uDecodeCount == 0);
        CHECK(audioStarts == 0);
    } else return 2;
    printf("%s: %s (video %u audio-only %u consumed %u)\n", test,
           failures ? "FAIL" : "PASS", videoCalls, audioOnlyCalls, GetMovieFrame());
    return failures ? 1 : 0;
}
