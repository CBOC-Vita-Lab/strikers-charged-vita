// np_smooth.cpp (wave 9 helper "smooth"): pictures drawn for the moment they are SHOWN.
//  1. MSC_SMOOTH_CLOCK (default 1 in the Vita plan, 0 in the code): under a held cadence in live single-player play, the game time a frame advances by is the predicted
//     show-to-show time of its picture (np_show_clock.h), not its start-to-start time. Fed to np_fps.cpp's SlipFrame (the debt clock caps and owes as before) and to every
//     gameplay task through NpSlipDelta. Only while the display has held the newest 8 pictures exactly one period each; otherwise the frame keeps its own time.
//  2. "[smooth]" (MSC_SMOOTH_LOG, default 1 in the Vita plan): one line per 300 pictures of live play, from the display-queue thread: per picture the game time each layer
//     represents against the refreshes it was shown for (players/ball/items = the blended snapshot, live = the newest step, camera = its aim's snapshot), the blend factor's
//     histogram, and how evenly a running player, the ball and the camera move per shown refresh.
//  3. MSC_SMOOTH_TRACE=<file> (host): one line per picture from the game thread (blend, times, positions, a bone of each player) for tools/fps/smooth_trace.py.
// Nothing here is read by the simulation; LAN and online sessions are never touched (NpLanActive).
#include "np_show_clock.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int NpFpsDisplayInterval(void);
uint32_t NpVI_GetRetraceCount(void) __attribute__((weak));
}
bool NpLanActive() __attribute__((weak));

