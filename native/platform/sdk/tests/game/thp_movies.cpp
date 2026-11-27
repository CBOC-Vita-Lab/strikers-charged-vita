// Plays whole movies of the disc through the game's own movie player (THPSimple.cpp of the decomp on the SDK layer, the way movie.cpp drives it:
// THPSimpleDecodeVideo until the player reports the end) and prints, per movie, what came out next to what the file authors:
//
//   MOVIE <name> frames=<decoded> authored=<header> audio_packets=<n> audio_samples=<sum> authored_samples=<sum> audio_hash=<fnv> video_hash=<fnv> OK|DIFF
//
//   frames / authored     the pictures the player consumed against the frame count of the THP header (and the frames the file really holds)
//   audio_samples         the samples per channel the audio decoder produced over the movie against the sum of the packets' own sample counts
//   audio_hash            FNV-1a of every decoded sample (interleaved L, R), in order: equal between two code paths = the same authored sound
//   video_hash            FNV-1a of the three texture planes after every frame (only in the waiting mode, MSC_THP_NOWAIT=0: with NOWAIT the
//                         picture on the screen is whatever was ready, by design)
//
// Usage: test_thp_movies <movie.thp>...   (names relative to the disc's Art/movies, or paths); Movies play in real time (use the short ones). Without
// arguments it plays the shortest stadium preview as a smoke test, and is skipped without the disc dump. The switches of the movie path
// (MSC_THP_HWJPEG, MSC_THP_NOWAIT, MSC_THP_LANE, MSC_THP_DEPTH) are read from the environment as in the game.
#define main thp_player_main
#include "thp_player.cpp"
#undef main

#include <stdint.h>

extern "C" uint32_t __real_THPAudioDecode(int16_t* audioBuffer, uint8_t* audioFrame, int32_t flag);
static uint64_t g_audioHash, g_audioSamples, g_audioPackets;
static uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
extern "C" uint32_t __wrap_THPAudioDecode(int16_t* audioBuffer, uint8_t* audioFrame, int32_t flag) {
    const uint32_t n = __real_THPAudioDecode(audioBuffer, audioFrame, flag);
    if (flag == 0) g_audioHash = Fnv(g_audioHash, audioBuffer, (size_t)n * 2 * sizeof(int16_t));
    g_audioSamples += n;
    ++g_audioPackets;
    return n;
}

static uint32_t Be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// what the file authors: the header's frame count, the frames the file really holds, and the audio samples of the packets
struct Authored {
    bool ok = false;
    uint32_t headerFrames = 0, heldFrames = 0;
    uint64_t samples = 0;
};
static Authored ReadAuthored(const char* path) {
    Authored a;
    FILE* f = fopen(path, "rb");
    if (!f) return a;
    fseek(f, 0, SEEK_END);
    const long fileSize = ftell(f);
    unsigned char h[48];
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "THP", 3) != 0) {
        fclose(f);
        return a;
    }
    a.headerFrames = Be32(h + 20);
    const uint32_t firstSize = Be32(h + 24), compOffset = Be32(h + 32), dataOffset = Be32(h + 40);
    unsigned char comp[20];
    fseek(f, compOffset, SEEK_SET);
    if (fread(comp, 1, sizeof comp, f) != sizeof comp) {
        fclose(f);
        return a;
    }
    const uint32_t nComp = Be32(comp);
    const unsigned char* types = comp + 4;
    uint64_t offset = dataOffset;
    uint32_t size = firstSize;
    for (uint32_t i = 0; i < a.headerFrames; ++i) {
        if ((long)(offset + size) > fileSize || size < 16) break;   // a truncated file holds fewer frames than its header says
        unsigned char fh[8 + 4 * 16];
        const size_t want = 8 + 4 * (size_t)nComp;
        fseek(f, (long)offset, SEEK_SET);
        if (nComp > 16 || fread(fh, 1, want, f) != want) break;
        uint64_t at = offset + want;
        for (uint32_t c = 0; c < nComp; ++c) {
            const uint32_t cs = Be32(fh + 8 + 4 * c);
            if (types[c] == 1) {
                unsigned char ah[8];
                fseek(f, (long)at, SEEK_SET);
                if (fread(ah, 1, 8, f) == 8) a.samples += Be32(ah + 4);
            }
            at += cs;
        }
        ++a.heldFrames;
        const uint32_t next = Be32(fh);
        offset += size;
        size = next;
    }
    a.ok = true;
    fclose(f);
    return a;
}

