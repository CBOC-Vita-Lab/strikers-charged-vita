// Host test of the movie lanes' pacing and of their failure paths (MSC_THP_DELAY, MSC_THP_RECOVER, the pace statistics line).
// No game data, no libjpeg: the frames are 16x16 THP frames made here by hand (a flat grey whose level is the frame number), the codec is
// the host stand-in (MSC_THP_HWJPEG=fake) with its test knobs (FAKE_SLOW_EVERY / FAKE_SLOW_MS / FAKE_HANG_AT / FAKE_HANG_MS).
//
//   1. np_thp_pace (thp_pacing.h), the pure functions: the cases their comments describe; delay 0 is the rule the lanes had before the
//      delay line (a transcription of the old inline selection is run against ChooseShow on 20000 random states).
//   2. A model of a decoder (Galaxy's thp_pacing test, adapted): pictures asked once a call, `workers` decoders that take `need` calls each.
//      keeps up: every picture on the call it is due at, none twice; slow pictures: the delay line absorbs them, delay 1 does not;
//      overloaded: never a step back, never early, evenly spread, bounded lag.
//   3. The real lanes, driven the way THPSimple.cpp drives them (each scenario in a child process: the switches are read once per process):
//        d0/fast      delay 0, nothing slow: pictures never back, never ahead, every one on time
//        d2/fast      delay 2: the picture at call k is frame k - 2 (frame 0 until the line is full), exactly
//        d0/slow      a slow picture now and then (140 ms, 4 calls): delay 0 repeats pictures
//        d3/slow      the same under delay 3: no repeat, every call on the due frame (the case the delay line is for)
//        d3/over      every picture 60 ms through one codec (it cannot keep up): never back, never early, no freeze (recovery off)
//        hang/off     the codec hangs in one call for 2.5 s, MSC_THP_RECOVER=0: the old behaviour, the picture freezes for the whole hang
//        hang/on      the same with recovery: the picture moves again within a quarter second, the lane is given up and counted
//        never/off    frames never queued (every lane busy): the picture freezes while calls go on (old behaviour, recovery off)
//        never/on     the same with recovery: a picture at least every 9 calls, the frames decoded on the game thread are counted
//        first/on     the first picture's lane hangs: the first call returns within 2 s with the right picture (old behaviour: waits the hang out)
//        lock/on      one lane hangs in the codec, the watchdog is far away: the other lane leaves the codec's lock loop (codec lock timeouts > 0), nothing is given up
//        first/slow   a lane that takes 150 ms for the first picture is waited for (the first call takes at least 100 ms, no timeout)
//        skew         the clock jumps a minute during a stamped codec call: no lane is given up, the codec stays on
//        loop/on      a looping movie (frame numbers start over) with a hung codec and recovery: pictures advance in every pass and restart at frame 0
//        reset/size   a codec call stuck across the end of a 16x16 movie returns into a 32x16 movie: right pictures, no crash (address sanitizer)
//        heal         a watchdog that fired on a call that later returned is undone at the next movie's start
//        swwatch      (tools15, movie14 review L4) a lane in the SOFTWARE decoder longer than MSC_THP_SWSTUCK_MS is counted once and logged once, and nothing else changes; the stamp is cleared after a real software job
//        pace         the summary line is written once per movie, with the new fields
//
// Usage: test_thp_delay [scenario]   (no argument: everything)
#include "../src/thp_native.cpp"

#include <algorithm>
#include <thread>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>


static int g_fail = 0;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            ++g_fail;                                         \
            printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); \
            printf("" __VA_ARGS__);                           \
            printf("\n");                                     \
        }                                                     \
    } while (0)

using namespace np_thp_pace;

// ---- 1. the pure functions -------------------------------------------------------------------------------------------------
// the selection the lanes made before the delay line (thp_native.cpp, nowait branch): the newest finished frame that is not newer than the one asked for
// and newer than the one on the screen
static int OldChoose(const int64_t* seqs, const bool* done, int n, int64_t frame, int64_t lastShown) {
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (done[i] && seqs[i] <= frame && seqs[i] > lastShown && (best < 0 || seqs[i] > seqs[best])) best = i;
    return best;
}

static uint32_t g_rng = 12345;
static uint32_t Rnd() { return g_rng = g_rng * 1664525u + 1013904223u; }

