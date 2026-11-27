// The decode-ahead lanes of the movie player (thp_native.cpp) driven the way THPSimple.cpp drives them, on real THP frames of the disc
// dump (skipped where the files are absent): the frames of a ring of read buffers are prefetched, asked for one after the other, the
// buffer of a consumed frame is refilled with a later one. Scenarios (each in a child process, the switches are read once per process):
//   wait/soft      MSC_THP_NOWAIT=0, no codec: every picture is the software decoder's, frame for frame
//   wait/fake      the host stand-in codec (MSC_THP_HWJPEG=fake): the same pictures through bridge, codec, pitch and retiling
//   wait/fail      ... with the codec refusing every 3rd frame: the software fallback gives the same pictures
//   nowait/fake    MSC_THP_NOWAIT=1: pictures equal the reference of the asked or an earlier frame, never older than the last one shown
//   nowait/slow    a codec that takes 90 ms a frame: the calls do not wait (each returns within 25 ms), pictures still advance
//   nowait/bad     a codec that returns wrong pictures: the check of the first frames switches it off, the pictures are right from then on
//   wait/damaged   a frame cut in half: the decode reports an error, the next intact frame decodes
//   nowait/first   a codec that takes 90 ms a frame and the very first call at once: the movie's first picture is there at that call (one bounded
//                  wait for the lane), not the picture left by the previous movie; MSC_THP_FIRSTFRAME=0 is the old "picture stays" (checked too)
//   nowait/big     every 7th frame is announced as larger than a lane takes (2 MiB): it is decoded on the game thread, the picture is that frame
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "np/np_plat.h"
#include "np/np_sdk.h"
#include "thp_testutil.h"

extern "C" {
int THPInit(void);
int THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);
void NpThpPrefetch(const void* video, unsigned size, int frame, unsigned width, unsigned height);
int NpThpVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work, int frame, unsigned width, unsigned height, unsigned size);
void NpThpDiscard(const void* video, int frame);
void NpThpReset(void);
void NpThpLogStats(void);
void NpThpNoteName(const char*);
}

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static double NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const int kFrames = 60;

