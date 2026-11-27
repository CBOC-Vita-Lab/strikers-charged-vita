// Trace model of the dynamic-resolution ladder (renderer/src/vgx_backend_gl.cpp DynamicResolution, ordinary scenes, MSC_DYNRES_RULES=2, the Vita defaults
// MSC_VGX_MSAA=4, MSC_DYNRES_FG=1 with its crowd step, MSC_DYNRES_SEVERE=1) with and without MSC_DYNRES_LATEHOLD (vgx_dynres_late.h, the shipped header),
// and two outside policies for comparison: strikersVita's (any late frame: a fixed lower size for 2 s, then straight back) and Halo's (down after two frames
// over 92 % of the budget, up only after 1 s that fits at 80 %).
//
// The GPU is modelled as one queue: frame i is submitted at s_i, finishes at f_i = max(f_{i-1}, s_i) + g_i, is shown at the first refresh at or after f_i
// that is at least one frame interval after the previous picture; the game may be two frames ahead (s_{i+1} >= p_{i-1}). The ladder sees a frame's GPU
// time once it has finished (the console's notifications come about two frames late). A size change can cost extra GPU time on its first frame
// (--resize=ms; kept targets make it small, a new target large). The GPU time of a frame at a level is the trace's time at the game's own size times
// the ladder's own cost model (DynCost), or a different pixel share (--pix=0.5) to see what a wrong model does.
//
// Not measured: the traces are synthetic, shaped to what the consoles showed (Strikers 1.0/1.1 logs: open play ~30 fps, menus GPU-bound at ~40 ms, Mega Saves
// ~15 fps; Galaxy r31-r41: 0.7-3 % late frames in play, one drop-in held 11-12 fps for 15 s). Its numbers compare policies; they are not frame rates.
//
// --console (1.3, MSC_DYNRES_BLIND): the ladder reads the GPU time as the console gives it: only the NEWEST finished frame, and only when every scene of it was timed.
// vitaGL gives a scene no notification once its watcher is 48 scenes behind (rt.c: RT_SLOTS 64 - 16), so a frame submitted while `inflight` earlier frames are unfinished
// is untimed when (inflight + 1) x scenes > 48 (--scenes=17). Each trace frame also has a CPU cost (the busiest lane; 0.5 of the budget unless the trace says) that
// paces the submissions; MSC_DYNRES_BLIND's guard (vgx_dynres_blind.h, the shipped header) sees it. --blind-test runs the checks of run_dynres_blind_test.sh.
// --hold30=N (1.3 MSC_DYNRES_HOLD30): at 30 the mean-over-90 % step needs N measured frames over 90 % of the budget (leaky count) first.
//
// Build: clang++ -std=c++20 -O2 -I renderer/src renderer/tools/dynres_sim.cpp -o /tmp/dynres_sim ; run: /tmp/dynres_sim [--fps=30|60] [--resize=ms] [--pix=0.75] [--depth=2] [--console] [--scenes=17] [--hold30=N]
#include "vgx_dynres_late.h"
#include "vgx_dynres_blind.h"
#include "vgx_dynres_severe.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Lv { float s, p; int ms; };
// The Vita's table with MSAA=4: 4x and 2x at the game's own size, then the 1.1 ladder from the game's own size down.
const Lv kLv[] = {{1.00f, 1.0f, 4}, {1.00f, 1.0f, 2}, {1.00f, 1.0f, 1}, {1.00f, 0.75f, 1}, {1.00f, 0.5f, 1}, {0.93f, 0.5f, 1},
                  {0.86f, 0.5f, 1}, {0.78f, 0.5f, 1}, {0.70f, 0.5f, 1}, {0.62f, 0.5f, 1}, {0.56f, 0.5f, 1}, {0.50f, 0.5f, 1}};