static int PlayMovie(const char* path, const char* name) {
    const Authored authored = ReadAuthored(path);
    g_audioHash = 1469598103934665603ull;
    g_audioSamples = g_audioPackets = 0;
    uint64_t videoHash = 1469598103934665603ull;
    const char* nw = getenv("MSC_THP_NOWAIT");
    const bool hashVideo = !(nw && *nw != '0');   // hosts default to waiting (exact pictures)

    if (THPSimpleOpen(path) != 1) {
        printf("MOVIE %s open failed\n", name);
        return 1;
    }
    THPVideoInfo vi;
    THPSimpleGetVideoInfo(&vi);
    unsigned long need = THPSimpleCalcNeedMemory();
    unsigned char* mem = (unsigned char*)memalign(32, need);
    THPSimpleSetBuffer(mem);
    THPSimplePreLoad(0);
    const size_t ySize = (size_t)vi.xSize * vi.ySize, cSize = ySize / 4;
    if (ySize > 640 * 480) {
        printf("MOVIE %s is larger than the test's planes (%ux%u)\n", name, (unsigned)vi.xSize, (unsigned)vi.ySize);
        return 1;
    }
    memset(g_planes[0], 0, 640 * 480);
    memset(g_planes[1], 0, 640 * 480 / 4);
    memset(g_planes[2], 0, 640 * 480 / 4);

    uint64_t frames = 0;
    bool started = false;
    int endCode = -1;
    uint64_t lastProgress = np_time_real_ns();
    unsigned long long codes[8] = {0};
    for (uint64_t spins = 0;; ++spins) {
        if (np_time_real_ns() - lastProgress > 8000000000ull) {   // 8 s without a picture: a stall, not a slow movie
            printf("MOVIE %s STALL after %llu frames (decode codes 0..5: %llu %llu %llu %llu %llu %llu)\n", name, (unsigned long long)frames, codes[0],
                   codes[1], codes[2], codes[3], codes[4], codes[5]);
            break;
        }
        nlServiceFileSystem();
        const long r = THPSimpleDecodeVideo(0, 1);
        ++codes[r >= 0 && r < 8 ? r : 7];
        if (r == 0) {
            lastProgress = np_time_real_ns();
            ++frames;
            if (hashVideo) {
                videoHash = Fnv(videoHash, g_planes[0], ySize);
                videoHash = Fnv(videoHash, g_planes[1], cSize);
                videoHash = Fnv(videoHash, g_planes[2], cSize);
            }
            if (!started) {
                THPSimpleAudioStart();
                started = true;
            }
            continue;
        }
        if (r == 1 || r == 2 || r == 4) {
            endCode = (int)r;
            break;
        }
        np_sleep_ns(200000);   // audio buffers full (3) or a read pending (5): let the sink and the disc go on
    }
    THPSimpleAudioStop();
    THPSimpleLoadStop();
    while (nlAsyncReadsPending(0)) nlServiceFileSystem();
    THPSimpleClose();
    free(mem);

    const bool same = authored.ok && frames == authored.heldFrames && authored.heldFrames == authored.headerFrames && g_audioSamples == authored.samples &&
                      g_audioPackets == frames && endCode == 2;
    printf("MOVIE %s frames=%llu authored=%u held=%u audio_packets=%llu audio_samples=%llu authored_samples=%llu audio_hash=%016llx video_hash=%016llx end=%d %s\n",
           name, (unsigned long long)frames, authored.headerFrames, authored.heldFrames, (unsigned long long)g_audioPackets,
           (unsigned long long)g_audioSamples, (unsigned long long)authored.samples, (unsigned long long)g_audioHash,
           hashVideo ? (unsigned long long)videoHash : 0ull, endCode, same ? "OK" : "DIFF");
    fflush(stdout);
    return same ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* dir = getenv("MSC_DISC_FILES");
    if (!dir) dir = "/work/msc/game/files";
    const char* names[256];
    int nNames = 0;
    for (int i = 1; i < argc && nNames < 256; ++i) names[nNames++] = argv[i];
    char probe[512];
    snprintf(probe, sizeof probe, "%s/Art/movies/stad_classroom_pal.thp", dir);
    if (nNames == 0) {
        FILE* f = fopen(probe, "rb");
        if (!f) {
            printf("  (%s not present: skipped)\ntest_thp_movies: ok (skipped)\n", probe);
            return 0;
        }
        fclose(f);
        names[nNames++] = "stad_classroom_pal.thp";
    }

    // (NP_VIRTUAL_TIME=1 is possible but the audio pacing then needs the harness clock: the movies play in real time here)
    setvbuf(stdout, 0, _IOLBF, 0);
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
    g_planes[0] = (unsigned char*)calloc(1, 640 * 480);
    g_planes[1] = (unsigned char*)calloc(1, 640 * 480 / 4);
    g_planes[2] = (unsigned char*)calloc(1, 640 * 480 / 4);
    if (THPSimpleInit(1) != 1) {
        printf("THPSimpleInit failed\n");
        return 1;
    }
    int bad = 0;
    for (int k = 0; k < nNames; ++k) {
        char path[1024];
        if (strchr(names[k], '/')) snprintf(path, sizeof path, "%s", names[k]);
        else snprintf(path, sizeof path, "%s/Art/movies/%s", dir, names[k]);
        const char* slash = strrchr(path, '/');
        bad += PlayMovie(path, slash ? slash + 1 : path);
    }
    THPSimpleQuit();
    printf("test_thp_movies: %s (%d of %d movies differ from the file)\n", bad ? "FAILED" : "ok", bad, nNames);
    return bad ? 1 : 0;
}
