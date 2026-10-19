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
    const char* names[] = {"introtest.thp", "stad_classroom.thp"};
    unsigned tested = 0;
    for (const char* name : names) {
        char path[512];
        snprintf(path, sizeof(path), "%s/Art/movies/%s", getenv("MSC_DISC_FILES"), name);
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
    THPSimpleQuit();
    printf("THP skip: %u movies, %d failures\n", tested, g_fail);
    return g_fail ? 1 : 0;
}
