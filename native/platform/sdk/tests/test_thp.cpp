// THP movie decoding against real movie files of the disc dump (skipped when absent): the file
// structures are read the way the patched THPSimple.cpp reads them, every frame of the first
// seconds is decoded and the planes and the audio are checked for plausibility, with the decode time.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <vector>

#include "np/np_sdk.h"

extern "C" {
int THPInit(void);
int THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);
uint32_t THPAudioDecode(int16_t* audioBuffer, uint8_t* audioFrame, int32_t flag);
void NpThpFixHeader(void*);
void NpThpFixCompInfo(void*);
void NpThpFixInfo(void*, int);
void NpThpFixFrame(void*, unsigned int);
}

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static bool test_file(const char* path, int maxFrames) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("  (%s not present: skipped)\n", path);
        return false;
    }
    uint32_t hdr[16];
    CHECK(fread(hdr, 4, 12, f) == 12);
    CHECK(memcmp(hdr, "THP", 4) == 0);
    NpThpFixHeader(hdr);
    uint32_t version = hdr[1], bufSize = hdr[2], audioMax = hdr[3], numFrames = hdr[5], firstSize = hdr[6];
    uint32_t compOff = hdr[8], movieOff = hdr[10];
    float fps;
    memcpy(&fps, &hdr[4], 4);
    CHECK(version == 0x00011000);
    fseek(f, compOff, SEEK_SET);
    uint32_t comp[8];
    CHECK(fread(comp, 4, 5, f) == 5);
    NpThpFixCompInfo(comp);
    uint32_t nc = comp[0];
    const uint8_t* types = (const uint8_t*)&comp[1];
    uint32_t vi[4] = {0}, ai[4] = {0};
    bool haveAudio = false;
    long off = compOff + 20;
    for (uint32_t i = 0; i < nc; ++i) {
        fseek(f, off, SEEK_SET);
        if (types[i] == 0) {
            CHECK(fread(vi, 4, 3, f) == 3);
            NpThpFixInfo(vi, 3);
            off += 12;
        } else {
            CHECK(fread(ai, 4, 4, f) == 4);
            NpThpFixInfo(ai, 4);
            haveAudio = true;
            off += 16;
        }
    }
    printf("  %s: %ux%u, %.3f fps, %u frames, %u component(s), audio %s (%u Hz, %u ch)\n", path, vi[0], vi[1], fps, numFrames, nc,
           haveAudio ? "yes" : "no", ai[1], ai[0]);
    CHECK(vi[0] > 0 && vi[0] <= 1024 && vi[1] > 0 && vi[1] <= 1024 && (vi[0] % 16) == 0);
    uint32_t w = vi[0], h = vi[1];
    std::vector<uint8_t> yPlane(w * h), uPlane(w * h / 4), vPlane(w * h / 4), work(1 << 16);
    std::vector<int16_t> pcm(audioMax * 2 + 64);
    std::vector<uint8_t> frame(bufSize + 64);
    uint32_t size = firstSize;
    long pos = movieOff;
    double decMs = 0;
    int decoded = 0;
    long double sumY = 0;
    uint64_t pcmEnergy = 0, pcmCount = 0;
    for (int i = 0; i < maxFrames && (uint32_t)i < numFrames; ++i) {
        fseek(f, pos, SEEK_SET);
        if (size > bufSize) { CHECK(size <= bufSize); break; }
        if (fread(frame.data(), 1, size, f) != size) break;
        memset(frame.data() + size, 0, 64);
        NpThpFixFrame(frame.data(), nc);
        uint32_t next = *(uint32_t*)frame.data();
        uint32_t* cs = (uint32_t*)(frame.data() + 8);
        uint8_t* p = frame.data() + nc * 4 + 8;
        for (uint32_t c = 0; c < nc; ++c) {
            if (types[c] == 0) {
                double t0 = now_ms();
                int r = THPVideoDecode(p, yPlane.data(), uPlane.data(), vPlane.data(), work.data());
                decMs += now_ms() - t0;
                CHECK(r == 0);
                decoded++;
            } else if (haveAudio) {
                uint32_t n = THPAudioDecode(pcm.data(), p, 0);
                CHECK(n <= audioMax);
                for (uint32_t k = 0; k < n * 2; ++k) pcmEnergy += (uint64_t)((int64_t)pcm[k] * pcm[k]) >> 8;
                pcmCount += n * 2;
            }
            p += cs[c];
        }
        pos += size;
        size = next;
    }
    for (uint8_t b : yPlane) sumY += b;
    double meanY = (double)sumY / yPlane.size();
    printf("  decoded %d frames, %.2f ms/frame on average; mean luma %.1f; audio rms %.1f over %llu samples\n", decoded,
           decoded ? decMs / decoded : 0.0, meanY, pcmCount ? sqrt((double)pcmEnergy * 256.0 / pcmCount) : 0.0,
           (unsigned long long)pcmCount);
    CHECK(decoded > 0);
    CHECK(meanY > 2.0 && meanY < 253.0);
    if (haveAudio) CHECK(pcmCount > 0);
    fclose(f);
    return true;
}

int main() {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    THPInit();
    const char* dir = getenv("MSC_DISC_FILES");
    if (!dir) dir = "/home/claude/msc/game/files";
    const char* names[] = {"Art/movies/nlglogo.thp", "Art/movies/nlgintrowide.thp", "Art/movies/stad_classroom_pal.thp"};
    int ran = 0;
    for (const char* n : names) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, n);
        ran += test_file(path, 120) ? 1 : 0;
    }
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_thp: ok (%d file(s))\n", ran);
    return 0;
}
