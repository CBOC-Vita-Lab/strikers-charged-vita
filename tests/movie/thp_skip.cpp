#include <unistd.h>
#include <stdlib.h>
#define main OriginalPlayerTestMain
#include "platform/sdk/tests/game/thp_player.cpp"
#undef main
extern "C" int np_irq_disabled(void);
static bool g_checkMixExclusion;
static unsigned g_mixChecks;
static void CheckMixFlush(void* ptr, u32 length) {
    if (g_checkMixExclusion) {
        CHECK(np_irq_disabled());
        ++g_mixChecks;
    }
    DCFlushRange(ptr, length);
}
#define DCFlushRange CheckMixFlush
#include THP_PLAYER_IMPLEMENTATION
#undef DCFlushRange

extern "C" long THPSimpleDecodeVideo(long, int) __attribute__((weak));

static void CheckReadSubmissionRecovery() {
    FakeFile file;
    unsigned char compressed[32] = {};
    file.f = tmpfile();
    CHECK(file.f != NULL);
    if (!file.f) return;
    CHECK(fwrite(compressed, 1, sizeof(compressed), file.f) == sizeof(compressed));
    memset(&SimpleControl, 0, sizeof(SimpleControl));
    SimpleControl.fileInfo = &file;
    SimpleControl.open = 1;
    SimpleControl.preFetchState = 1;
    SimpleControl.numFrames = 20;
    SimpleControl.readSize = sizeof(compressed);
    SimpleControl.readBuffer[0].ptr = compressed;
    const unsigned submissions = g_readSubmissions;

    // A refused request has no callback. It must leave the head retryable, not
    // pretend that disk I/O is in flight forever while the audio ring drains.
    g_rejectReadSubmission = true;
    CHECK(THPSimpleDecodeVideo(0, 0) == 5);
    CHECK(g_readSubmissions == submissions + 1);
    CHECK(SimpleControl.readProgress == 0);
    CHECK(!nlAsyncReadsPending(&file));
    CHECK(SimpleControl.totalReadFrame == 0);
    CHECK(SimpleControl.nextDecodeIndex == 0);
    CHECK(SimpleControl.audioDecodeIndex == 0);

    g_rejectReadSubmission = false;
    CHECK(THPSimpleDecodeVideo(0, 0) == 5);
    CHECK(g_readSubmissions == submissions + 2);
    CHECK(SimpleControl.readProgress == 1);
    CHECK(nlAsyncReadsPending(&file));
    // A legitimate in-flight request must not be resubmitted by wait retries.
    CHECK(THPSimpleDecodeVideo(0, 0) == 5);
    CHECK(g_readSubmissions == submissions + 2);
    CHECK(SimpleControl.readProgress == 1);
    CHECK(SimpleControl.audioDecodeIndex == 0);
    nlCancelPendingAsyncReads(&file, NULL);

    // Completing during submission clears readProgress before nlReadAsync
    // returns. Do not overwrite that completion with an accepted-request flag.
    SimpleControl.readProgress = 0;
    SimpleControl.numFrames = 1;
    g_completeReadSynchronously = true;
    CHECK(THPSimpleDecodeVideo(0, 0) == 5);
    g_completeReadSynchronously = false;
    CHECK(SimpleControl.readProgress == 0);
    CHECK(SimpleControl.totalReadFrame == 1);
    CHECK(SimpleControl.readBuffer[0].isValid);
    CHECK(!nlAsyncReadsPending(&file));
    CHECK(THPSimpleDecodeVideo(0, 0) == 0);
    CHECK(THPSimpleDecodeVideo(0, 0) == 2);
    memset(&SimpleControl, 0, sizeof(SimpleControl));
    fclose(file.f);
}