constexpr int kNative = 2, kFloor30 = 6, kFloor60 = 7, kSevere = 8, kPivot = kNative + 2;
float g_pix = 0.75f;
bool g_console = false;   // --console: newest-only GPU times, untimed frames when the watcher is 48 scenes behind
int g_scenes = 17;
int g_hold30 = 0;
int g_depth = 2;     // frames the game may be ahead of the display (MSC_VITA_QUEUE_FIXED30=1: 2)   // the share of the cost that follows the pixels (the renderer's model says 0.75)
float CostModel(int l, float pix) {
    const Lv& v = kLv[l];
    const float m = v.ms >= 4 ? 1.9f : (v.ms >= 2 ? 1.3f : 1.0f);
    return (1.0f - pix) + pix * v.s * v.s * (0.7f + 0.3f * v.p * v.p) * m;
}
float DynCost(int l, int b) { return CostModel(l, 0.75f) * (b > 0 ? 0.98f : 1.0f); }      // what the renderer predicts
float TrueCost(int l, int b) { return CostModel(l, g_pix) * (b > 0 ? 0.98f : 1.0f); }     // what the trace's GPU pays
bool DynDown(int& l, int& b, int floorLv) {
    if (l == kPivot && b < 1) { ++b; return true; }
    if (l >= floorLv) return false;
    ++l;
    return true;
}
bool DynUp(int& l, int& b, int best) {
    if (l == kPivot && b > 0) { --b; return true; }
    if (l <= best) return false;
    --l;
    return true;
}

struct Rng {
    uint64_t s;
    double U() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<double>(s >> 11) / 9007199254740992.0; }
    double N() { const double a = U() + 1e-12, b = U(); return std::sqrt(-2.0 * std::log(a)) * std::cos(6.283185307 * b); }
};

// A trace: the GPU time of each frame at the game's own size, as a fraction of the budget.
struct Trace { std::string name; std::vector<float> f; std::vector<float> cpu; };   // cpu: the busiest CPU lane, fraction of the budget (empty: 0.5)
Trace Make(const char* name, int frames, uint64_t seed, float mean, float sigma, float pSpike, float spikeLen, float spikeLo, float spikeHi,
           std::vector<std::pair<int, int>> heavy = {}, float heavyMult = 1.0f) {
    Trace t{name, {}, {}};
    Rng r{seed * 2654435761u + 1};
    int left = 0;
    float mult = 1.0f;
    for (int i = 0; i < frames; ++i) {
        if (left == 0 && r.U() < pSpike) {
            left = 1 + static_cast<int>(-std::log(r.U() + 1e-12) * (spikeLen - 1.0f));   // a cluster: geometric length, mean spikeLen
            mult = spikeLo + static_cast<float>(r.U()) * (spikeHi - spikeLo);
        }
        float v = mean * static_cast<float>(std::exp(sigma * r.N()));
        if (left > 0) { v *= mult; --left; }
        for (auto& h : heavy) if (i >= h.first && i < h.second) v *= heavyMult;
        t.f.push_back(v);
    }
    return t;
}

enum Policy { kLadder, kLate, kSv, kHalo };
struct Cfg { Policy pol = kLadder; float holdMs = 2000; int step = 2; float at = 1.0f; int svLow = 2; const char* name = ""; bool blind = false; int hold30 = -1; };

// win*: the same within frames [w0, w1) (the --blind-test stretch); untimed: frames the console would not have timed; fed: blind pictures fed as GPU-late
struct Out { double late = 0, frames = 0, longGap = 0, changes = 0, pix = 0, worstRun = 0, winLate = 0, winFrames = 0, winPix = 0, untimed = 0, fed = 0; };