static void TestPure() {
    CHECK(Due(100, 100, 4) == 100 && Due(100, 103, 4) == 100 && Due(100, 104, 4) == 100 && Due(100, 105, 4) == 101, "Due: the first frame until the line is full");
    CHECK(Due(0, 0, 0) == 0 && Due(7, 9, 0) == 9, "Due: no delay");
    {
        const int64_t seqs[8] = {16, 17, 10, 11, 12, 13, 14, 15};
        const bool all[8] = {true, true, true, true, true, true, true, true};
        CHECK(ChooseShow(seqs, all, 8, 0, 17, 4, true, 12) == 5, "the due frame (13)");
        const bool no13[8] = {true, true, true, true, true, false, true, true};
        CHECK(ChooseShow(seqs, no13, 8, 0, 17, 4, true, 12) == -1, "13 is late and 12 is on the screen: nothing new, the picture stays");
        CHECK(ChooseShow(seqs, no13, 8, 0, 17, 4, true, 11) == 4, "12 is the newest finished not after the due one");
        const bool old[8] = {false, false, true, true, false, false, false, false};
        CHECK(ChooseShow(seqs, old, 8, 0, 17, 4, true, 12) == -1, "no step back: finished frames before the one on the screen are not shown");
        const bool early[8] = {true, true, false, false, false, false, false, false};
        CHECK(ChooseShow(seqs, early, 8, 0, 17, 4, true, 12) == -1, "no frame newer than the due one is ever shown");
        CHECK(ChooseShow(seqs, all, 8, 16, 17, 4, false, 0) == 0, "a movie's first calls: frame 16 (the base) is the due one, frames of the movie before (below the base) are never shown");
        CHECK(ChooseShow(seqs, all, 8, 17, 17, 4, false, 0) == 1, "base 17: only frame 17 qualifies");
        CHECK(ChooseShow(seqs, all, 8, 0, 17, 0, true, 12) == 1, "delay 0: the newest");
    }
    CHECK(!JobStale(100, 100, 101, 4, false, 0), "the first frame is not stale during the first calls");
    CHECK(JobStale(12, 0, 17, 4, true, 11), "older than the frame due at the next call");
    CHECK(!JobStale(13, 0, 17, 4, true, 11), "due at the next call: still wanted");
    CHECK(JobStale(13, 0, 17, 4, true, 13) && JobStale(13, 0, 17, 4, true, 14), "not after the one shown last");
    {
        const int64_t q[3] = {15, 13, 14};
        CHECK(PickJob(q, 3, 17, 4, 2000, 16700, 8000) == 1, "a decoder that keeps up: the oldest first");
        CHECK(PickJob(q, 3, 17, 4, 2000, 16700, 20000) == 2, "13 cannot be made in 20 ms any more: 14");
        CHECK(PickJob(q, 3, 17, 4, 2000, 16700, 40000) == 0, "40 ms: only 15");
        CHECK(PickJob(q, 3, 17, 4, 2000, 16700, 90000) == 0, "none can be made: the newest");
        CHECK(PickJob(q, 3, 17, 4, 2000, 16700, 0) == 1, "no decode time known yet: the oldest");
        CHECK(PickJob(q, 1, 17, 4, 2000, 16700, 90000) == 0, "one job");
    }
    CHECK(StarveNext(0, true, false) == 1 && StarveNext(5, true, false) == 6 && StarveNext(5, true, true) == 0 && StarveNext(5, false, false) == 0, "StarveNext");

    // delay 0 is exactly the old selection
    int diffs = 0;
    for (int t = 0; t < 20000; ++t) {
        int64_t seqs[8];
        bool done[8];
        const int n = 1 + (int)(Rnd() % 8);
        for (int i = 0; i < n; ++i) {
            seqs[i] = (int64_t)(Rnd() % 24);
            done[i] = (Rnd() & 3) != 0;
        }
        const int64_t frame = (int64_t)(Rnd() % 24), lastShown = (int64_t)(Rnd() % 26) - 1;
        const int a = OldChoose(seqs, done, n, frame, lastShown);
        const int b = ChooseShow(seqs, done, n, 0, frame, 0, lastShown >= 0, lastShown);
        if (a != b && !(a >= 0 && b >= 0 && seqs[a] == seqs[b])) ++diffs;
    }
    CHECK(diffs == 0, "delay 0 differs from the old rule in %d of 20000 random states", diffs);
}

// ---- 2. a model of the pipeline ---------------------------------------------------------------------------------------------
struct ModelResult {
    std::vector<long> shown;   // per call: the frame on the screen after the call, -1 none yet
    int repeatsAfterFill = 0, backSteps = 0, early = 0, longestGap = 0, fresh = 0;
};