// A movie file cut short (a damaged copy, an interrupted transfer): the disc layer zero-fills what lies past the end of the file, so
// the size field of the next frame reads 0. The player must end the movie there (the same status as the real end) instead of
// decoding the zeros or reading past its buffers, and name the file in the log.
static void CheckTruncatedMovie(const char* discFiles, bool tableKnowsTheLength) {
    char src[512];
    snprintf(src, sizeof(src), "%s/Art/movies/stad_classroom.thp", discFiles);
    FILE* in = fopen(src, "rb");
    CHECK(in != NULL);
    if (!in) return;
    fseek(in, 0, SEEK_END);
    const long full = ftell(in);
    fseek(in, 0, SEEK_SET);
    const long keep = (full * 6 / 10) | 1;   // not on a frame boundary
    char tmp[] = "/tmp/np_thp_cut_XXXXXX";
    const int fd = mkstemp(tmp);
    CHECK(fd >= 0);
    if (fd < 0) { fclose(in); return; }
    {
        char chunk[65536];
        long left = keep;
        while (left > 0) {
            const size_t n = fread(chunk, 1, left < (long)sizeof(chunk) ? (size_t)left : sizeof(chunk), in);
            if (n == 0) break;
            CHECK(write(fd, chunk, n) == (ssize_t)n);
            left -= (long)n;
        }
    }
    close(fd);
    fclose(in);

    // tableKnowsTheLength: the file reports its real (short) length, the player ends the movie before the read that would pass it;
    // otherwise the file table still says the size of the whole movie and the disc layer zero-fills what it cannot read
    g_zeroFillShortReads = !tableKnowsTheLength;
    g_fileSizeOverride = tableKnowsTheLength ? 0 : full;
    CHECK(THPSimpleOpen(tmp) == 1);
    THPVideoInfo vi;
    THPSimpleGetVideoInfo(&vi);
    const unsigned yBytes = vi.xSize * ((vi.ySize + 15u) & ~15u);
    for (unsigned i = 0; i < 3; ++i) g_planes[i] = (unsigned char*)calloc(1, i ? yBytes / 4 : yBytes);
    unsigned char* memory = (unsigned char*)memalign(32, THPSimpleCalcNeedMemory());
    CHECK(THPSimpleSetBuffer(memory) == 1);
    CHECK(THPSimplePreLoad(0) == 1);
    const unsigned total = THPSimpleGetTotalFrame();
    unsigned decoded = 0;
    long r = -1;
    for (unsigned spins = 0; spins < 4 * total + 200; ++spins) {
        for (unsigned i = 0; i < 6; ++i) SimpleControl.audioBuffer[i].validSample = 0;
        nlServiceFileSystem();
        r = THPSimpleDecodeVideo(0, 0);
        if (r == 0) ++decoded;
        else if (r != 5) break;
    }
    printf("truncated movie (%s): %ld of %ld bytes kept, %u of %u frames played, ended with status %ld\n", tableKnowsTheLength ? "real length" : "zero-filled", keep, full, decoded, total, r);
    CHECK(r == 2);
    CHECK(decoded > 0 && decoded < total);
    CHECK(THPSimpleDecodeVideo(0, 0) == 2);
    THPSimpleLoadStop();
    CHECK(THPSimpleClose() == 1);
    g_zeroFillShortReads = false;
    g_fileSizeOverride = 0;
    for (unsigned i = 0; i < 3; ++i) free(g_planes[i]);
    free(memory);
    unlink(tmp);
}