Out Run(const Trace& tr, const Cfg& c, int interval, float resizeMs, int w0 = -1, int w1 = -1) {
    const double B = (interval == 1 ? 1.0 : 2.0) * 1001.0 / 60.0, V = 1001.0 / 60.0;
    const int floor = interval == 1 ? kFloor60 : kFloor30;
    const int n = static_cast<int>(tr.f.size());
    std::vector<double> s(n), f(n), p(n), g(n);
    std::vector<int> lvOf(n), bOf(n);
    std::vector<char> untimed(n, 0);
    const int hold30 = c.hold30 >= 0 ? c.hold30 : g_hold30;
    int hot = 0;                 // (hold30's leaky count)
    uint32_t lseq = 0;           // (--console: the ladder's frame count, real and blind)
    int lastNewest = -1;
    vgx::BlindGuard guard;
    guard.on = c.blind;
#ifdef BLIND_BREAK
    guard.on = false;            // the deliberate break of run_dynres_blind_test.sh
#endif
    auto cpuOf = [&](int i) { return tr.cpu.empty() ? 0.5f : tr.cpu[i]; };
    // ladder state (DynRes)
    int level = kNative, bg = 0, over = 0, hi = 0, headroom = 0, upHold = 45, aboveWin = 0, aboveBad = 0, aboveFails = 0;
    uint32_t seqSeen = 0, ignoreUntil = 0, lastUpSeq = 0, lastDownSeq = 0, lastChange = 0, lastResize = 0, aboveLockUntil = 0, lastAboveDown = 0;
    float ema = -1;
    vgx::SevereGuard sev;
    sev.on = true;
    vgx::LateHold late;
    late.on = c.pol == kLate;
    late.holdMs = c.holdMs;
    late.maxStep = c.step;
    late.at = c.at;
    int svLeft = 0, haloHi = 0, haloRoom = 0;
    double lastMeasuredAt = 0;
    Out o;
    int next = 0;   // the next frame whose GPU time the ladder has not seen
    int run = 0;
    auto setLevel = [&](int l, int b, uint32_t present, bool force) -> bool {
        if (l == level && b == bg) return false;
        if (kLv[l].s != kLv[level].s || kLv[l].ms != kLv[level].ms) {
            if (!force && present - lastResize < 20) return false;
            lastResize = present;
        }
        lastChange = present;
        if (ema > 0) ema = ema * DynCost(l, b) / DynCost(level, bg);
        level = l;
        bg = b;
        ignoreUntil = seqSeen + 3;
        over = headroom = 0;
        o.changes += 1;
        return true;
    };
    for (int i = 0; i < n; ++i) {
        const uint32_t present = static_cast<uint32_t>(i);
        // (--console: the host's GPU queue limit instead, MSC_VITA_QUEUE_FIXED30 depth 2: after the swap of frame i-1 the render thread waits until frame i-1-depth
        // is finished on the GPU, so frames i-2 and i-1 may still be drawing when frame i's scenes are ended)
        const double gate = !g_console ? (i >= g_depth ? p[i - g_depth] : 0.0) : (i >= g_depth + 1 ? f[i - 1 - g_depth] : 0.0);
        s[i] = i == 0 ? 0.0 : std::max(s[i - 1] + std::max(B, static_cast<double>(cpuOf(i - 1)) * B), gate);
        auto measure = [&](float ms, uint32_t seq, double at) {
            seqSeen = seq;
            if (seq <= ignoreUntil) return;
            const float dt = static_cast<float>(std::max(B, at - lastMeasuredAt));
            lastMeasuredAt = at;
            ema = ema < 0 ? ms : 0.75f * ema + 0.25f * ms;
            const float cur = DynCost(level, bg);
            auto pred = [&](int l, int b) { return ema * DynCost(l, b) / cur; };
            const float Bf = static_cast<float>(B);
            if (c.pol == kSv) {   // strikersVita: any late frame (here: GPU over the budget) -> svLow rungs below the game's own size for 2 s, then back
                if (ms > Bf) svLeft = static_cast<int>(2000.0 / B);
                else if (svLeft > 0) --svLeft;
                const int want = svLeft > 0 ? kNative + c.svLow : kNative;
                if (want != level) setLevel(want, want >= kPivot + 1 ? 1 : 0, present, true);
                return;
            }
            if (c.pol == kHalo) {   // Halo: down a rung after two frames over 92 %; up a rung after 1 s in which every frame fits the next level at 80 %
                haloHi = ms > 0.92f * Bf ? haloHi + 1 : 0;
                int ul = level, ub = bg;
                const bool canUp = DynUp(ul, ub, kNative);
                const bool fits = canUp && ms * DynCost(ul, ub) / cur < 0.80f * Bf;
                haloRoom = fits ? haloRoom + 1 : 0;
                if (haloHi >= 2) {
                    int l = level, b = bg;
                    if (DynDown(l, b, floor) && setLevel(l, b, present, false)) haloHi = 0;
                } else if (canUp && haloRoom >= static_cast<int>(1000.0 / B)) {
                    if (setLevel(ul, ub, present, false)) haloRoom = 0;
                }
                return;
            }
            if (ms > Bf) ++over; else over = 0;
            hot = ms > 0.90f * Bf ? std::min(hot + 1, std::max(hold30, 1)) : std::max(hot - 1, 0);
            const bool held30 = interval != 1 && hold30 > 0 && hot < hold30;   // (MSC_DYNRES_HOLD30: the mean alone does not step at 30 until it is sustained)
            if (level < kNative && ms > 0.92f * Bf) ++hi; else hi = 0;
            if (upHold > 45 && seq - lastDownSeq > 1200) upHold = 45;
            sev.Frame(ema, ms, Bf, pred(floor, 1), false);
            late.Frame(ms, Bf, dt, false);
            const int floorUsed = sev.open ? std::max(floor, kSevere) : floor;
            if (level > floorUsed) setLevel(floorUsed, 1, present, false);
            else if (level < kNative && (hi >= 2 || late.want)) {
                if (setLevel(kNative, 0, present, true)) {
                    lastDownSeq = seq;
                    if (present - lastAboveDown > 7200) aboveFails = 0;
                    lastAboveDown = present;
                    uint32_t lock = 1200;
                    for (int k = 0; k < aboveFails && lock < 9600; ++k) lock *= 2;
                    aboveLockUntil = present + lock;
                    if (aboveFails < 3) ++aboveFails;
                    aboveWin = aboveBad = hi = 0;
                    if (late.want) late.Took(true);
                }
            } else if ((ema > 0.90f * Bf && !held30) || over >= 3 || late.want) {
                int l = level, b = bg;
                while (pred(l, b) > 0.82f * Bf && DynDown(l, b, floorUsed)) {}
                if (l == level && b == bg && over >= 3) DynDown(l, b, floorUsed);
                if (late.want) {   // MSC_DYNRES_LATEHOLD: as far as this late frame says it needs, up to maxStep rungs, one at least
                    int ll = level, lb = bg, k = 0;
                    const float lms = std::max(late.wantMs, ms);
                    while (k < late.maxStep && (k == 0 || lms * DynCost(ll, lb) / cur > 0.82f * Bf) && DynDown(ll, lb, floorUsed)) ++k;
                    if (ll > l || (ll == l && lb > b)) { l = ll; b = lb; }
                }
                if (l != level || b != bg) {
                    if (lastUpSeq && seq - lastUpSeq < 180 && upHold < 720) upHold *= 2;
                    const bool done = setLevel(l, b, present, false);
                    if (done) lastDownSeq = seq;
                    if (late.want && done) late.Took(true);
                } else {
                    over = 0;
                    if (late.want) late.Took(false);
                }
            } else if (level > 0 && level <= kNative) {
                const int nxt = level - 1;
                const float proj = ms * DynCost(nxt, 0) / cur;
                const bool locked = present < aboveLockUntil || present - lastChange < 90 || late.Holding();
                const int need = interval == 1 ? 60 : 30;
                if (locked || proj > 0.92f * Bf) {
                    aboveWin = aboveBad = headroom = 0;
                } else {
                    ++aboveWin;
                    if (proj > 0.80f * Bf) ++aboveBad;
                    if (aboveBad * 10 > aboveWin) aboveWin = aboveBad = 0;
                    else if (aboveWin >= need) {
                        if (setLevel(nxt, 0, present, false)) lastUpSeq = seq;
                        aboveWin = aboveBad = 0;
                    }
                }
            } else if (int ul = level, ub = bg; !late.Holding() && DynUp(ul, ub, 0) && pred(ul, ub) < 0.72f * Bf) {
                if (++headroom >= upHold && present - lastChange >= 90)
                    if (setLevel(ul, ub, present, false)) lastUpSeq = seq;
            } else {
                headroom = 0;
            }
        };
        if (!g_console) {
            // the ladder: every frame that has finished by now is measured, in order
            while (next < i && f[next] <= s[i]) {
                const int j = next++;
                measure(static_cast<float>(g[j]), static_cast<uint32_t>(j + 1), f[j]);
            }
        } else {
            // the console: only the newest finished frame, and only if it was timed (vglRtLastGpuFrame)
            int newest = -1;
            while (next < i && f[next] <= s[i]) newest = next++;
            bool fresh = false;
            if (newest >= 0 && newest != lastNewest) {
                lastNewest = newest;
                if (!untimed[newest]) {
                    fresh = true;
                    measure(static_cast<float>(g[newest]), ++lseq, f[newest]);
                }
            }
            if (guard.on) {
                const float picture = i >= 2 ? static_cast<float>(p[i - 1] - p[i - 2]) : static_cast<float>(B);
                const vgx::BlindGuard::Verdict v = guard.Picture(fresh, picture, cpuOf(i - 1 >= 0 ? i - 1 : 0) * static_cast<float>(B), static_cast<float>(B));
                if (v == vgx::BlindGuard::kGpuLate) { o.fed += 1; measure(picture, ++lseq, s[i]); }
                else if (v != vgx::BlindGuard::kNone) seqSeen = ++lseq;   // (a frame counted, nothing measured)
            }
        }
        lvOf[i] = level;
        bOf[i] = bg;
        const bool resized = i > 0 && (kLv[level].s != kLv[lvOf[i - 1]].s || kLv[level].ms != kLv[lvOf[i - 1]].ms);
        g[i] = tr.f[i] * B * TrueCost(level, bg) / TrueCost(kNative, 0) + (resized ? resizeMs : 0.0);
        if (g_console) {
            int inflight = 0;
            for (int j = i - 1; j >= 0 && f[j] > s[i]; --j) ++inflight;
            untimed[i] = (inflight + 1) * g_scenes > 48;
            o.untimed += untimed[i];
        }
        f[i] = std::max(i ? f[i - 1] : 0.0, s[i]) + g[i];
        double pi = std::ceil(f[i] / V - 1e-9) * V;
        if (i > 0) pi = std::max(pi, p[i - 1] + B - 1e-6);
        p[i] = std::ceil(pi / V - 1e-6) * V;
        if (i > 0) {
            const double gap = p[i] - p[i - 1];
            const bool isLate = gap > B + 1.0;
            if (isLate) { o.late += 1; ++run; } else run = 0;
            if (i >= w0 && i < w1) { o.winLate += isLate; o.winFrames += 1; o.winPix += kLv[level].s * kLv[level].s; }
            if (isLate && std::getenv("SIM_DEBUG")) std::printf("  late frame %d: gap %.1f ms, GPU %.1f ms, level %d, trace %.2f\n", i, gap, g[i], level, tr.f[i]);
            o.worstRun = std::max(o.worstRun, static_cast<double>(run));
            o.longGap = std::max(o.longGap, gap);
        }
        o.pix += kLv[level].s * kLv[level].s;
        o.frames += 1;
    }
    o.pix /= o.frames;
    return o;
}

} // namespace