template <class Need> static ModelResult RunModel(int calls, int64_t delay, int workers, int64_t period, Need needUs, int64_t needEstimate) {
    struct Job {
        int64_t seq;
        int state;   // 0 queued, 1 busy, 2 done, 3 dropped
        int64_t finish;
    };
    std::vector<Job> jobs;
    std::vector<int64_t> freeAt((size_t)workers, 0);
    ModelResult r;
    bool anyShown = false;
    int64_t lastShown = 0;
    int gap = 0;
    for (int64_t t = 0, call = 0; call < calls; t += 500) {
        for (Job& j : jobs)
            if (j.state == 1 && j.finish <= t) j.state = 2;
        const bool isCall = t % period == 0;
        if (isCall) {
            const int64_t seq = call;
            jobs.push_back({seq, 0, 0});
            int64_t seqs[64];
            bool done[64];
            int n = 0;
            for (const Job& j : jobs)
                if (j.seq + 8 > seq && n < 64) {   // the lanes' slots: the frames of the last 8 calls
                    seqs[n] = j.seq;
                    done[n] = j.state == 2;
                    n++;
                }
            const int use = ChooseShow(seqs, done, n, 0, seq, delay, anyShown, lastShown);
            const int64_t due = Due(0, seq, delay);
            if (use >= 0) {
                const int64_t pic = seqs[use];
                if (anyShown && pic < lastShown) r.backSteps++;
                if (pic > due) r.early++;
                r.fresh++;
                gap = 0;
                lastShown = pic;
                anyShown = true;
            } else {
                if (seq >= delay + 1) r.repeatsAfterFill++;
                gap++;
                if (gap > r.longestGap) r.longestGap = gap;
            }
            r.shown.push_back(anyShown ? (long)lastShown : -1);
            call++;
        }
        const int64_t callsDone = call;
        const int64_t sinceCall = isCall ? 0 : t % period;
        for (int w = 0; w < workers; w++) {
            if (freeAt[(size_t)w] > t) continue;
            int64_t seqs[64];
            int idx[64], n = 0;
            for (size_t i = 0; i < jobs.size(); i++) {
                Job& j = jobs[i];
                if (j.state != 0) continue;
                if (JobStale(j.seq, 0, callsDone, delay, anyShown, lastShown) || j.seq + 6 < callsDone) {
                    j.state = 3;
                    continue;
                }
                if (n < 64) {
                    seqs[n] = j.seq;
                    idx[n++] = (int)i;
                }
            }
            if (!n) break;
            const int pick = PickJob(seqs, n, callsDone, delay, sinceCall, period, needEstimate);
            Job& j = jobs[(size_t)idx[pick]];
            j.state = 1;
            j.finish = t + needUs(j.seq);
            freeAt[(size_t)w] = j.finish;
        }
    }
    return r;
}

static void TestModel() {
    const int64_t period = 33000;
    {   // keeps up: 12 ms a picture, three workers, delay 3
        const ModelResult r = RunModel(300, 3, 3, period, [](int64_t) { return (int64_t)12000; }, 14000);
        int wrong = 0;
        for (size_t c = 4; c < r.shown.size(); c++) wrong += r.shown[c] != (long)c - 3;
        CHECK(wrong == 0 && r.repeatsAfterFill == 0 && r.backSteps == 0 && r.early == 0, "keeps up: %d calls off the due frame, %d repeats", wrong, r.repeatsAfterFill);
    }
    {   // every 20th picture takes five calls (a card read in the way, a busy scene): delay 3 absorbs it (it has 3 calls + the look-ahead), delay 0 repeats
        auto need = [](int64_t k) { return k % 20 == 7 ? (int64_t)140000 : (int64_t)12000; };
        const ModelResult r = RunModel(300, 5, 3, period, need, 14000);
        int wrong = 0;
        for (size_t c = 6; c < r.shown.size(); c++) wrong += r.shown[c] != (long)c - 5;
        CHECK(wrong == 0 && r.repeatsAfterFill == 0, "slow pictures under delay 5: %d calls off the due frame, %d repeats", wrong, r.repeatsAfterFill);
        const ModelResult r1 = RunModel(300, 1, 3, period, need, 14000);
        CHECK(r1.repeatsAfterFill > 0 && r1.backSteps == 0 && r1.early == 0, "slow pictures under delay 1 must repeat (that is what a longer delay is for): %d repeats", r1.repeatsAfterFill);
    }
    {   // overloaded: one worker, 66 ms a picture (the calls come every 33)
        const ModelResult r = RunModel(300, 3, 1, period, [](int64_t) { return (int64_t)66000; }, 66000);
        CHECK(r.backSteps == 0 && r.early == 0, "overloaded: %d steps back, %d early", r.backSteps, r.early);
        CHECK(r.fresh >= 120 && r.longestGap <= 3, "overloaded: %d new pictures of 300 calls, longest run without one %d", r.fresh, r.longestGap);
        long worstLag = 0;
        for (size_t c = 8; c < r.shown.size(); c++) worstLag = std::max(worstLag, (long)c - r.shown[c]);
        CHECK(worstLag <= 3 + 3, "overloaded: a picture %ld calls old was shown (the movie drifts from its sound)", worstLag);
    }
}