int main() {
    if (!THPSimpleDecodeVideo) {
        puts("FAIL: no audio-preserving video skip API");
        return 1;
    }
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    OSInit();
    AIInit(0);
    AIRegisterDMACallback([]() {});
    CHECK(THPSimpleInit(1) == 1);
    if (g_fail) return 1;
    CheckReadSubmissionRecovery();
    if (g_fail) return 1;
    const char* discFiles = getenv("MSC_DISC_FILES");
    if (!discFiles || !*discFiles) {
        puts("movie read-admission: passed; real-movie integration skipped (set MSC_DISC_FILES)");
        return 0;
    }
    const char* names[] = {"introtest.thp", "stad_classroom.thp"};
    unsigned tested = 0;
    for (const char* name : names) {
        char path[512];
        snprintf(path, sizeof(path), "%s/Art/movies/%s", discFiles, name);
        CHECK(THPSimpleOpen(path) == 1);
        if (g_fail) return 1;
        THPVideoInfo vi;
        THPSimpleGetVideoInfo(&vi);
        const unsigned yBytes = vi.xSize * ((vi.ySize + 15u) & ~15u);
        for (unsigned i = 0; i < 3; ++i) g_planes[i] = (unsigned char*)calloc(1, i ? yBytes / 4 : yBytes);
        unsigned char* memory = (unsigned char*)memalign(32, THPSimpleCalcNeedMemory());
        CHECK(THPSimpleSetBuffer(memory) == 1);
        CHECK(THPSimplePreLoad(1) == 1);
        CHECK(THPSimpleGetDecodeCapacity() == 6);

        // A failing picture must already have published its audio, but retrying
        // that same compressed packet must not publish it a second time.
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            unsigned char* y = SimpleControl.textureSet.ytexture;
            SimpleControl.textureSet.ytexture = NULL;
            CHECK(THPSimpleDecodeVideo(0, 1) == 1);
            CHECK(SimpleControl.nextDecodeIndex == 0);
            CHECK(SimpleControl.audioDecodeIndex == 1);
            CHECK(SimpleControl.audioBuffer[0].validSample > 0);
            SimpleControl.textureSet.ytexture = y;
            if (attempt == 0) {
                SimpleControl.audioBuffer[0].validSample = 0; // Already played.
                CHECK(THPSimpleDecodeVideo(0, 1) == 0);
                CHECK(SimpleControl.nextDecodeIndex == 1);
                CHECK(SimpleControl.audioDecodeIndex == 1);
                CHECK(SimpleControl.audioBuffer[0].validSample == 0);
            }
            THPSimpleLoadStop();
            CHECK(THPSimplePreLoad(1) == 1);
        }

        // Save both the final visible image and each audio packet from six full decodes.
        unsigned char* expectedVideo[3];
        for (unsigned i = 0; i < 3; ++i) expectedVideo[i] = (unsigned char*)malloc(i ? yBytes / 4 : yBytes);
        short* expectedAudio[6];
        unsigned samples[6];
        for (unsigned frame = 0; frame < 6; ++frame) {
            CHECK(THPSimpleDecode(0) == 0);
            CHECK(THPSimpleGetDecodeCapacity() == 5 - frame);
            const THPAudioBuffer& audio = SimpleControl.audioBuffer[frame];
            samples[frame] = audio.validSample;
            expectedAudio[frame] = (short*)malloc(samples[frame] * 4);
            memcpy(expectedAudio[frame], audio.buffer, samples[frame] * 4);
        }
        for (unsigned i = 0; i < 3; ++i) memcpy(expectedVideo[i], g_planes[i], i ? yBytes / 4 : yBytes);
        THPSimpleLoadStop();
        CHECK(THPSimplePreLoad(1) == 1);

        // Skip five pictures without starting the audio consumer. All six audio buffers
        // must match the uninterrupted run, and the sixth picture must be identical.
        for (unsigned i = 0; i < 3; ++i) memset(g_planes[i], 0xA5, i ? yBytes / 4 : yBytes);
        for (unsigned frame = 0; frame < 6; ++frame) {
            CHECK(THPSimpleDecodeVideo(0, frame == 5) == 0);
            CHECK(SimpleControl.audioBuffer[frame].validSample == samples[frame]);
            CHECK(!memcmp(SimpleControl.audioBuffer[frame].buffer, expectedAudio[frame], samples[frame] * 4));
            if (frame < 5) {
                for (unsigned plane = 0; plane < 3; ++plane) {
                    const unsigned bytes = plane ? yBytes / 4 : yBytes;
                    for (unsigned i = 0; i < bytes; ++i) {
                        if (g_planes[plane][i] != 0xA5) { CHECK(false); break; }
                    }
                }
            }
        }
        for (unsigned i = 0; i < 3; ++i) CHECK(!memcmp(expectedVideo[i], g_planes[i], i ? yBytes / 4 : yBytes));
        const long pending = SimpleControl.nextDecodeIndex;
        CHECK(THPSimpleDecodeVideo(0, 0) == 3);
        CHECK(SimpleControl.nextDecodeIndex == pending);
        CHECK(THPSimpleGetDecodeCapacity() == 0);
        CHECK(SimpleControl.readBuffer[pending].isValid);
        CHECK(THPSimpleDecodeVideo(99, 0) == 4);
        CHECK(SimpleControl.nextDecodeIndex == pending);

        // Exhaust the preloaded packets without servicing asynchronous disk reads.
        // This is a temporary wait, not EOF, and must not consume another packet.
        for (unsigned i = 0; i < 6; ++i) SimpleControl.audioBuffer[i].validSample = 0;
        for (unsigned i = 0; i < 4; ++i) CHECK(THPSimpleDecodeVideo(0, 0) == 0);
        const long waiting = SimpleControl.nextDecodeIndex;
        CHECK(THPSimpleGetDecodeCapacity() == 0);
        CHECK(THPSimpleDecodeVideo(0, 1) == 5);
        CHECK(SimpleControl.nextDecodeIndex == waiting);
        nlServiceFileSystem();
        CHECK(THPSimpleDecodeVideo(0, 1) == 0);

        // A skipped frame may already be in a worker. Reuse the ten-frame read ring
        // repeatedly, then close with outstanding prefetch work.
        const unsigned ringFrames = tested ? 2 * THPSimpleGetTotalFrame() + 12 : 48;
        for (unsigned frame = 0; frame < ringFrames; ++frame) {
            for (unsigned i = 0; i < 6; ++i) SimpleControl.audioBuffer[i].validSample = 0;
            nlServiceFileSystem();
            CHECK(THPSimpleDecodeVideo(0, frame % 3 == 2) == 0);
        }
        // Distinguish missing movie samples from a healthy device output queue.
        for (unsigned i = 0; i < 6; ++i) SimpleControl.audioBuffer[i].validSample = 0;
        short mixed[192] = {};
        short source[192] = {};
        SimpleControl.audioState = 1;
        const unsigned gaps = NpThpAudioGapBlocks;
        const unsigned missing = NpThpAudioGapSamples;
        MixAudio(mixed, NULL, 96);
        CHECK(NpThpAudioGapBlocks == gaps + 1);
        CHECK(NpThpAudioGapSamples == missing + 96);
        THPAudioBuffer& ready = SimpleControl.audioBuffer[SimpleControl.audioOutputIndex];
        ready.validSample = 96;
        ready.curPtr = expectedAudio[0];
        MixAudio(mixed, NULL, 96);
        CHECK(NpThpAudioGapSamples == missing + 96);
        MixAudio(mixed, source, 96);
        CHECK(NpThpAudioGapBlocks == gaps + 2);
        CHECK(NpThpAudioGapSamples == missing + 192);
        // Native callbacks must keep the producer's exclusion while consuming
        // the ring, in both standalone and AX-mixed audio modes.
        g_checkMixExclusion = true;
        for (unsigned system = 0; system < 3; ++system) {
            AudioSystem = system;
            const BOOL enabled = OSDisableInterrupts();
            THPAudioMixCallback();
            CHECK(np_irq_disabled());
            OSRestoreInterrupts(enabled);
        }
        g_checkMixExclusion = false;
        CHECK(g_mixChecks == (tested + 1) * 3);
        AudioSystem = 1;
        THPSimpleAudioStop();
        THPSimpleLoadStop();

        // A finite movie must distinguish a delayed read from its real end.
        CHECK(THPSimplePreLoad(0) == 1);
        const unsigned total = THPSimpleGetTotalFrame();
        CHECK(total > 10);
        for (unsigned frame = 0; frame < 10; ++frame) {
            for (unsigned i = 0; i < 6; ++i) SimpleControl.audioBuffer[i].validSample = 0;
            CHECK(THPSimpleDecodeVideo(0, 0) == 0);
        }
        const long delayed = SimpleControl.nextDecodeIndex;
        CHECK(THPSimpleDecodeVideo(0, 0) == 5);
        CHECK(SimpleControl.nextDecodeIndex == delayed);
        for (unsigned frame = 10; frame < total; ++frame) {
            for (unsigned i = 0; i < 6; ++i) SimpleControl.audioBuffer[i].validSample = 0;
            nlServiceFileSystem();
            CHECK(THPSimpleDecodeVideo(0, 0) == 0);
        }
        nlServiceFileSystem();
        const long end = SimpleControl.nextDecodeIndex;
        CHECK(THPSimpleDecodeVideo(0, 1) == 2);
        CHECK(THPSimpleDecodeVideo(0, 0) == 2);
        CHECK(SimpleControl.nextDecodeIndex == end);
        THPSimpleLoadStop();
        CHECK(THPSimpleClose() == 1);
        for (unsigned i = 0; i < 3; ++i) { free(expectedVideo[i]); free(g_planes[i]); }
        for (unsigned i = 0; i < 6; ++i) free(expectedAudio[i]);
        free(memory);
        printf("skip/audio/ring/close: %s tested\n", name);
        ++tested;
    }
    CheckTruncatedMovie(discFiles, true);
    CheckTruncatedMovie(discFiles, false);
    THPSimpleQuit();
    printf("THP skip: %u movies, %d failures\n", tested, g_fail);
    return g_fail ? 1 : 0;
}
