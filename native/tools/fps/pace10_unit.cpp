// pace10: spacing, game speed and estimated press-to-picture delay of a held 30 on a model of the console pipeline, for the late-start controllers.
// A MODEL, not a console measurement: every figure printed is an estimate from stage costs taken from the r11 console logs (Vice = d1, The Dump = worstgpu1). The structure is
// tools/fps/pace_mid_model.cpp's held 30 (game thread -> send/decode -> back end -> GPU -> display queue of two -> display gate of two refreshes); the controllers are the REAL classes:
// np_feel.h PadLate (MSC_PAD_LATE, today), np_fps_phase.h PhaseSync (MSC_PHASE_SYNC, new), np_fps_bp.h BackPressure (MSC_LAT_BP), np_game_clock.h GameClock (the debt clock).
//   g++ -std=c++17 -O2 -Inative/platform/compat native/tools/fps/pace10_unit.cpp -o /tmp/pace10_unit && /tmp/pace10_unit [--table]
// Mutants: -DNP_PHASE_BREAK=1..5 (np_fps_phase.h, caught by the controller checks) and -DPACE10_BREAK=1 (the pipeline checks); the harness itself has a negative control (the wiring bug of feeding the sleep back as idle time must be seen).
#include "../../platform/sdk/input/np_feel.h"
#include "np_fps_bp.h"
#include "np_fps_phase.h"
#include "np_game_clock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace {
constexpr double kV = 1001.0 / 60.0;
int g_fail = 0;
void Require(bool ok, const char* why) { if (!ok) { std::printf("FAIL: %s\n", why); ++g_fail; } }
#ifndef PACE10_BREAK
#define PACE10_BREAK 0   // 1: the controller is not given the thread's idle time (a delay may exceed it)
#endif
enum class Latch { None, PadLate, Phase };
enum class Gate { Old, Bp };
struct Policy { const char* name; Gate gate; int depth; Latch latch; unsigned window = 90; unsigned maxUs = 12000; unsigned ignore = 3; unsigned margin = 2500; };
struct Scene { const char* name; double game, pre, front, back, gpu, xlat, jitter, refP50, refP95, spikeP, spikeMs, gameJitter, gpuSpikeP, stepMs; };   // gameJitter: the game thread's own spread (-1 = jitter); gpuSpikeP: GPU spikes of +60 % (independent of the late start); stepMs: the game thread costs this much more from the middle of the run   // spikeP / spikeMs: a game-thread hitch (a texture, a shader, the collector) in that share of pictures
// stage costs (ms): game = frame start to hand-over, pre = frame start to pad read; front = send + decode; back = back end; gpu; xlat = GXM/display latency after the GPU, fitted so the control matches the console's p50.
const Scene kScenes[] = {
    {"Vice (r11 d1: game 22.7, send+fe 17, be 7.4, gpu 18.8; console p50 100.5 p95 130.5)", 22.7, 4.0, 17.0, 7.4, 18.8, 17.0, 0.12, 100.5, 130.5, 0.03, 18.0, -1, 0, 0},
    {"The Dump (r11 worstgpu1: game 21.3, send+fe 18, be 11, gpu 24.8; console p50 107.4 p95 143.9)", 21.3, 4.0, 18.0, 11.0, 24.8, 17.0, 0.14, 107.4, 143.9, 0.07, 18.0, -1, 0, 0},
    {"r10 bp0d1 (game 20.5, gpu 29.7: the depth-1 run; console p50 141.1 p95 281.6, 25.1 fps)", 20.5, 4.0, 17.0, 9.0, 29.7, 17.0, 0.14, 141.1, 281.6, 0.05, 18.0, -1, 0, 0},
    {"steady game thread (Vice costs, game spread 3 %, no hitches, GPU spikes in 3 % of pictures)", 22.7, 4.0, 17.0, 7.4, 18.8, 17.0, 0.06, 0, 0, 0.0, 0.0, 0.03, 0.03, 0},
    {"steady, then the game thread costs 5 ms more from half time (a heavier area)", 22.7, 4.0, 17.0, 7.4, 18.8, 17.0, 0.06, 0, 0, 0.0, 0.0, 0.03, 0.03, 5.0},
};
struct Result { double mean, fps, p50, p95, h1, h2, h3, h4, speed, engaged, meanDelay, slackMed; unsigned bpDepth; double lateShare; };
uint32_t Vc(double t) { return static_cast<uint32_t>(std::floor(t / kV)); }
double NextVblank(double t) { return (Vc(t) + 1) * kV + 0.02; }