// ---- 3. the real lanes ------------------------------------------------------------------------------------------------------
// A 16x16 THP frame of one flat grey: quantisation 8 (the DC value is the level above 128), a DC table of two codes and an AC table of one.
static std::vector<uint8_t> FlatFrame(int level /* 64..127 */, int width = 16) {
    std::vector<uint8_t> f = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00};
    f.insert(f.end(), 64, 8);
    const uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, (uint8_t)width, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00};
    f.insert(f.end(), sof, sof + sizeof(sof));
    const uint8_t dhtDc[] = {0xFF, 0xC4, 0x00, 0x15, 0x00, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x07};
    f.insert(f.end(), dhtDc, dhtDc + sizeof(dhtDc));
    const uint8_t dhtAc[] = {0xFF, 0xC4, 0x00, 0x14, 0x10, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00};
    f.insert(f.end(), dhtAc, dhtAc + sizeof(dhtAc));
    const uint8_t sos[] = {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x3F, 0x00};
    f.insert(f.end(), sos, sos + sizeof(sos));
    const uint32_t bits = (2u << 8 | (uint32_t)level << 1) << 22;
    f.push_back((uint8_t)(bits >> 24));
    f.push_back((uint8_t)(bits >> 16));
    f.push_back((uint8_t)(bits >> 8));
    f.push_back((uint8_t)bits);
    // The decoder checks its reads against the component's size (a real component is far larger than one macroblock): zero bits after the
    // macroblock stand for the rest of a picture; the end-of-image marker closes the component, as the codec bridge wants.
    f.insert(f.end(), 400, 0);
    f.push_back(0xFF);
    f.push_back(0xD9);
    return f;
}

static double NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void SleepMs(double ms) {
    if (ms > 0) np_sleep_ns((uint64_t)(ms * 1e6));
}

struct Run {
    std::vector<int> shown;      // per call: the frame whose picture is in the planes after the call (-1 none yet)
    std::vector<int> ret;
    std::vector<double> callMs;  // how long each call took
    std::string log;
    int frames = 0;
};