// --blind-test: the console's GPU timers go silent under a heavy GPU overload (the Galaxy console's Beach Bowl at zoom 2.0: 25 fps for 25 s); the ladder must step
// down with MSC_DYNRES_BLIND and could not without it. And where the timers keep reporting, the guard changes nothing. Returns 1 on a failed check.
int BlindTest() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; };
    g_console = true;
    g_scenes = 17;
    for (int fps : {30, 60}) {
        const int interval = fps == 60 ? 1 : 2;
        const int perSec = fps;
        const int N = 120 * perSec, a = 40 * perSec, b = 65 * perSec;
        // 80 % play, 25 s at 1.5x (the GPU at 1.2 x the budget at the game's own size), the CPU lanes at half the budget
        // 80 % play, then 25 s at twice the cost (the GPU at 1.6 x the budget at the game's own size), 17 scenes a frame (the 1.0/1.1 console logs' menus; the emulator
        // counts 7-12.6 in play, up to 16.7), the CPU lanes at half the budget. With 10 scenes a frame the timers never go silent in this model (three frames in flight
        // are 30 scenes): the blind spot needs 17 or more.
        Trace heavy = Make("80% + 25 s at 2x, 17 scenes a frame", N, 7, 0.80f, 0.05f, 0.004f, 3.0f, 1.1f, 1.3f, {{a, b}}, 2.0f);
        Cfg off{kLadder, 0, 0, 1.0f, 0, "ladder"}, on = off;
        on.blind = true;
        if (fps == 60) { off = Cfg{kLate, 2000, 2, 1.0f, 0, "LATEHOLD"}; on = off; on.blind = true; }
        const Out o0 = Run(heavy, off, interval, 3.0f, a, b), o1 = Run(heavy, on, interval, 3.0f, a, b);
        const double l0 = o0.winLate / o0.winFrames, l1 = o1.winLate / o1.winFrames;
        std::printf("== %d fps, %s: in the stretch: late %.1f%% -> %.1f%%, pixels %.3f -> %.3f; untimed frames %.0f -> %.0f; blind pictures fed %.0f\n", fps, heavy.name.c_str(),
                    100.0 * l0, 100.0 * l1, o0.winPix / o0.winFrames, o1.winPix / o1.winFrames, o0.untimed, o1.untimed, o1.fed);
        check(o0.untimed > 0.5 * (b - a), "without the guard the timers go silent for most of the stretch (the blind spot reproduces)");
        check(l0 >= (fps == 30 ? 0.30 : 0.15), "without the guard many of the stretch's pictures are late (30 %: 30 % or more, 60: 15 %)");
        check(o1.fed > 0, "the guard feeds late pictures to the ladder");
        check(l1 <= 0.5 * l0, "with the guard the stretch has at most half as many late pictures");
        check(o1.winPix < o0.winPix - 1e-6, "with the guard the stretch is drawn smaller");
        // the timers keep reporting (a moderate load): the guard is never blind and nothing changes
        Trace calm = Make("85% clusters, timers reporting", N, 1, 0.85f, 0.06f, 0.010f, 6.0f, 1.2f, 1.6f);
        const Out c0 = Run(calm, off, interval, 3.0f), c1 = Run(calm, on, interval, 3.0f);
        std::printf("   %s: late %.2f%% / %.2f%%, changes %.0f / %.0f, pixels %.4f / %.4f, untimed %.0f, fed %.0f\n", calm.name.c_str(), 100.0 * c0.late / c0.frames,
                    100.0 * c1.late / c1.frames, c0.changes, c1.changes, c0.pix, c1.pix, c1.untimed, c1.fed);
        check(c1.fed == 0 && c0.late == c1.late && c0.changes == c1.changes && c0.pix == c1.pix, "timers reporting: the guard changes nothing");
    }
    std::printf("%s\n", fails ? "dynres_sim --blind-test: FAILED" : "dynres_sim --blind-test: ok");
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    int fps = 30;
    float resize = 3.0f;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--blind-test")) return BlindTest();
        if (!std::strcmp(argv[i], "--console")) g_console = true;
        else if (!std::strncmp(argv[i], "--scenes=", 9)) g_scenes = std::clamp(std::atoi(argv[i] + 9), 1, 48);
        else if (!std::strncmp(argv[i], "--hold30=", 9)) g_hold30 = std::clamp(std::atoi(argv[i] + 9), 0, 60);
        else if (!std::strncmp(argv[i], "--fps=", 6)) fps = std::atoi(argv[i] + 6);
        else if (!std::strncmp(argv[i], "--resize=", 9)) resize = static_cast<float>(std::atof(argv[i] + 9));
        else if (!std::strncmp(argv[i], "--pix=", 6)) g_pix = static_cast<float>(std::atof(argv[i] + 6));
        else if (!std::strncmp(argv[i], "--depth=", 8)) g_depth = std::clamp(std::atoi(argv[i] + 8), 1, 3);
    }
    const int interval = fps == 60 ? 1 : 2;
    const int perSec = fps == 60 ? 60 : 30;
    const int F = 180 * perSec;   // three minutes each
    std::vector<Trace> traces = {
        // open play near the budget: mean 85 %, clusters of heavier frames (shots, items, a goal-mouth scramble) 1 % of frames start one, mean 6 frames, 1.2-1.6x
        Make("open play 85%, clusters", F, 1, 0.85f, 0.06f, 0.010f, 6.0f, 1.2f, 1.6f),
        // the Galaxy shape: mean 90 %, wider spread, longer clusters (r31: 1-3 % late before any ladder)
        Make("galaxy-shaped 90%", F, 2, 0.90f, 0.10f, 0.010f, 8.0f, 1.1f, 1.5f),
        // light play: mean 65 %, isolated spikes to 1.5-2.5x (a frame of a load or an explosion)
        Make("light 65%, isolated spikes", F, 3, 0.65f, 0.05f, 0.004f, 1.2f, 1.5f, 2.5f),
        // heavy stretches: mean 80 % with 6 s at 1.3x every 40 s (crowd shots, Pipeline Central mist)
        Make("80% + 6 s stretches at 1.3x", F, 4, 0.80f, 0.06f, 0.006f, 4.0f, 1.1f, 1.4f,
             {{20 * perSec, 26 * perSec}, {60 * perSec, 66 * perSec}, {100 * perSec, 106 * perSec}, {140 * perSec, 146 * perSec}}, 1.3f),
        // a drop-in: 15 s at 1.8x the budget (Galaxy's measured 11-12 fps stretch) in 80 % play
        Make("80% + 15 s drop-in at 1.8x", F, 5, 0.80f, 0.06f, 0.004f, 4.0f, 1.1f, 1.4f, {{60 * perSec, 75 * perSec}}, 2.25f),
    };
    std::vector<Cfg> cfgs = {
        {kLadder, 0, 0, 1.0f, 0, "ladder (1.3 today)"},
        {kLate, 1000, 1, 1.0f, 0, "LATEHOLD=1000 STEP=1"},
        {kLate, 2000, 1, 1.0f, 0, "LATEHOLD=2000 STEP=1"},
        {kLate, 2000, 2, 1.0f, 0, "LATEHOLD=2000 STEP=2"},
        {kLate, 3000, 2, 1.0f, 0, "LATEHOLD=3000 STEP=2"},
        {kLate, 2000, 2, 0.95f, 0, "LATEHOLD=2000 STEP=2 AT=95"},
        {kSv, 0, 0, 1.0f, 3, "strikersVita: late -> 3 rungs, 2 s"},
        {kHalo, 0, 0, 1.0f, 0, "Halo: 2>92% down, 1 s<80% up"},
    };
    if (g_console) {   // MSC_DYNRES_BLIND, on the console's timer model
        Cfg a{kLadder, 0, 0, 1.0f, 0, "ladder + BLIND"}, b{kLate, 2000, 2, 1.0f, 0, "LATEHOLD=2000 STEP=2 + BLIND"};
        a.blind = b.blind = true;
        cfgs.push_back(a);
        cfgs.push_back(b);
    }
    if (g_hold30 > 0) {   // MSC_DYNRES_HOLD30 against the ladder without it
        Cfg h{kLadder, 0, 0, 1.0f, 0, "ladder, HOLD30 off"};
        h.hold30 = 0;
        cfgs.insert(cfgs.begin(), h);
    }
    std::printf("dynres_sim: %d fps budget, queue depth %d, resize cost %.1f ms, true pixel share %.2f (model 0.75); 3 min per trace%s; HOLD30 %d\n", fps, g_depth, resize, g_pix,
                g_console ? "; console GPU times (newest only, untimed past 48 scenes)" : "", g_hold30);
    for (const Trace& t : traces) {
        std::printf("\n== %s\n%-38s %7s %6s %8s %7s %8s\n", t.name.c_str(), "policy", "late%", "run", "maxgap", "chg/min", "pixels");
        for (const Cfg& c : cfgs) {
            const Out o = Run(t, c, interval, resize);
            std::printf("%-38s %6.2f%% %6.0f %6.0fms %7.1f %7.3f\n", c.name, 100.0 * o.late / o.frames, o.worstRun, o.longGap, o.changes / 3.0, o.pix);
        }
    }
    return 0;
}