Result Run(const Policy& pol, const Scene& sc, unsigned seed, double seconds = 120.0) {
    std::mt19937 rng(seed);
    auto cost = [&](double mean) {
        std::normal_distribution<double> d(mean, sc.jitter * mean);
        const double spike = std::uniform_real_distribution<double>(0, 1)(rng) < 0.03 ? 0.4 * mean : 0.0;
        const double v = d(rng) + spike;
        return v < 0.4 * mean ? 0.4 * mean : v;
    };
    NpFeel::PadLate pl;
    pl.maxUs = pol.maxUs;
    npfps::PhaseSync ps;
    { npfps::PhaseConfig c; c.window = pol.window; c.maxUs = pol.maxUs; c.ignore = pol.ignore; c.marginUs = pol.margin; ps.Configure(c); }
    npfps::BackPressure bp;
    npfps::ClockConfig cc;
    npfps::GameClock clk(cc);
    double t = 0.3, frontFree = 0, beFree = 0, gpuFree = 0, cbFree = 0;
    uint32_t anchor = 0, lastShown = 0, fifLast = 0, lastEntry = 0;
    bool haveAnchor = false, haveShown = false, fifHave = false, haveEntry = false;
    int fifPaused = 0;
    std::deque<double> cbReturn;
    std::vector<double> shownAt, shownTime, handedAt;
    std::vector<double> delays, slacks;
    size_t taken = 0;
    uint64_t late3 = 0;
    uint64_t hold[6] = {};
    double prevStart = -1, waitPrev = 0, dPrev = 0, front0Prev = 0, gameMsPrev = 22, seen = 33.4, dSum = 0;
    int prevShownV = -1;
    uint64_t pictures = 0, engaged = 0, counted = 0;
    double firstT = -1, lastT = 0, realTot = 0, advTot = 0;
    const double warm = 6000.0;
    while (t < seconds * 1000.0) {
        const double start = t;
        // what the display reported since the last call (the real code: NpLatSlackTake)
        uint32_t minSlackUs = 0xFFFFFFFFu, maxGapUs = 0;
        int n = 0;
        while (taken < shownAt.size() && shownAt[taken] <= start) {
            const double sl = (shownTime[taken] - handedAt[taken]) * 1000.0;
            if (sl >= 0 && sl < minSlackUs) minSlackUs = static_cast<uint32_t>(sl);
            if (taken > 0) { const double g = (shownTime[taken] - shownTime[taken - 1]) * 1000.0; if (g > maxGapUs) maxGapUs = static_cast<uint32_t>(g); }
            ++taken; ++n;
        }
        const uint32_t period = 2 * 16683u;
        bool late = n > 0 && maxGapUs > period + period / 2;
        uint32_t D = 0;
        const uint32_t cpuRaw = waitPrev > dPrev ? static_cast<uint32_t>((waitPrev - dPrev) * 1000.0) : 0u;   // (the previous frame's waits less the sleep)
        if (pol.latch == Latch::PadLate) {
            if (n > 0) pl.Observe(std::min(minSlackUs, cpuRaw), late);
            D = pl.DelayUs();
        } else if (pol.latch == Latch::Phase) {
#if PACE10_BREAK == 1
            const uint32_t cpuIn = npfps::PhaseSync::kNone;
#else
            const uint32_t cpuIn = cpuRaw;
#endif
            D = ps.Step(n > 0 ? minSlackUs : npfps::PhaseSync::kNone, cpuIn, late);
        }
        const double Dms = D / 1000.0;
        if (prevStart >= 0) {
            const double real = start - prevStart;
            realTot += real;
            const unsigned cap = npfps::CapSteps(cc, 33.4, seen, 11.0, 8.0);
            advTot += clk.Frame(real, cap);
            seen += (real - seen) / 8.0;
        }
        prevStart = start;
        const double hitch = std::uniform_real_distribution<double>(0, 1)(rng) < sc.spikeP ? sc.spikeMs : 0.0;
        const double gameBase = sc.game + (sc.stepMs > 0 && start > seconds * 500.0 ? sc.stepMs : 0.0);
        const double gj = sc.gameJitter >= 0 ? sc.gameJitter : sc.jitter;
        const double gameMs = [&] { std::normal_distribution<double> d(gameBase, gj * gameBase); return std::fmax(0.4 * gameBase, d(rng)); }() + hitch, frontMs = cost(sc.front), backMs = cost(sc.back), gpuMs = cost(sc.gpu) * (std::uniform_real_distribution<double>(0, 1)(rng) < sc.gpuSpikeP ? 1.6 : 1.0);
        const double padT = start + sc.pre + Dms;
        const double hand = start + gameMs + Dms;
        const double front0 = std::fmax(hand, frontFree), front1 = front0 + frontMs;
        const double be0 = std::fmax(front1, beFree);
        frontFree = be0;
        double be1 = be0 + backMs;
        if (cbReturn.size() >= 2) be1 = std::fmax(be1, cbReturn[cbReturn.size() - 2]);
        beFree = be1;
        const double gpu0 = std::fmax(be0, gpuFree), gpu1 = std::fmax(gpu0 + gpuMs, be1);
        gpuFree = gpu1;
        const double handed = std::fmax(gpu1 + sc.xlat, cbFree);
        const uint32_t earliest = Vc(handed) + 1;
        uint32_t target = earliest;
        if (haveShown && target < lastShown + 2) target = lastShown + 2;
        lastShown = target;
        const double shownT = target * kV;
        cbFree = target != earliest ? (target - 1) * kV + 0.2 : handed + 0.1;
        cbReturn.push_back(cbFree);
        if (cbReturn.size() > 4) cbReturn.pop_front();
        shownAt.push_back(cbFree);
        shownTime.push_back(shownT);
        handedAt.push_back(handed);
        const bool counting = start > warm;
        if (counting) {
            delays.push_back(shownT - padT);
            slacks.push_back(shownT - handed);
            ++counted;
            if (D) { ++engaged; dSum += Dms; }
            if (firstT < 0) firstT = start;
            lastT = start;
        }
        if (prevShownV >= 0 && counting) { const int h = static_cast<int>(target) - prevShownV; ++hold[h < 5 ? h : 5]; if (h >= 3) ++late3; }
        prevShownV = static_cast<int>(target);
        haveShown = true;
        ++pictures;
        dPrev = Dms;
        // the game thread goes on: the fork waits for the job before, the late wait (two refreshes after the previous frame's wait ended), then the gate
        double next = front0;
        const uint32_t sendRef = haveAnchor ? anchor + 2 : Vc(hand) + 2;
        while (Vc(next) < sendRef) next = NextVblank(next);
        const uint32_t preGate = Vc(next);
        unsigned waited = 0;
        if (pol.gate == Gate::Bp) {
            auto shownCount = [&](double at) { size_t c = 0; for (size_t i = shownAt.size(); i > 0; --i) if (shownAt[i - 1] <= at) { c = i; break; } return c; };
            const bool haveGap = haveEntry && preGate - lastEntry <= 8;
            const unsigned gap = preGate - lastEntry;
            lastEntry = preGate; haveEntry = true;
            const double roomMs = 2 * kV - gameMsPrev - 3.0;
            const unsigned slackR = roomMs > 0 ? static_cast<unsigned>(roomMs / kV) : 0u;
            while (bp.Waits(pictures, shownCount(next), waited, slackR)) { next = NextVblank(next); ++waited; }
            bp.After(waited, gap, haveGap, 2);
        } else if (pol.depth) {
            const bool lateFrame = fifHave && static_cast<int>(Vc(next) - fifLast) > 2;
            if (lateFrame) fifPaused = 150;
            if (fifPaused) { --fifPaused; }
            else {
                size_t shownTag = 0;
                for (size_t i = shownAt.size(); i > 0; --i) if (shownAt[i - 1] <= next) { shownTag = i; break; }
                if (pictures > shownTag + static_cast<uint64_t>(pol.depth)) { next = NextVblank(next); ++waited; }
            }
            fifLast = preGate; fifHave = true;
        }
        anchor = preGate; haveAnchor = true;
        waitPrev = (next - front0) + Dms;   // (late wait + gate + the sleep: what the real code adds to g.waitNs)
        front0Prev = front0;
        gameMsPrev = 0.9 * gameMsPrev + 0.1 * gameMs;
        t = next;
    }
    (void)front0Prev;
    Result r{};
    const double span = (lastT - firstT) / 1000.0;
    r.fps = counted / span;
    std::sort(delays.begin(), delays.end());
    { double sm = 0; for (double d : delays) sm += d; r.mean = sm / delays.size(); }
    r.p50 = delays[delays.size() / 2];
    r.p95 = delays[delays.size() * 95 / 100];
    const double hn = static_cast<double>(hold[1] + hold[2] + hold[3] + hold[4] + hold[5]);
    r.h1 = 100 * hold[1] / hn; r.h2 = 100 * hold[2] / hn; r.h3 = 100 * hold[3] / hn; r.h4 = 100 * (hold[4] + hold[5]) / hn;
    r.speed = realTot > 0 ? 100.0 * advTot / realTot : 100.0;
    r.engaged = counted ? 100.0 * engaged / counted : 0;
    r.meanDelay = engaged ? dSum / engaged : 0;
    std::sort(slacks.begin(), slacks.end());
    r.slackMed = slacks[slacks.size() / 2];
    r.bpDepth = bp.Depth();
    r.lateShare = hn ? 100.0 * late3 / hn : 0;
    return r;
}
Result Mean(const Policy& p, const Scene& s, unsigned seeds = 8) {
    Result m{};
    for (unsigned i = 1; i <= seeds; ++i) {
        const Result r = Run(p, s, i);
        m.mean += r.mean; m.fps += r.fps; m.p50 += r.p50; m.p95 += r.p95; m.h1 += r.h1; m.h2 += r.h2; m.h3 += r.h3; m.h4 += r.h4; m.speed += r.speed; m.engaged += r.engaged; m.meanDelay += r.meanDelay; m.slackMed += r.slackMed; m.lateShare += r.lateShare;
        m.bpDepth = r.bpDepth;
    }
    const double k = seeds;
    m.mean /= k; m.fps /= k; m.p50 /= k; m.p95 /= k; m.h1 /= k; m.h2 /= k; m.h3 /= k; m.h4 /= k; m.speed /= k; m.engaged /= k; m.meanDelay /= k; m.slackMed /= k; m.lateShare /= k;
    return m;
}
void Print(const char* name, const Result& r) {
    std::printf("  %-44s %5.1f fps  held 1/2/3/4+ %4.1f/%5.1f/%4.1f/%4.1f %%  pad->shown mean %5.1f p50 %5.1f p95 %5.1f ms  speed %6.2f %%  latch on %3.0f %% (mean %4.1f ms)  handed->shown %4.1f  bp depth %u\n", name, r.fps, r.h1, r.h2, r.h3, r.h4,
                r.mean, r.p50, r.p95, r.speed, r.engaged, r.meanDelay, r.slackMed, r.bpDepth);
}