static std::string ReadFile(const char* path) {
    std::string s;
    if (FILE* f = fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}
static int Count(const std::string& s, const char* what) {
    int n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
    return n;
}
// the number after `key` in the first line containing `line`
static long Field(const std::string& s, const char* line, const char* key) {
    const size_t l = s.find(line);
    if (l == std::string::npos) return -1;
    const size_t k = s.find(key, l);
    if (k == std::string::npos) return -1;
    return atol(s.c_str() + k + strlen(key));
}

// Plays `frames` frames the way the player does: a ring of 10 read buffers, the look-ahead of 4 frames queued after each call, a call every
// `periodMs`. `prefetch`: whether the look-ahead is made at all (the never-queued scenario makes it only for frame 0).
static Run Play(int frames, double periodMs, bool prefetch, bool movieEnd = true, int passes = 1, int width = 16) {
    Run run;
    run.frames = frames;
    std::vector<std::vector<uint8_t>> ring(10);
    std::vector<size_t> sizes(frames);
    std::vector<uint8_t> frameData[128];
    for (int k = 0; k < frames; ++k) {
        frameData[k] = FlatFrame(64 + k, width);
        sizes[k] = frameData[k].size();
        frameData[k].resize(frameData[k].size() + 64, 0);
    }
    // reference: the level each frame decodes to
    std::vector<int> level(frames);
    for (int k = 0; k < frames; ++k) {
        std::vector<uint8_t> y(width * 16), u(width * 4), v(width * 4);
        if (THPVideoDecode(frameData[k].data(), y.data(), u.data(), v.data(), nullptr) != 0) { printf("  (the test frame %d does not decode)\n", k); ++g_fail; return run; }
        level[k] = y[0];
    }
    auto frameOf = [&](int lv) { for (int k = 0; k < frames; ++k) if (level[k] == lv) return k; return -2; };
    // the ring buffers must stay put while lanes copy from them: allocate once, then only overwrite
    for (int i = 0; i < 10; ++i) ring[i].resize(frameData[0].size() + 64, 0);
    auto fill = [&](int k) -> uint8_t* {
        std::vector<uint8_t>& b = ring[k % 10];
        memcpy(b.data(), frameData[k].data(), frameData[k].size());
        return b.data();
    };
    std::vector<uint8_t> y(width * 16, 0), u(width * 4, 0), v(width * 4, 0);
    // the player offers the next four frames of its ring after every decode (NpThpPrefetchNextFrames); one already queued is not queued again
    auto lookAhead = [&](int from) {
        for (int g = from; g < from + 4 && (g < frames || passes > 1); ++g) {
            const int f = g % frames;   // (a looping movie: the ring offers the first frames again)
            NpThpPrefetch(fill(f), (unsigned)sizes[f], f, width, 16);
        }
    };
    if (prefetch) lookAhead(0);
    else NpThpPrefetch(fill(0), (unsigned)sizes[0], 0, width, 16);
    double next = NowMs();
    for (int c = 0; c < frames * passes; ++c) {
        const int k = c % frames;
        SleepMs(next - NowMs());
        next += periodMs;
        // (the buffer of frame k is in the ring since its prefetch; refresh it: the player's read buffer holds it until it is consumed)
        uint8_t* p = fill(k);
        const double t0 = NowMs();
        const int r = NpThpVideoDecode(p, y.data(), u.data(), v.data(), nullptr, k, width, 16, (unsigned)sizes[k]);
        run.callMs.push_back(NowMs() - t0);
        run.ret.push_back(r);
        run.shown.push_back(frameOf(y[0]));
        if (prefetch) lookAhead(c + 1);
    }
    if (movieEnd) NpThpReset();
    return run;
}

// "nothing changes while the lanes keep up": the recovery and watchdog counters of the pace line are all 0
static void CheckQuiet(const std::string& log) {
    CHECK(Field(log, "[thp-pace]", "recovered on the game thread ") == 0, "recovered: %s", log.c_str());
    CHECK(Field(log, "[thp-pace]", "first picture timeouts ") == 0, "first picture timeouts");
    CHECK(Field(log, "[thp-pace]", "lanes given up ") == 0, "lanes given up");
    CHECK(Field(log, "[thp-pace]", "codec lock timeouts ") == 0, "codec lock timeouts");
}

static const char* kLog = "/tmp/thp_delay_scenario.log";

// the longest run of calls without a new picture, from the second picture on
static int LongestFreeze(const Run& r, int from = 1) {
    int best = 0, cur = 0, last = -2;
    for (size_t k = 0; k < r.shown.size(); ++k) {
        if ((int)k < from) { last = r.shown[k]; continue; }
        if (r.shown[k] == last) { if (++cur > best) best = cur; } else { cur = 0; last = r.shown[k]; }
    }
    return best;
}
static int Repeats(const Run& r, int after) {
    int n = 0;
    for (size_t k = (size_t)after; k < r.shown.size(); ++k) n += r.shown[k] == r.shown[k - 1];
    return n;
}
static int Backs(const Run& r) {
    int n = 0;
    for (size_t k = 1; k < r.shown.size(); ++k) n += r.shown[k] < r.shown[k - 1];
    return n;
}

static void Setup(const char* name, const char* delay, const char* recover, const char* extra1 = nullptr, const char* v1 = nullptr, const char* extra2 = nullptr, const char* v2 = nullptr) {
    freopen(kLog, "w", stderr);
    setenv("MSC_THP_HWJPEG", "fake", 1);
    setenv("MSC_THP_NOWAIT", "1", 1);
    setenv("MSC_THP_HWJPEG_VERIFY", "0", 1);
    if (delay) setenv("MSC_THP_DELAY", delay, 1);
    if (recover) setenv("MSC_THP_RECOVER", recover, 1);
    if (extra1) setenv(extra1, v1, 1);
    if (extra2) setenv(extra2, v2, 1);
    THPInit();
    NpThpNoteName(name);
}
static std::string Finish() {
    fflush(stderr);
    return ReadFile(kLog);
}

static int Scenario(const std::string& name) {
    if (name == "d0/fast") {
        Setup("d0fast", nullptr, nullptr);
        const Run r = Play(40, 33, true);
        const std::string log = Finish();
        int ahead = 0, offTime = 0;
        for (size_t k = 0; k < r.shown.size(); ++k) { ahead += r.shown[k] > (int)k; offTime += k >= 6 && r.shown[k] != (int)k; }
        CHECK(Backs(r) == 0 && ahead == 0, "back %d ahead %d", Backs(r), ahead);
        CHECK(offTime <= 1, "delay 0, nothing slow: %d calls (after the start) off the asked frame", offTime);
        CHECK(Count(log, "[thp-pace]") == 1 && Field(log, "[thp-pace]", "delay ") == 0, "one pace line");
        CheckQuiet(log);
        return g_fail;
    }
    if (name == "d2/fast") {
        Setup("d2fast", "2", nullptr);
        const Run r = Play(40, 33, true);
        const std::string log = Finish();
        int wrong = 0;
        for (size_t k = 0; k < r.shown.size(); ++k) wrong += r.shown[k] != (int)std::max<long>(0, (long)k - 2);
        CHECK(wrong == 0, "delay 2: %d calls off the due frame (k - 2)", wrong);
        if (wrong) { for (size_t k = 0; k < r.shown.size(); ++k) printf(" %d", r.shown[k]); printf("\n"); }
        CHECK(Field(log, "[thp-pace]", "delay ") == 2 && Field(log, "[thp-pace]", "repeated ") == 0, "pace line: %s", log.c_str());
        return g_fail;
    }
    if (name == "d0/slow" || name == "d3/slow") {
        const bool d3 = name == "d3/slow";
        Setup(d3 ? "d3slow" : "d0slow", d3 ? "3" : nullptr, nullptr, "MSC_THP_HWJPEG_FAKE_SLOW_EVERY", "9", "MSC_THP_HWJPEG_FAKE_SLOW_MS", "140");
        if (!d3) setenv("MSC_THP_DEPTH", "1", 1);   // one lane: a 140 ms call is then nobody's lock wait (with two, the other lane leaves the lock after 60 ms: that is the lock bound)
        const Run r = Play(48, 33, true);
        const std::string log = Finish();
        const int rep = Repeats(r, 6);
        CHECK(Backs(r) == 0, "back %d", Backs(r));
        if (d3) {
            int wrong = 0;
            for (size_t k = 3; k < r.shown.size(); ++k) wrong += r.shown[k] != (int)k - 3;
            CHECK(wrong == 0 && rep == 0, "delay 3 with slow pictures: %d calls off the due frame, %d repeats", wrong, rep);
        } else {
            CHECK(rep >= 2, "delay 0 with slow pictures should repeat pictures (the scenario would prove nothing otherwise): %d", rep);
            CheckQuiet(log);   // repeats are not a freeze: no recovery, no watchdog, no timeout
        }
        printf("  %s: %d repeats\n", name.c_str(), rep);
        return g_fail;
    }
    if (name == "d3/over") {
        Setup("d3over", "3", "0", "MSC_THP_HWJPEG_FAKE_DELAY_MS", "60");
        const Run r = Play(48, 33, true);
        Finish();
        int early = 0;
        for (size_t k = 0; k < r.shown.size(); ++k) early += r.shown[k] > std::max<long>(0, (long)k - 3);
        CHECK(Backs(r) == 0 && early == 0, "overloaded: back %d early %d", Backs(r), early);
        CHECK(LongestFreeze(r, 6) <= 7, "overloaded: freeze of %d calls", LongestFreeze(r, 6));
        CHECK(r.shown.back() >= 20, "overloaded: the picture advanced to frame %d only", r.shown.back());
        return g_fail;
    }
    if (name == "hang/off" || name == "hang/on") {
        const bool on = name == "hang/on";
        Setup(on ? "hangon" : "hangoff", nullptr, on ? nullptr : "0", "MSC_THP_HWJPEG_FAKE_HANG_AT", "5", "MSC_THP_HWJPEG_FAKE_HANG_MS", "1500");
        setenv("MSC_THP_HWSTUCK_MS", "150", 1);
        const Run r = Play(60, 33, true);
        const std::string log = Finish();
        const int freeze = LongestFreeze(r, 3);
        printf("  %s: longest freeze %d calls (%.0f ms)\n", name.c_str(), freeze, freeze * 33.0);
        if (on) {
            CHECK(freeze <= 12, "the picture was frozen for %d calls", freeze);
            CHECK(Count(log, "has not returned for") == 1, "the stuck codec call is reported once");
            CHECK(Field(log, "[thp-pace]", "lanes given up ") == 1, "the lane given up is counted");
            CHECK(Backs(r) == 0, "back %d", Backs(r));
        } else {
            CHECK(freeze >= 30, "without recovery the hang freezes the picture (that is the report): only %d calls", freeze);
        }
        return g_fail;
    }
    if (name == "never/off" || name == "never/on") {
        const bool on = name == "never/on";
        Setup(on ? "neveron" : "neveroff", nullptr, on ? nullptr : "0");
        const Run r = Play(40, 33, false);
        const std::string log = Finish();
        const int freeze = LongestFreeze(r, 1);
        printf("  %s: longest freeze %d calls\n", name.c_str(), freeze);
        if (on) {
            CHECK(freeze <= 9, "frames never queued: the picture was frozen for %d calls", freeze);
            CHECK(LongestFreeze(r, 14) <= 3, "after the first recovery the threshold is 2 calls until a lane delivers: freeze %d", LongestFreeze(r, 14));
            CHECK(Field(log, "[thp-pace]", "recovered on the game thread ") >= 3, "the recoveries are counted: %s", log.c_str());
            CHECK(Backs(r) == 0, "back %d", Backs(r));
            for (int ret : r.ret) CHECK(ret == 0, "a call returned %d", ret);
        } else {
            CHECK(freeze >= 30, "frames never queued and no recovery: the picture freezes (%d calls)", freeze);
        }
        return g_fail;
    }
    if (name == "first/on" || name == "first/off") {
        const bool on = name == "first/on";
        Setup(on ? "firston" : "firstoff", nullptr, on ? nullptr : "0", "MSC_THP_HWJPEG_FAKE_HANG_AT", "1", "MSC_THP_HWJPEG_FAKE_HANG_MS", "2500");
        setenv("MSC_THP_DEPTH", "1", 1);   // one lane: the one that has frame 0 is the one that hangs
        const Run r = Play(6, 33, true, false);
        const std::string log = Finish();
        printf("  %s: the first call took %.0f ms\n", name.c_str(), r.callMs.empty() ? -1.0 : r.callMs[0]);
        CHECK(r.shown.size() == 6 && r.shown[0] == 0 && r.ret[0] == 0, "the first picture is frame 0");
        if (on) {
            CHECK(r.callMs[0] < 1100.0, "the first call waited %.0f ms for a lane that hangs", r.callMs[0]);
            CHECK(Count(log, "did not finish the first picture") == 1, "logged");
        } else {
            CHECK(r.callMs[0] >= 2000.0, "old behaviour: the first call waits the hang out (%.0f ms)", r.callMs[0]);
        }
        return g_fail;
    }
    if (name == "lock/on") {
        Setup("lockon", nullptr, nullptr, "MSC_THP_HWJPEG_FAKE_HANG_AT", "5", "MSC_THP_HWJPEG_FAKE_HANG_MS", "1000");
        setenv("MSC_THP_HWSTUCK_MS", "5000", 1);
        const Run r = Play(50, 33, true);
        const std::string log = Finish();
        const long to = Field(log, "[thp-pace]", "codec lock timeouts ");
        printf("  lock/on: lock timeouts %ld, longest freeze %d\n", to, LongestFreeze(r, 3));
        CHECK(to >= 1, "the lane behind the stuck call must leave the lock loop: %ld timeouts", to);
        CHECK(Field(log, "[thp-pace]", "lanes given up ") == 0, "no watchdog here");
        CHECK(LongestFreeze(r, 3) <= 12 && Backs(r) == 0, "freeze %d back %d", LongestFreeze(r, 3), Backs(r));
        return g_fail;
    }
    if (name == "first/slow") {
        Setup("firstslow", nullptr, nullptr, "MSC_THP_HWJPEG_FAKE_DELAY_MS", "150");
        setenv("MSC_THP_DEPTH", "1", 1);
        const Run r = Play(4, 33, true, false);
        const std::string log = Finish();
        printf("  first/slow: the first call took %.0f ms\n", r.callMs.empty() ? -1.0 : r.callMs[0]);
        CHECK(r.shown.size() == 4 && r.shown[0] == 0 && r.ret[0] == 0, "first picture");
        CHECK(r.callMs[0] >= 100.0, "a lane that needs 150 ms must be waited for: the call took %.0f ms", r.callMs[0]);
        CHECK(Count(log, "did not finish the first picture") == 0, "no timeout");
        return g_fail;
    }
    if (name == "skew") {
        Setup("skew", nullptr, nullptr, "MSC_THP_HWJPEG_FAKE_HANG_AT", "5", "MSC_THP_HWJPEG_FAKE_HANG_MS", "400");
        setenv("MSC_THP_HWSTUCK_MS", "700", 1);
        std::thread jump([] { SleepMs(300); g_clockSkewUs = 60 * 1000000ll; });   // inside the 400 ms call
        const Run r = Play(40, 33, true);
        jump.join();
        const std::string log = Finish();
        CHECK(Count(log, "has not returned") == 0, "a clock jump is not a hung codec");
        CHECK(!g_hw.dead && g_lanes.st.lanesLost == 0, "the codec must stay on");
        CHECK(Backs(r) == 0, "back");
        return g_fail;
    }
    if (name == "loop/on") {
        Setup("loop", nullptr, nullptr, "MSC_THP_HWJPEG_FAKE_HANG_AT", "5", "MSC_THP_HWJPEG_FAKE_HANG_MS", "1500");
        setenv("MSC_THP_HWSTUCK_MS", "150", 1);
        const Run r = Play(30, 33, true, true, 3);
        const std::string log = Finish();
        int back = 0, freeze = 0, cur = 0;
        for (size_t c = 1; c < r.shown.size(); ++c) {
            if (c % 30 == 0) { cur = 0; continue; }   // a pass starts over
            back += r.shown[c] < r.shown[c - 1];
            if (r.shown[c] == r.shown[c - 1]) { if (++cur > freeze) freeze = cur; } else cur = 0;
        }
        CHECK(back == 0 && freeze <= 12, "loop: back %d, freeze %d", back, freeze);
        CHECK(r.shown[30] == 0 && r.shown[60] == 0, "each pass starts at frame 0: %d %d", r.shown[30], r.shown[60]);
        CHECK(r.shown[59] >= 20 && r.shown[89] >= 20, "each pass advances to its end: %d %d", r.shown[59], r.shown[89]);
        return g_fail;
    }
    if (name == "reset/size") {
        Setup("resetsize", nullptr, nullptr, "MSC_THP_HWJPEG_FAKE_HANG_AT", "4", "MSC_THP_HWJPEG_FAKE_HANG_MS", "1200");
        setenv("MSC_THP_HWSTUCK_MS", "5000", 1);
        const Run a = Play(10, 33, true);                     // 16x16; the call stuck from frame ~3 is still inside the codec when the movie ends
        NpThpNoteName("resetsize2");
        const Run b = Play(20, 33, true, true, 1, 32);        // 32x16
        const std::string log = Finish();
        int unknown = 0;
        for (size_t k = 3; k < b.shown.size(); ++k) unknown += b.shown[k] < 0;
        CHECK(a.shown.size() == 10 && b.shown.size() == 20, "both movies played");
        CHECK(unknown == 0 && Backs(b) == 0 && b.shown.back() >= 10, "second movie: %d unknown pictures, back %d, last %d", unknown, Backs(b), b.shown.back());
        CHECK(LongestFreeze(b, 3) <= 12, "second movie freeze %d", LongestFreeze(b, 3));
        return g_fail;
    }
    if (name == "heal") {
        Setup("heal", nullptr, nullptr, "MSC_THP_HWJPEG_FAKE_HANG_AT", "3", "MSC_THP_HWJPEG_FAKE_HANG_MS", "400");
        setenv("MSC_THP_HWSTUCK_MS", "200", 1);
        const Run r = Play(30, 33, true, false);              // the watchdog fires; the call returns before the end
        CHECK(g_hw.dead && g_hw.watchdogDead, "the watchdog fired");
        NpThpReset();
        const std::string log = Finish();
        CHECK(!g_hw.dead, "a false alarm is undone when the call returned");
        CHECK(Count(log, "is used again") == 1, "logged");
        (void)r;
        return g_fail;
    }
    if (name == "swwatch") {
        freopen(kLog, "w", stderr);   // software decoding only (no codec): the stamp is set and cleared around every job
        setenv("MSC_THP_HWJPEG", "0", 1);
        setenv("MSC_THP_NOWAIT", "1", 1);
        setenv("MSC_THP_SWSTUCK_MS", "50", 1);
        THPInit();
        NpThpNoteName("swwatch");
        const Run r = Play(24, 33, true);
        int unknown = 0;
        for (size_t k = 4; k < r.shown.size(); ++k) unknown += r.shown[k] < 0;
        CHECK(unknown == 0, "software lanes still deliver pictures: %d unknown", unknown);
        for (int i = 0; i < kMaxSlots; ++i) CHECK(g_lanes.slot[i].swSinceMs == 0, "slot %d: the software stamp is cleared after a job", i);
        CHECK(g_lanes.st.swStuck == 0, "nothing stuck in a healthy run: %llu", g_lanes.st.swStuck);
        const uint32_t now = (uint32_t)(NowUs() / 1000u);
        Slot& sl = g_lanes.slot[0];
        __atomic_store_n(&sl.swSinceMs, (now - 300u) | 1u, __ATOMIC_RELEASE);
        SwWatch();
        SwWatch();                                            // the same episode is counted once
        CHECK(g_lanes.st.swStuck == 1, "one episode, counted once: %llu", g_lanes.st.swStuck);
        __atomic_store_n(&sl.swSinceMs, (now - 5u) | 1u, __ATOMIC_RELEASE);
        SwWatch();                                            // a short one is not
        CHECK(g_lanes.st.swStuck == 1, "a software decode of 5 ms is not stuck: %llu", g_lanes.st.swStuck);
        __atomic_store_n(&sl.swSinceMs, (now - 400u) | 1u, __ATOMIC_RELEASE);
        SwWatch();                                            // another episode
        CHECK(g_lanes.st.swStuck == 2, "second episode: %llu", g_lanes.st.swStuck);
        const int stateBefore = sl.state;
        __atomic_store_n(&sl.swSinceMs, 0u, __ATOMIC_RELEASE);
        CHECK(sl.state == stateBefore && !g_hw.dead, "counting changes nothing else (no lane replaced, no codec off)");
        const std::string log = Finish();
        CHECK(Count(log, "inside the software decoder") == 2, "two log lines, got %d: %s", Count(log, "inside the software decoder"), log.c_str());
        return g_fail;
    }
    if (name == "pace") {
        Setup("pace", "0", nullptr);
        const Run r = Play(30, 33, true);
        const std::string log = Finish();
        (void)r;
        CHECK(Count(log, "[thp-pace]") == 1, "one pace line per movie, got %d", Count(log, "[thp-pace]"));
        CHECK(Field(log, "[thp-pace]", "calls ") == 30, "calls: %ld", Field(log, "[thp-pace]", "calls "));
        CHECK(Count(log, "gaps late") == 1 && Count(log, "doubled") == 1 && Count(log, "longest gap") == 1 && Count(log, "a second") == 1, "the pace fields: %s", log.c_str());
        // a second movie writes its own line
        Setup("pace2", "0", nullptr);
        NpThpReset();
        return g_fail;
    }
    printf("unknown scenario %s\n", name.c_str());
    return 1;
}

int main(int argc, char** argv) {
    if (argc > 2 && !strcmp(argv[1], "--child")) return Scenario(argv[2]) ? 1 : 0;
    const bool only = argc > 1;
    if (!only) {
        TestPure();
        TestModel();
        printf("pure functions and model: %s\n", g_fail ? "FAILED" : "ok");
    }
    static const char* all[] = {"d0/fast", "d2/fast", "d0/slow", "d3/slow", "d3/over", "hang/off", "hang/on", "never/off", "never/on", "first/off", "first/on", "first/slow", "lock/on", "skew", "loop/on", "reset/size", "heal", "swwatch", "pace"};
    int failed = g_fail;
    for (const char* sc : all) {
        if (only && strcmp(argv[1], sc)) continue;
        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            const int rc = Scenario(sc) ? 1 : 0;
            fflush(stdout);
            _exit(rc);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        printf("scenario %-10s %s\n", sc, ok ? "ok" : "FAILED");
        if (!ok) ++failed;
    }
    printf("test_thp_delay: %s\n", failed ? "FAILED" : "all checks passed");
    return failed ? 1 : 0;
}