namespace {
const double kVblankMs = 1001.0 / 60.0;

int EnvInt(const char* n, int def) { const char* e = getenv(n); return (e && *e) ? atoi(e) : def; }

// ---- display side -> game thread (atomics: written by the display-queue thread, read at the frame start)
std::atomic<uint64_t> g_fb{0};          // (tag & 0xffffffff) << 32 | shown vblank
std::atomic<int> g_L{-1};               // learned latency to ready (vblanks), -1 unknown
std::atomic<unsigned> g_steady{0};      // pictures in a row held exactly the period

// ---- the clock (game thread)
npfps::ShowClock g_clock;
int g_clockOn = -1;
bool g_frameActive = false;
uint32_t g_frameStamp = 0;
uint64_t g_clockFrames = 0, g_clockSmoothed = 0;

// ---- per-picture record (game thread writes, display thread reads by tag)
struct Rec {
    std::atomic<uint64_t> tag{0};
    float blend, rep, live, camTime, real, adv;
    float cam[3], ball[3], pos[10][3];
    uint16_t moving;   // bit i: character i has a position
    uint8_t live_play, blended, stuck, steps;
};
Rec g_ring[64];
struct Cur {
    float blend = -1, rep = 0, live = 0, camTime = 0, real = 0, adv = 0;
    float cam[3] = {0, 0, 0}, ball[3] = {0, 0, 0}, pos[10][3] = {};
    uint16_t moving = 0;
    uint8_t live_play = 0, blended = 0, stuck = 0, steps = 0;
    bool haveDraw = false;
} g_cur;
int g_logOn = -1;
FILE* g_trace = nullptr;
bool g_traceTried = false;
unsigned long long g_traceFrame = 0;

// ---- display-thread statistics
struct Hist {
    static const int kBins = 40;   // |error| in 0.1 refresh bins
    unsigned b[kBins + 1] = {};
    unsigned n = 0;
    double sum = 0, worst = 0;
    void Add(double e) { e = std::fabs(e); int i = (int)(e * 10.0 + 0.5); if (i > kBins) i = kBins; ++b[i]; ++n; sum += e; if (e > worst) worst = e; }
    double Mean() const { return n ? sum / n : 0; }
    double Pct(double p) const { if (!n) return 0; const unsigned want = (unsigned)(p * n + 0.999); unsigned a = 0; for (int i = 0; i <= kBins; ++i) { a += b[i]; if (a >= want) return i / 10.0; } return kBins / 10.0; }
    double Over(double x) const { unsigned k = 0; for (int i = 0; i <= kBins; ++i) if (i / 10.0 > x + 1e-9) k += b[i]; return n ? 100.0 * k / n : 0; }
};
struct Stats {
    Hist snap, live, cam, run, ball, pan;
    unsigned blendBins[5] = {}, pictures = 0, unblended = 0, stuck = 0, holds[5] = {};
    void Clear() { *this = Stats(); }
} g_st;
uint64_t g_prevTag = 0;
uint32_t g_prevShown = 0;
Rec g_prev;   // copy of the previous shown picture's record
bool g_havePrev = false;
float g_prevRun[10] = {}, g_prevBall = -1, g_prevPan = -1;
npfps::LatencyLearner g_learn;
uint32_t (*const g_hostVc)(void) = &NpVI_GetRetraceCount;

bool LogOn() { if (g_logOn < 0) g_logOn = EnvInt("MSC_SMOOTH_LOG", 0) != 0 ? 1 : 0; return g_logOn == 1; }

double Dist(const float* a, const float* b) { const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return std::sqrt(x * x + y * y + z * z); }

void Line() {
    Stats& s = g_st;
    const unsigned bn = s.blendBins[0] + s.blendBins[1] + s.blendBins[2] + s.blendBins[3] + s.blendBins[4];
    const double pb = bn ? 100.0 / bn : 0;
    std::fprintf(stderr, "[smooth] %u pictures of live play: held 1/2/3/4+ %u/%u/%u/%u; motion minus shown, refreshes a picture (mean / p95 / worst / share over 0.5): players+ball %.2f / %.1f / %.1f / %.1f %%, "
                 "newest step %.2f / %.1f / %.1f / %.1f %%, camera aim %.2f / %.1f / %.1f / %.1f %%; blend 0-.2/-.4/-.6/-.8/-1 %.0f/%.0f/%.0f/%.0f/%.0f %%, %u not blended, %u stuck at 1; "
                 "speed on screen per shown refresh against the picture before (|ratio-1|, median / p95): running players %.2f / %.2f, ball %.2f / %.2f, camera pan %.2f / %.2f; "
                 "clock %s: %llu frames held, %llu given a show time other than their own, %llu agreed, %llu not steady, %llu re-anchored, L %d, steady run %u\n",
                 s.pictures, s.holds[1], s.holds[2], s.holds[3], s.holds[4], s.snap.Mean(), s.snap.Pct(0.95), s.snap.worst, s.snap.Over(0.5), s.live.Mean(), s.live.Pct(0.95),
                 s.live.worst, s.live.Over(0.5), s.cam.Mean(), s.cam.Pct(0.95), s.cam.worst, s.cam.Over(0.5), s.blendBins[0] * pb, s.blendBins[1] * pb, s.blendBins[2] * pb,
                 s.blendBins[3] * pb, s.blendBins[4] * pb, s.unblended, s.stuck, s.run.Pct(0.5), s.run.Pct(0.95), s.ball.Pct(0.5), s.ball.Pct(0.95), s.pan.Pct(0.5), s.pan.Pct(0.95),
                 g_clockOn == 1 ? "on" : "off", (unsigned long long)g_clockFrames, (unsigned long long)g_clock.Changed(), (unsigned long long)g_clock.Agreed(),
                 (unsigned long long)g_clock.Unsteady(), (unsigned long long)g_clock.Reanchors(), g_L.load(std::memory_order_relaxed), g_steady.load(std::memory_order_relaxed));
    s.Clear();
}

void TraceOpen() {
    if (g_traceTried) return;
    g_traceTried = true;
    const char* f = getenv("MSC_SMOOTH_TRACE");
    if (f && *f) {
        g_trace = std::fopen(f, "w");
        if (g_trace) std::fprintf(g_trace, "# frame live blended stuck steps real_ms adv_ms blend rep live camTime cam.xyz ball.xyz pos0..9.xyz bone0..9.xyz\n");
    }
}
float g_bone[10][3] = {};
uint16_t g_boneHave = 0;
} // namespace