// ---- the controller on synthetic observations (no pipeline): raw slack of a picture's display side S, the thread's idle time C (both microseconds, what the delay may use up) ----
struct SynthOut { double engaged, meanD, causedLates, otherLates; unsigned steps; double dAfterDrop; unsigned causedAfterDrop; unsigned widenings, marginEnd; };
// kind 0 PadLate (np_feel.h), 1 PhaseSync. uncaused: share of pictures that are late whatever the delay (a GPU spike). dropAt/dropUs: the raw slacks fall by dropUs from that step on.
SynthOut Synth(int kind, double uncaused, unsigned steps, unsigned dropAt, uint32_t dropUs, uint32_t S0 = 14000, uint32_t C0 = 10000, unsigned seed = 7, bool bugSleepInIdle = false) {
    std::mt19937 rng(seed);
    NpFeel::PadLate pl;
    npfps::PhaseSync ps;
    npfps::PhaseConfig cfg;
    ps.Configure(cfg);
    const unsigned lag = 3;
    std::vector<uint32_t> D(steps + lag + 2, 0), S(steps + lag + 2, 0), Cc(steps + lag + 2, 0);
    std::vector<char> lateAt(steps + lag + 2, 0), caused(steps + lag + 2, 0);
    SynthOut o{};
    unsigned on = 0, cn = 0, un = 0, dn = 0;
    double dsum = 0, dAfter = 0;
    unsigned afterN = 0;
    for (unsigned k = 0; k < steps; ++k) {
        std::uniform_real_distribution<double> u(0, 1);
        const uint32_t drop = dropAt && k >= dropAt ? dropUs : 0;
        S[k] = static_cast<uint32_t>(S0 - drop + static_cast<int>(1000 * (u(rng) - 0.5)));
        Cc[k] = static_cast<uint32_t>(C0 - drop + static_cast<int>(1000 * (u(rng) - 0.5)));
        // what the controller sees now: the picture `lag` steps ago (display side), the previous frame's idle (raw)
        uint32_t seenDisplay = npfps::PhaseSync::kNone;
        bool seenLate = false;
        if (k >= lag) { const unsigned j = k - lag; seenDisplay = S[j] > D[j] ? S[j] - D[j] : 0u; seenLate = lateAt[j]; }
        const uint32_t cpu = k ? Cc[k - 1] + (bugSleepInIdle ? D[k - 1] : 0u) : C0;   // (the bug: the previous sleep counted as idle time)
        uint32_t d;
        if (kind == 1) d = ps.Step(seenDisplay, cpu, seenLate);
        else { if (k >= lag) pl.Observe(std::min(seenDisplay, cpu), seenLate); d = pl.DelayUs(); }
        D[k] = d;
        const uint32_t room = std::min(S[k], Cc[k]);
        const bool byDelay = d > room;
        const bool spike = u(rng) < uncaused;
        lateAt[k] = byDelay || spike;
        caused[k] = byDelay;
        if (d) { ++on; dsum += d; }
        if (byDelay) { ++cn; if (dropAt && k >= dropAt) ++o.causedAfterDrop; } else if (spike) ++un;
        if (dropAt && k >= dropAt + 150) { dAfter += d; ++afterN; }
        ++dn;
    }
    o.engaged = 100.0 * on / dn;
    o.meanD = on ? dsum / on / 1000.0 : 0;
    o.causedLates = 100.0 * cn / dn;
    o.otherLates = 100.0 * un / dn;
    o.dAfterDrop = afterN ? dAfter / afterN / 1000.0 : 0;
    o.widenings = ps.Stats().widens;
    o.marginEnd = ps.MarginNowUs();
    o.steps = dn;
    return o;
}
}  // namespace