static int RunScenario(const char* scenario) {
    const char* dir = getenv("MSC_DISC_FILES");
    if (!dir || !*dir) dir = "/nonexistent";   // without the disc dump every scenario is skipped
    char path[512];
    snprintf(path, sizeof path, "%s/Art/movies/stad_lavapit_pal.thp", dir);
    std::vector<Frame> frames;
    uint32_t w, h;
    if (!ReadFrames(path, kFrames + 12, 1, kFrames + 12, frames, w, h)) {
        printf("  (%s not present: skipped)\n", path);
        return 0;
    }
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    THPInit();
    NpThpNoteName("lavapit");

    const size_t ysz = (size_t)w * h, uvsz = (size_t)w / 2 * (h / 2);
    // references: the software decoder, one frame at a time
    std::vector<std::vector<uint8_t>> ref[3];
    for (int i = 0; i < kFrames; ++i) {
        std::vector<uint8_t> p[3] = {std::vector<uint8_t>(ysz), std::vector<uint8_t>(uvsz), std::vector<uint8_t>(uvsz)};
        CHECK(THPVideoDecode((void*)frames[i].data.data(), p[0].data(), p[1].data(), p[2].data(), nullptr) == 0);
        for (int k = 0; k < 3; ++k) ref[k].push_back(p[k]);
    }

    const bool nowait = !strncmp(scenario, "nowait", 6);
    const bool slow = !strcmp(scenario, "nowait/slow");
    const bool damaged = !strcmp(scenario, "wait/damaged");
    const bool firstWait = !strcmp(scenario, "nowait/first");
    const bool firstOff = !strcmp(scenario, "nowait/first-off");
    const bool big = !strcmp(scenario, "nowait/big");
    auto bigFrame = [&](int f) { return big && f % 7 == 3; };
    auto sizeOf = [&](int f) { return bigFrame(f) ? (unsigned)(2u << 20) : (unsigned)frames[f].size; };
    // the player's ring of ten read buffers
    std::vector<uint8_t> ring[10];
    for (int i = 0; i < 10; ++i) ring[i].assign(frames[0].size * 4 + 4096, 0);
    auto load = [&](int f) {
        std::vector<uint8_t>& b = ring[f % 10];
        memset(b.data(), 0xA5, b.size());
        if (f < (int)frames.size()) memcpy(b.data(), frames[f].data.data(), frames[f].size);
    };
    for (int f = 0; f < 10; ++f) {
        if (ring[f % 10].size() < frames[f].size + 64) ring[f % 10].assign(frames[f].size + 4096, 0);
        load(f);
    }
    std::vector<uint8_t> tex[3] = {std::vector<uint8_t>(ysz, 0x10), std::vector<uint8_t>(uvsz, 0x80), std::vector<uint8_t>(uvsz, 0x80)};
    int shown = -1, exact = 0, late = 0, kept = 0, distinct = 0;
    double worstCall = 0;
    for (int i = 0; i < kFrames; ++i) {
        // prefetch, as NpThpPrefetchNextFrames does after a packet was consumed
        for (int a = 0; a < 4 && i + a < kFrames; ++a) NpThpPrefetch(ring[(i + a) % 10].data(), sizeOf(i + a), i + a, w, h);
        const uint8_t* src = ring[i % 10].data();
        unsigned size = sizeOf(i);
        std::vector<uint8_t> cut;
        if (damaged && i == 20) {   // this frame arrives cut in half (the file is truncated in the middle of it)
            cut.assign(frames[i].data.begin(), frames[i].data.begin() + frames[i].size / 2);
            cut.resize(frames[i].size / 2 + 64, 0);
            src = cut.data();
            size = (unsigned)(frames[i].size / 2);
        }
        if (nowait && !slow && !(firstWait || firstOff)) np_sleep_ns(12u * 1000000u);   // the lanes get time between the calls
        if (slow || ((firstWait || firstOff) && i > 0)) np_sleep_ns(33u * 1000000u);
        const double t0 = NowMs();
        const int r = NpThpVideoDecode((void*)src, tex[0].data(), tex[1].data(), tex[2].data(), nullptr, i, w, h, size);
        const double dt = NowMs() - t0;
        if (i > 0 && dt > worstCall) worstCall = dt;   // (the first call of a movie may wait once for its picture)
        // the player refills the buffer of the frame it consumed with a later one
        {
            const int nf = i + 10;
            if (nf < (int)frames.size() && ring[nf % 10].size() < frames[nf].size + 64) ring[nf % 10].assign(frames[nf].size + 4096, 0);
            load(nf);
        }
        if (damaged && i == 20) {
            CHECK(r != 0);   // an error, which ends the movie in the player
            continue;
        }
        CHECK(r == 0);
        if (i == 0 && (firstWait || firstOff)) {
            const bool is0 = tex[0] == ref[0][0] && tex[1] == ref[1][0] && tex[2] == ref[2][0];
            CHECK(firstOff ? !is0 : is0);   // the first picture is there at the first call (and, with the fix off, is not: the old behaviour)
            if (firstWait) CHECK(dt < 1000.0);
        }
        if (bigFrame(i)) CHECK(tex[0] == ref[0][i] && tex[1] == ref[1][i] && tex[2] == ref[2][i]);   // decoded here, not left at an older picture
        // which reference is on the screen now?
        int on = -1;
        for (int j = i; j >= 0 && j >= i - 12; --j)
            if (tex[0] == ref[0][j] && tex[1] == ref[1][j] && tex[2] == ref[2][j]) { on = j; break; }
        if (!nowait) {
            CHECK(on == i);
        } else if (on >= 0) {
            CHECK(on >= shown);   // never an older picture than the one shown before
            if (on != shown) ++distinct;
            shown = on;
            ++(on == i ? exact : late);
        } else {
            ++kept;   // nothing shown yet: the initial gray
            CHECK(shown < 0 || tex[0] == ref[0][shown]);
        }
    }
    NpThpReset();
    NpThpLogStats();
    if (nowait) {
        printf("  %s: exact %d, late %d, kept %d, last shown %d, slowest call %.1f ms\n", scenario, exact, late, kept, shown, worstCall);
        if (firstWait || firstOff) {
            CHECK(shown >= 5);   // the movie plays on behind a slow codec
        } else {
            CHECK(shown >= (slow ? 5 : kFrames - 6));   // the movie plays on (slowly, with a codec far behind the clock)
            if (slow) CHECK(worstCall < 25.0);        // 90 ms of codec time per frame is not waited for
            if (slow) CHECK(distinct < kFrames);      // ... so some pictures were skipped (the first one is always there)
            if (!slow) CHECK(exact + late >= kFrames - 8);
        }
    } else {
        printf("  %s: %d frames equal to the reference\n", scenario, kFrames);
    }
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return RunScenario(argv[1]);
    struct Sc { const char* name; const char* env[6]; };
    const Sc scenarios[] = {
        {"wait/soft", {"MSC_THP_NOWAIT=0", "MSC_THP_HWJPEG=0", nullptr}},
        {"wait/fake", {"MSC_THP_NOWAIT=0", "MSC_THP_HWJPEG=fake", nullptr}},
        {"wait/fail", {"MSC_THP_NOWAIT=0", "MSC_THP_HWJPEG=fake", "MSC_THP_HWJPEG_FAKE_FAIL=3", nullptr}},
        {"nowait/fake", {"MSC_THP_NOWAIT=1", "MSC_THP_HWJPEG=fake", nullptr}},
        {"nowait/slow", {"MSC_THP_NOWAIT=1", "MSC_THP_HWJPEG=fake", "MSC_THP_HWJPEG_FAKE_DELAY_MS=90", "MSC_THP_HWJPEG_VERIFY=0", nullptr}},
        {"nowait/bad", {"MSC_THP_NOWAIT=1", "MSC_THP_HWJPEG=fake", "MSC_THP_HWJPEG_FAKE_BAD=1", "MSC_THP_DEPTH=1", nullptr}},
        {"wait/damaged", {"MSC_THP_NOWAIT=0", "MSC_THP_HWJPEG=0", nullptr}},
        {"nowait/first", {"MSC_THP_NOWAIT=1", "MSC_THP_HWJPEG=fake", "MSC_THP_HWJPEG_FAKE_DELAY_MS=90", "MSC_THP_HWJPEG_VERIFY=0", nullptr}},
        {"nowait/first-off", {"MSC_THP_NOWAIT=1", "MSC_THP_HWJPEG=fake", "MSC_THP_HWJPEG_FAKE_DELAY_MS=90", "MSC_THP_HWJPEG_VERIFY=0", "MSC_THP_FIRSTFRAME=0", nullptr}},
        {"nowait/big", {"MSC_THP_NOWAIT=1", "MSC_THP_HWJPEG=fake", nullptr}},
    };
    int bad = 0;
    for (const Sc& sc : scenarios) {
        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            for (int i = 0; sc.env[i]; ++i) putenv((char*)sc.env[i]);
            const int rc = RunScenario(sc.name);
            fflush(stdout);
            _exit(rc);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            printf("FAIL scenario %s (status 0x%x)\n", sc.name, st);
            bad = 1;
        }
    }
    if (bad) return 1;
    printf("test_thp_lanes: ok\n");
    return 0;
}