extern "C" {

void NpSmoothShown(uint64_t tag, uint32_t shown, uint32_t natural, int naturalValid);

// ---- the clock (np_fps.cpp NpFpsBeginFrame): returns the game seconds this frame advances by, or -1 (the frame's own time)
double NpSmoothFrameBegin(double realSeconds, int interval, int haveVcount, uint32_t vcount, int livePlay)
{
    if (g_clockOn < 0) {
        g_clockOn = EnvInt("MSC_SMOOTH_CLOCK", 0) != 0 ? 1 : 0;
        g_clock.SetSteady((unsigned)EnvInt("MSC_SMOOTH_STEADY", (int)npfps::ShowClock::kSteady));
        if (g_clockOn) std::fprintf(stderr, "[smooth] clock on (MSC_SMOOTH_CLOCK=1): under a held cadence each picture shows the game at its predicted show time (whole refreshes, after %d evenly held pictures)\n", EnvInt("MSC_SMOOTH_STEADY", (int)npfps::ShowClock::kSteady));
    }
    g_cur.live_play = livePlay ? 1 : 0;
    g_cur.real = (float)(realSeconds * 1000.0);
    g_cur.adv = (float)(realSeconds * 1000.0);
    g_frameActive = false;
    const bool active = g_clockOn == 1 && livePlay && interval > 0 && haveVcount && !(&NpLanActive != 0 && NpLanActive());
    if (active) ++g_clockFrames;
    const uint64_t fb = g_fb.load(std::memory_order_acquire);
    const int L = g_L.load(std::memory_order_relaxed);
    const int a = g_clock.Begin(active, vcount, interval, L >= 0 ? L : 3, g_steady.load(std::memory_order_relaxed), fb != 0, fb >> 32, (uint32_t)(fb & 0xFFFFFFFFu));
    g_frameActive = active;
    g_frameStamp = vcount;
    if (a < 0) return -1.0;
    ++g_clockSmoothed;
    const double s = a * kVblankMs * 0.001;
    g_cur.adv = (float)(s * 1000.0);
    return s;
}

// np_fps.cpp NpFpsSwapWaitEnd: the frame was handed over with this number
void NpSmoothHanded(uint64_t tag)
{
    if (g_frameActive) g_clock.Handed(tag & 0xFFFFFFFFull);
    TraceOpen();
    const bool want = LogOn() || g_trace;
    if (want && tag && g_cur.haveDraw) {
        Rec& r = g_ring[tag % 64];
        r.tag.store(0, std::memory_order_relaxed);
        r.blend = g_cur.blend; r.rep = g_cur.rep; r.live = g_cur.live; r.camTime = g_cur.camTime; r.real = g_cur.real; r.adv = g_cur.adv;
        std::memcpy(r.cam, g_cur.cam, sizeof r.cam); std::memcpy(r.ball, g_cur.ball, sizeof r.ball); std::memcpy(r.pos, g_cur.pos, sizeof r.pos);
        r.moving = g_cur.moving; r.live_play = g_cur.live_play; r.blended = g_cur.blended; r.stuck = g_cur.stuck; r.steps = g_cur.steps;
        r.tag.store(tag, std::memory_order_release);
    }
    if (g_trace && g_cur.haveDraw) {
        std::fprintf(g_trace, "%llu %u %u %u %u %.3f %.3f %.5f %.5f %.5f %.5f %.4f %.4f %.4f %.4f %.4f %.4f", ++g_traceFrame, g_cur.live_play, g_cur.blended, g_cur.stuck, g_cur.steps,
                     g_cur.real, g_cur.adv, g_cur.blend, g_cur.rep, g_cur.live, g_cur.camTime, g_cur.cam[0], g_cur.cam[1], g_cur.cam[2], g_cur.ball[0], g_cur.ball[1], g_cur.ball[2]);
        for (int i = 0; i < 10; ++i) std::fprintf(g_trace, " %.4f %.4f %.4f", g_cur.pos[i][0], g_cur.pos[i][1], g_cur.pos[i][2]);
        for (int i = 0; i < 10; ++i) {
            if (g_boneHave & (1u << i)) std::fprintf(g_trace, " %.4f %.4f %.4f", g_bone[i][0], g_bone[i][1], g_bone[i][2]);
            else std::fprintf(g_trace, " nan nan nan");
        }
        std::fputc('\n', g_trace);
        if ((g_traceFrame & 255) == 0) std::fflush(g_trace);
    }
    g_cur.haveDraw = false;
    g_cur.steps = 0;
    g_boneHave = 0;
    // host only (MSC_SMOOTH_HOSTDISPLAY=1, no display thread): the picture is reported shown on the retrace after its hand-over, ready one refresh after its start
    static const int hostDisplay = EnvInt("MSC_SMOOTH_HOSTDISPLAY", 0);
    // (=2: shown on the retrace after its frame START: an even display that always agrees with the frame's own time, for the exactness proof of the active path)
    if (hostDisplay && tag && g_hostVc) NpSmoothShown(tag, hostDisplay == 2 ? g_frameStamp + 1 : g_hostVc() + 1, 1, 1);
}

int NpSmoothBreak(void) { static const int b = EnvInt("MSC_SMOOTH_BREAK", 0); return b; }

int NpSmoothWanted(void) { TraceOpen(); return LogOn() || g_trace ? 1 : 0; }

void NpSmoothNoteSteps(int steps) { g_cur.steps = (uint8_t)(steps > 255 ? 255 : steps); }

// ReplayManager::RenderSnapshotAt (game thread): what this picture shows. pos: 10 x 3 blended character positions (NaN = none), bone: 10 x 3 (a bone's model-space
// translation, NaN = none; trace only)
void NpSmoothNoteDraw(float blend, float rep, float live, int blended, int stuck, const float* ball, const float* pos, const float* bone)
{
    g_cur.haveDraw = true;
    g_cur.blend = blend; g_cur.rep = rep; g_cur.live = live; g_cur.blended = blended ? 1 : 0; g_cur.stuck = stuck ? 1 : 0;
    std::memcpy(g_cur.ball, ball, sizeof g_cur.ball);
    g_cur.moving = 0;
    for (int i = 0; i < 10; ++i) {
        std::memcpy(g_cur.pos[i], pos + 3 * i, sizeof g_cur.pos[i]);
        if (pos[3 * i] == pos[3 * i]) g_cur.moving |= (uint16_t)(1u << i);
        if (bone && bone[3 * i] == bone[3 * i]) { std::memcpy(g_bone[i], bone + 3 * i, sizeof g_bone[i]); g_boneHave |= (uint16_t)(1u << i); }
    }
}

// BeginFrameTask after cCameraManager::Update (game thread): the camera of this picture and the snapshot time its aim was read from (the previous picture's)
void NpSmoothNoteCamera(const float* cam, float aimTime)
{
    std::memcpy(g_cur.cam, cam, sizeof g_cur.cam);
    g_cur.camTime = aimTime;
}

// vgx_present SyncShown (display-queue thread): picture `tag` went up on vblank `shown`; natural = vblanks from its frame start to the first vblank it could be on (valid when stamped)
void NpSmoothShown(uint64_t tag, uint32_t shown, uint32_t natural, int naturalValid)
{
    if (!tag) return;
    const int period = NpFpsDisplayInterval();
    if (naturalValid) {
        g_learn.Add(natural);
        if (g_learn.Ready()) g_L.store(g_learn.Quantile(0.8), std::memory_order_relaxed);
    }
    const bool consecutive = g_havePrev && tag == g_prevTag + 1;
    if (consecutive) {
        const uint32_t hold = shown - g_prevShown;
        const unsigned run = g_steady.load(std::memory_order_relaxed);
        g_steady.store(period > 0 && hold == (uint32_t)period ? run + 1 : 0, std::memory_order_relaxed);
    } else {
        g_steady.store(0, std::memory_order_relaxed);
    }
    g_fb.store(((tag & 0xFFFFFFFFull) << 32) | shown, std::memory_order_release);
    // statistics: needs this picture's record (and the previous one's)
    Rec& r = g_ring[tag % 64];
    const bool have = LogOn() && r.tag.load(std::memory_order_acquire) == tag;
    Rec cur;
    if (have) {
        cur.blend = r.blend; cur.rep = r.rep; cur.live = r.live; cur.camTime = r.camTime; cur.real = r.real; cur.adv = r.adv;
        std::memcpy(cur.cam, r.cam, sizeof cur.cam); std::memcpy(cur.ball, r.ball, sizeof cur.ball); std::memcpy(cur.pos, r.pos, sizeof cur.pos);
        cur.moving = r.moving; cur.live_play = r.live_play; cur.blended = r.blended; cur.stuck = r.stuck; cur.steps = r.steps;
        if (r.tag.load(std::memory_order_acquire) != tag) return;   // (overwritten while copying)
    }
    if (have && consecutive && g_prev.live_play && cur.live_play) {
        Stats& s = g_st;
        const double hold = (double)(shown - g_prevShown);
        if (hold >= 1 && hold < 30) {
            ++s.pictures;
            ++s.holds[hold < 4 ? (int)hold : 4];
            s.snap.Add((cur.rep - g_prev.rep) * 1000.0 / kVblankMs - hold);
            s.live.Add((cur.live - g_prev.live) * 1000.0 / kVblankMs - hold);
            s.cam.Add((cur.camTime - g_prev.camTime) * 1000.0 / kVblankMs - hold);
            const int b = cur.blend < 0 ? 0 : (cur.blend >= 1 ? 4 : (int)(cur.blend * 5));
            ++s.blendBins[b];
            if (!cur.blended) ++s.unblended;
            if (cur.stuck) ++s.stuck;
            // speed per shown refresh, against the previous picture's (1 = the same speed on screen): players over 2 m/s, the ball over 3 m/s, the camera over 1 m/s
            const double secs = hold * kVblankMs * 0.001;
            for (int i = 0; i < 10; ++i) {
                if (!((cur.moving & g_prev.moving) & (1u << i))) { g_prevRun[i] = -1; continue; }
                const double v = Dist(cur.pos[i], g_prev.pos[i]) / secs;
                if (v > 2.0 && v < 15.0 && g_prevRun[i] > 2.0) s.run.Add(v / g_prevRun[i] - 1.0);
                g_prevRun[i] = (float)v;
            }
            const double vb = Dist(cur.ball, g_prev.ball) / secs;
            if (vb > 3.0 && vb < 60.0 && g_prevBall > 3.0) s.ball.Add(vb / g_prevBall - 1.0);
            g_prevBall = (float)vb;
            const double vc = Dist(cur.cam, g_prev.cam) / secs;
            if (vc > 1.0 && vc < 40.0 && g_prevPan > 1.0) s.pan.Add(vc / g_prevPan - 1.0);
            g_prevPan = (float)vc;
            if (s.pictures >= 300) Line();
        }
    }
    if (have) { g_prev.blend = cur.blend; g_prev.rep = cur.rep; g_prev.live = cur.live; g_prev.camTime = cur.camTime; std::memcpy(g_prev.cam, cur.cam, sizeof cur.cam);
                std::memcpy(g_prev.ball, cur.ball, sizeof cur.ball); std::memcpy(g_prev.pos, cur.pos, sizeof cur.pos); g_prev.moving = cur.moving; g_prev.live_play = cur.live_play; }
    else g_prev.live_play = 0;
    g_prevTag = tag;
    g_prevShown = shown;
    g_havePrev = true;
}

} // extern "C"