int main(int argc, char**) {
    const bool table = argc > 1;
    const Policy control = {"control: BP gate, no late latch", Gate::Bp, 2, Latch::None};
    const Policy padlate = {"MSC_PAD_LATE=1 (today's plan)", Gate::Bp, 2, Latch::PadLate};
    const Policy phase = {"MSC_PHASE_SYNC=1 (window 90, ignore 3)", Gate::Bp, 2, Latch::Phase, 90, 12000, 3};
    const Policy phase24 = {"MSC_PHASE_SYNC=1 window 24, ignore 0", Gate::Bp, 2, Latch::Phase, 24, 12000, 0};
    const Policy phase90 = {"MSC_PHASE_SYNC=1 window 90, ignore 0 (the min)", Gate::Bp, 2, Latch::Phase, 90, 12000, 0};
    const Policy phase16 = {"MSC_PHASE_SYNC=1 window 90, ignore 3, max 16 ms", Gate::Bp, 2, Latch::Phase, 90, 16000, 3};
    const Policy old1 = {"old gate, depth 1 (MSC_LAT_BP=0 MSC_LAT_DEPTH=1)", Gate::Old, 1, Latch::None};
    const Policy old2 = {"old gate, depth 2 (MSC_LAT_BP=0)", Gate::Old, 2, Latch::None};
    const Policy old3 = {"old gate, depth 3", Gate::Old, 3, Latch::None};
    if (std::getenv("P10_SWEEP")) {
        for (int sc : {0, 1, 3, 4}) { std::printf("%s\n", kScenes[sc].name); const Result c0 = Mean(control, kScenes[sc], 24); Print("control", c0);
            for (unsigned mg : {2500u, 4000u, 6000u}) for (unsigned ig : {0u, 3u}) for (unsigned mx : {8000u, 12000u}) { Policy q = {"PS", Gate::Bp, 2, Latch::Phase, 90, mx, ig, mg}; char nm[80]; std::snprintf(nm, sizeof nm, "PS margin %u ignore %u max %u", mg, ig, mx); Print(nm, Mean(q, kScenes[sc], 24)); } }
        return 0;
    }
    if (std::getenv("P10_ONE")) { const int sc = std::atoi(std::getenv("P10_ONE")); const Policy* q[2] = {&control, &phase}; for (int i = 0; i < 2; ++i) { std::printf("%s\n", q[i]->name); Run(*q[i], kScenes[sc], 1); } return 0; }
    Result res[5][9];
    const Policy* pols[9] = {&control, &padlate, &phase24, &phase, &phase90, &phase16, &old1, &old2, &old3};
    for (int s = 0; s < 5; ++s) {
        if (table) std::printf("%s\n", kScenes[s].name);
        for (int p = 0; p < 9; ++p) { res[s][p] = Mean(*pols[p], kScenes[s]); if (table) Print(pols[p]->name, res[s][p]); }
    }
    // ---- checks ----
    char w[220];
    // 1. the controller on synthetic observations. S = display slack 14 ms, thread idle 10 ms: the most a delay can be is 10 - margin (2.5) = 7.5 ms.
    const SynthOut ps0 = Synth(1, 0.02, 6000, 0, 0), pl0 = Synth(0, 0.02, 6000, 0, 0);
    const SynthOut psq = Synth(1, 0.0, 6000, 0, 0);
    const SynthOut psd = Synth(1, 0.0, 6000, 3000, 6000), pld = Synth(0, 0.0, 6000, 3000, 6000);
    if (table) {
        std::printf("controller, synthetic (display slack 14 ms, thread idle 10 ms, 2 %% late pictures that no delay causes):\n");
        std::printf("  PAD_LATE    on %5.1f %% mean %4.1f ms  caused lates %.2f %%\n  PHASE_SYNC  on %5.1f %% mean %4.1f ms  caused lates %.2f %%\n", pl0.engaged, pl0.meanD, pl0.causedLates, ps0.engaged, ps0.meanD, ps0.causedLates);
        std::printf("  slack falls by 6 ms at step 3000: PAD_LATE caused lates %u, PHASE_SYNC %u (delay at the end %.1f ms)\n", pld.causedAfterDrop, psd.causedAfterDrop, psd.dAfterDrop);
    }
    Require(ps0.engaged >= 70.0, "controller: with 2 % late pictures that no delay causes, phase sync is on at least 70 % of the steps");
    Require(ps0.engaged >= pl0.engaged + 30.0, "controller: ... and 30 points more often than PAD_LATE (whose hold-off follows every late picture)");
    Require(ps0.meanD >= 6.0 && ps0.meanD <= 8.0, "controller: the delay settles at the thread's idle time less the margin (6 to 8 ms of 10 - 2.5)");
    Require(ps0.causedLates < 0.5, "controller: the delay itself causes under 0.5 % late pictures");
    Require(psq.causedLates < 0.3, "controller: a quiet scene: the delay causes under 0.3 % late pictures");
    Require(psd.causedAfterDrop <= 12, "controller: the slack falls by 6 ms: at most 12 late pictures before the delay is down");
    Require(psd.dAfterDrop <= 4.5, "controller: ... and the delay settles under the new idle time (4 ms) less the margin");
    const SynthOut bug = Synth(1, 0.0, 6000, 0, 0, 14000, 10000, 7, true);
    std::printf("harness negative control: caused lates %.2f %% (delay mean %.1f ms)\n", bug.causedLates, bug.meanD);
    Require(bug.causedLates > 0.5, "harness negative control: a controller fed idle time that includes its own sleep over-delays and the harness sees the late pictures");
    Require(psd.widenings >= 1, "controller: a run of late pictures that the delay caused widens the margin");
    {   // the back-off: the second of two late pictures in a row with a delay in force halves it, and it does not rise again for the hold-off
        npfps::PhaseSync q;
        npfps::PhaseConfig c;
        q.Configure(c);
        for (int i = 0; i < 400; ++i) q.Step(14000 - q.DelayNow(), 10000, false);
        const uint32_t before = q.DelayNow();
        q.Step(14000, 10000, true);
        q.Step(14000, 10000, true);
        const uint32_t after = q.DelayNow();
        for (int i = 0; i < 5; ++i) q.Step(14000, 10000, false);
        Require(before >= 6000, "controller: back-off test: the delay settled first");
        Require(after * 10 <= before * 6, "controller: back-off: two late pictures in a row halve the delay at once (the first may be a spike)");
        Require(q.DelayNow() <= after, "controller: back-off: the delay does not rise during the hold-off");
    }
    // 2. the pipeline: nothing slower, nothing more uneven (model estimates; these are also what the console run is judged by)
    for (int s = 0; s < 5; ++s) {
        if (s == 2) continue;   // (the depth-1 run: printed only)
        const Result& c = res[s][0];
        const Result& ph = res[s][3];
        std::snprintf(w, sizeof w, "scene %d: the control holds an even 30 (fps over 29, 3+ holds under 5 %%)", s);
        Require(c.fps > 29.0 && c.h3 + c.h4 < 5.0, w);
        std::snprintf(w, sizeof w, "scene %d: phase sync does not slow the game (speed >= 99.5 %%)", s);
        Require(ph.speed >= 99.5, w);
        std::snprintf(w, sizeof w, "scene %d: phase sync adds no stutter (3+ holds at most 0.5 points above the control)", s);
        Require(ph.h3 + ph.h4 <= c.h3 + c.h4 + 0.5, w);
        std::snprintf(w, sizeof w, "scene %d: phase sync keeps the frame rate (within 0.3 fps of the control)", s);
        Require(ph.fps >= c.fps - 0.3, w);
        std::snprintf(w, sizeof w, "scene %d: phase sync press-to-picture mean within +1.5 ms of the control (the model finds -1 .. +1)", s);
        Require(ph.mean <= c.mean + 1.5, w);
    }
    // the depth-1 run of r10: what the model does reproduce is that depth 1 on the OLD gate is not better than depth 2 on the press
    Require(res[2][6].mean >= res[2][7].mean - 1.0, "old gate at depth 1 does not beat depth 2 on the press (r10 console: 141 against 110)");
    std::printf("pace10_unit: %s\n", g_fail ? "FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
