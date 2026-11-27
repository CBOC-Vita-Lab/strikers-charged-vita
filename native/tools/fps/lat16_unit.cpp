// lat16: the pad read aligned to the controller's samples (np_lat16.h, MSC_LAT_SAMPLE) on a MODEL of the console's held 30: every figure is an estimate built from the r13 console logs
// (w8/press16.md): frame wake at a retrace, pad -> GPU done 68 ms mean (22 game + 18 send/decode + 10 present + ~18 GPU), the picture shown 7 refreshes after the wake (117 ms; [present]
// "shown-minus-start 7.07"), the display callback entered no earlier than one refresh before the previous picture's flip (its serialisation behind the previous gate wait), the controller
// sampling once a period at a phase of its own. The controller code is the REAL header; the legacy comparator is np_feel.h PadLate fed what the code feeds it today (handed -> shown).
//   g++ -std=c++17 -O2 -Inative/platform/compat native/tools/fps/lat16_unit.cpp -o /tmp/lat16_unit && /tmp/lat16_unit [--table]
// Mutants: -DNP_LAT16_BREAK=1..5 (np_lat16.h; each must fail a check below).
#include "../../platform/sdk/input/np_feel.h"
#include "np_lat16.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {
int g_fail = 0;
void Require(bool ok, const char* why) { if (!ok) { std::printf("FAIL: %s\n", why); ++g_fail; } }
constexpr double Pf = 16683.33;   // our refresh (the frame grid and the display)
constexpr double P = Pf;          // (kept for the display arithmetic below)

struct Result {
    double trueMean = 0, trueP95 = 0, latMean = 0;   // press -> picture (true: from the press to the screen), pad read -> picture ([lat]'s measure)
    double lates = 0, waitMean = 0, hitRate = 1, waitShare = 0, maxWait = 0;
    uint32_t waits = 0;
};
enum class Mode { None, Legacy, Aligned };
struct Scene { double chainMean = 68000, chainSd = 6000, spikeP = 0.03, spikeMs = 25000, cpuIdle = 11000, shownAfter = 7; unsigned episode = 0; };

Result Run(Mode mode, double phase, const Scene& sc, double hwP = Pf, unsigned frames = 6000, unsigned seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nrm(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    npl16::Latch lat;
    lat.room = npl16::Room();
    NpFeel::PadLate old;
    const double Z = 7.0e9;   // the controller's clock is not ours
    auto hwStamp = [&](double now) {   // newest sample at `now`: the arrivals are phase + j P (a little jitter on each)
        const double j = std::floor((now - 100000.0 - phase) / hwP);   // (the phase is relative to the first frame's retrace; hwP != Pf: the two clocks drift against each other)
        return 100000.0 + phase + j * hwP;
    };
    std::vector<double> F(frames + 8, 0), D(frames + 8, 0), usedH(frames + 8, 0), Tread(frames + 8, 0), wv(frames + 8, 0);
    std::vector<char> late(frames + 8, 0);
    uint32_t nWaits = 0, nHits = 0, nLate = 0;
    double maxWait = 0;
    double sumWait = 0, sumLat = 0;
    double prevF = 0;
    uint32_t oldDelay = 0;
    for (unsigned k = 0; k < frames; ++k) {
        const double v = 100000.0 + k * 2 * P;
        const double wake = v + 300.0;
        double w = 0;
        // the shown picture of frame k-4 is known by now
        if (k >= 6) {
            const unsigned j = k - 4;
            const double Ej = std::max(D[j], j ? F[j - 1] - P + 200.0 : 0.0);
            const double slackLb = std::max(0.0, F[j] - Ej);
            const double gateExit = F[j] - P + 100.0;   // handed: the gate's vblank wait returned, the picture goes on the next vblank
            const double slackOld = std::max(0.0, F[j] - gateExit);
            const bool isLate = late[j] != 0;
            const double cpuPrev = 2 * P - 22000.0 + wv[k - 1] + 500.0 * nrm(rng) * 0;
            if (mode == Mode::Aligned) lat.Observe(true, (uint32_t)slackLb, (uint32_t)std::max(0.0, cpuPrev), isLate);
            if (mode == Mode::Legacy) {
                const double cpu = std::max(0.0, cpuPrev - old.delay);
                old.Observe((uint32_t)std::min(slackOld, cpu), isLate);
            }
        }
        double tnow = wake + 50.0;
        if (mode == Mode::Aligned) {
            const double t0 = tnow;
            const double ts0 = hwStamp(t0);
            w = npl16::DoWait(lat, [&]() -> uint64_t { return (uint64_t)tnow; }, [&]() -> uint64_t { return (uint64_t)(Z + hwStamp(tnow)); },
                              [&](uint32_t us) { tnow += us + (npl16::kPeriodNomUs ? 400.0 * uni(rng) : 0.0); });   // a sleep ends up to 0.4 ms late
            if (w > 0) { ++nWaits; if (hwStamp(tnow) > ts0) ++nHits; sumWait += w; maxWait = std::max(maxWait, (double)w); }
            w = tnow - t0;
        } else if (mode == Mode::Legacy) {
            w = old.DelayUs();
            oldDelay = old.delay;
        }
        (void)oldDelay;
        wv[k] = w;
        const double T = wake + 50.0 + w;
        Tread[k] = T;
        const double h = hwStamp(T);
        usedH[k] = h;
        if (mode == Mode::Aligned) lat.OnRead((uint64_t)(Z + h), (uint64_t)T);
        if (mode == Mode::Legacy && w > 0) { ++nWaits; sumWait += w; }
        double chain = sc.chainMean + sc.chainSd * nrm(rng);
        if (uni(rng) < sc.spikeP) chain += sc.spikeMs;
        if (sc.episode && (k / sc.episode) % 3 == 2) chain += 12000.0;   // a slow stretch (every third block): the GPU takes longer
        D[k] = T + chain;
        double f = v + sc.shownAfter * P;
        if (f < prevF + P) f = prevF + P;   // (a late picture is shown one refresh late; the next one keeps its own slot, a one-refresh hold)
        if (D[k] > f - 1000.0) { f += P; late[k] = 1; ++nLate; }
        F[k] = f;
#ifdef LAT16_DBG
        if (k > 100 && k < 106) std::printf("k%u v%.0f T%.0f w%.0f D%.0f F%.0f late%d\n", k, v, T, w, D[k], f, (int)late[k]);
#endif
        prevF = f;
    }
#ifdef LAT16_DBG
    if (mode == Mode::Aligned) std::printf("st: notReady %u noRoom %u noFit %u waits %u allowed %u margin %u lastTs ready %d per %u\n", lat.st.skipNotReady, lat.st.skipNoRoom, lat.st.skipNoFit, lat.st.waits, lat.room.AllowedUs(), lat.room.margin, (int)lat.sc.Ready(), lat.sc.PeriodUs());
#endif
    Result r;
    std::vector<double> lats;
    lats.reserve(frames * 70);
    double prevH = usedH[0];
    for (unsigned k = 1; k < frames; ++k) {
        const double hk = usedH[k];
        if (hk <= prevH) continue;   // (a read that got the same sample as the one before: its presses were seen earlier)
        for (double p = prevH + 125.0; p <= hk; p += 250.0) lats.push_back(F[k] - p);
        prevH = hk;
        sumLat += F[k] - Tread[k];
    }
#ifdef LAT16_DBG
    { double a=0; unsigned n=0; for (unsigned k=1;k<frames;++k){a+=F[k]-Tread[k];++n;} std::printf("mode %d direct mean F-T %.1f sumLat %.1f\n",(int)mode,a/n,sumLat/(frames-1)); }
#endif
    std::sort(lats.begin(), lats.end());
    double s = 0;
    for (double x : lats) s += x;
    r.trueMean = s / lats.size();
    r.trueP95 = lats[lats.size() * 95 / 100];
    r.latMean = sumLat / (frames - 1);
    r.lates = 100.0 * nLate / frames;
    r.waits = nWaits;
    r.waitShare = 100.0 * nWaits / frames;
    r.waitMean = nWaits ? sumWait / nWaits : 0;
    r.maxWait = maxWait;
    r.hitRate = nWaits ? (double)nHits / nWaits : 1.0;
    return r;
}
const char* Name(Mode m) { return m == Mode::None ? "no latch " : m == Mode::Legacy ? "PadLate  " : "aligned  "; }
} // namespace

int main(int argc, char** argv) {
    const bool table = argc > 1 && std::string(argv[1]) == "--table";
    Scene base;
    // ---- 1. the stamp clock alone: period, offset from reads, offset from a find
    {
        npl16::StampClock sc;
        std::mt19937 rng(3);
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        const double Z = 123456789012.0, ph = 6000, hwP = 16690.0;
        double now = 1.0e6;
        for (int i = 0; i < 80; ++i) {
            now += 33380.0 + 3000.0 * uni(rng);   // reads at all phases of the controller's period
            const double h = ph + std::floor((now - ph) / hwP) * hwP;
            sc.Observe((uint64_t)(Z + h), (uint64_t)now);
        }
        Require(sc.Ready(), "the clock is learned after 80 reads");
        Require(std::fabs((double)sc.PeriodUs() - hwP) <= 12, "the period is learned within 12 us");
        const double h = ph + std::floor((now - ph) / hwP) * hwP;
        const int64_t age = sc.AgeUs((uint64_t)(Z + h), (uint64_t)now);
        Require(std::fabs((double)age - (now - h)) < 2500.0, "the age of a read is within 2.5 ms when the reads cover the period");
        // reads that all fall at the same phase hide the offset: the age reads 0 (a lower bound), a find makes it exact
        npl16::StampClock lock;
        double t = 2.0e6;
        for (int i = 0; i < 60; ++i) {
            t += 2 * Pf;
            const double hh = 4000 + std::floor((t - 4000) / Pf) * Pf;   // always 12.7 ms old at the read
            lock.Observe((uint64_t)(Z + hh), (uint64_t)t);
        }
        const double hh = 4000 + std::floor((t - 4000) / Pf) * Pf;
        Require(lock.AgeUs((uint64_t)(Z + hh), (uint64_t)t) < 500, "model check: reads at one phase cannot see their age without a find");
        lock.Detected((uint64_t)(Z + hh + Pf), (uint64_t)(hh + Pf + 150));   // a poll saw the next sample 0.15 ms after it came
        Require(std::fabs((double)lock.AgeUs((uint64_t)(Z + hh), (uint64_t)t) - (t - hh)) < 400.0, "a find makes the age exact (0.4 ms)");
        npl16::StampClock fresh;
        Require(!fresh.Ready(), "nothing is trusted before the clock is learned");
        npl16::StampClock bad;
        for (int i = 0; i < 60; ++i) bad.Observe((uint64_t)(i * 2.0 * 16683.0 / 4.0) + 5, (uint64_t)(i * 33366.0));
        Require(!bad.Ready(), "a stamp clock that runs at another rate is refused");
    }
    // ---- 2. the room: absolute, backs off after a late picture, margin grows and decays
    {
        npl16::Room rm;
        for (int i = 0; i < 40; ++i) rm.Observe(true, 30000, 12000, false, 0);
        Require(rm.AllowedUs() == 9000, "room = smallest of slack and thread idle (12 ms) less the 3 ms margin");
        rm.Observe(true, 30000, 12000, true, 0);
        Require(rm.AllowedUs() == 0 && rm.Hold() == 30, "a late picture closes the room for 30 pictures");
        Require(rm.margin == 4000, "and widens the margin by 1 ms");
        for (int i = 0; i < 30; ++i) rm.Observe(true, 30000, 12000, false, 0);
        Require(rm.AllowedUs() == 0, "the window must fill again before a wait is allowed");
        for (int i = 0; i < 30; ++i) rm.Observe(true, 30000, 12000, false, 0);
        Require(rm.AllowedUs() > 0, "the room opens again after the hold and a full window");
        npl16::Room t;
        for (int i = 0; i < 40; ++i) t.Observe(true, 6000, 12000, false, 0);
        Require(t.AllowedUs() == 3000, "a 6 ms slack leaves 3 ms");
        npl16::Room t2;
        for (int i = 0; i < 40; ++i) t2.Observe(true, 2500, 12000, false, 0);
        Require(t2.AllowedUs() == 0, "less than the margin leaves nothing");
        npl16::Room t3;   // the slack seen includes the waits made: add back the smallest of the last 4
        for (int i = 0; i < 40; ++i) t3.Observe(true, 5000, 20000, false, 6000);
        Require(t3.AllowedUs() == 8000, "the waits made are added back to the slack seen (5 + 6 - 3 ms)");
    }
    // ---- 3. the console model: the controller locked to the retrace at six phases (where its sample arrives relative to the frame's start), and drifting against it
    const double phases[] = {200, 2500, 4000, 6500, 9000, 14000};
    Result none[6], al[6], lg[6];
    for (int i = 0; i < 6; ++i) {
        none[i] = Run(Mode::None, phases[i], base);
        lg[i] = Run(Mode::Legacy, phases[i], base);
        al[i] = Run(Mode::Aligned, phases[i], base);
        if (table)
            std::printf("phase %5.0f us | %s true mean %6.1f p95 %6.1f [lat] %6.1f lates %.2f%% | %s true mean %6.1f p95 %6.1f [lat] %6.1f lates %.2f%% waits %.0f%% (%.1f ms) | %s true mean %6.1f p95 %6.1f [lat] %6.1f lates %.2f%% waits %.0f%% (%.1f ms, max %.1f) found %.0f%%\n",
                        phases[i], Name(Mode::None), none[i].trueMean / 1000, none[i].trueP95 / 1000, none[i].latMean / 1000, none[i].lates,
                        Name(Mode::Legacy), lg[i].trueMean / 1000, lg[i].trueP95 / 1000, lg[i].latMean / 1000, lg[i].lates, lg[i].waitShare, lg[i].waitMean / 1000,
                        Name(Mode::Aligned), al[i].trueMean / 1000, al[i].trueP95 / 1000, al[i].latMean / 1000, al[i].lates, al[i].waitShare, al[i].waitMean / 1000, al[i].maxWait / 1000, 100 * al[i].hitRate);
    }
    // phases where a sample arrives inside the room after the frame's start: the read takes a whole controller period off the delay a press really sees
    for (int i : {1, 2, 3}) {
        Require(none[i].trueMean - al[i].trueMean > 12500.0, "aligned: a sample inside the room, the press-to-picture falls by more than 12.5 ms");
        Require(al[i].hitRate >= 0.97, "aligned: the read after a wait gets a newer sample (at least 97 % of waits)");
        Require(al[i].lates <= none[i].lates + 0.3, "aligned: no more late pictures than without (0.3 % allowed)");
        Require(al[i].maxWait <= 9000.0, "aligned: no wait is longer than the room");
    }
    // phases where none fits: it looks again every 12th frame, otherwise no wait, no change, nothing lost
    for (int i : {0, 4, 5}) {
        Require(std::fabs(none[i].trueMean - al[i].trueMean) < 700.0, "aligned: no sample in the room, the delay is unchanged");
        Require(al[i].waitShare < 15.0, "aligned: no wait is made when it would cross no arrival (a look every 12th frame)");
        Require(al[i].lates <= none[i].lates + 0.3, "aligned: no sample in the room, no new late pictures");
    }
    // the late latch today: [lat] shows its gain, the press does not (it is a staircase of the controller's period)
    {
        int shows = 0, real = 0;
        for (int i = 0; i < 6; ++i) { if (lg[i].latMean < none[i].latMean - 5000.0) ++shows; if (lg[i].trueMean < none[i].trueMean - 5000.0) ++real; }
        if (table) std::printf("late latch today: [lat] shorter by over 5 ms at %d of 6 phases, the press at %d of 6\n", shows, real);
        Require(shows == 6 && real < 6, "model check: the late latch shortens [lat] at every phase but the press only at some");
    }
    // drifting: the controller's clock and the retrace slip against each other (7 us a period: all phases in a 6000-frame run)
    {
        const Result n = Run(Mode::None, 0, base, 16690.0), a = Run(Mode::Aligned, 0, base, 16690.0);
        if (table) std::printf("drifting phase | none true %.1f lates %.2f%% | aligned true %.1f lates %.2f%% waits %.0f%% (%.1f ms) found %.0f%%\n", n.trueMean / 1000, n.lates, a.trueMean / 1000, a.lates, a.waitShare, a.waitMean / 1000, 100 * a.hitRate);
        Require(n.trueMean - a.trueMean > 4000.0, "aligned: with the phase drifting through every value the press falls by more than 4 ms on average (it takes 16.7 ms at about 40 % of the phases)");
        Require(a.lates <= n.lates + 0.3, "aligned: drifting phase, no more late pictures");
    }
    // a slow stretch (the GPU +12 ms for a third of the time): the back-off keeps the late pictures down
    {
        Scene s = base;
        s.episode = 200;
        s.chainMean = 76000;
        const Result n = Run(Mode::None, 4000, s), a = Run(Mode::Aligned, 4000, s);
        if (table) std::printf("slow stretches | none lates %.2f%% true %.1f | aligned lates %.2f%% true %.1f waits %.0f%%\n", n.lates, n.trueMean / 1000, a.lates, a.trueMean / 1000, a.waitShare);
        Require(a.lates <= n.lates + 1.0, "aligned: slow stretches cost at most 1 % more late pictures");
        Require(a.trueMean <= n.trueMean + 500.0, "aligned: slow stretches, never worse on average");
    }
    // a tight pipeline (shown 6 refreshes after the wake): the room is small, the late pictures must stay close to none
    {
        Scene s = base;
        s.shownAfter = 6;
        s.chainMean = 70000;
        const Result n = Run(Mode::None, 4000, s), a = Run(Mode::Aligned, 4000, s);
        if (table) std::printf("tight pipeline | none lates %.2f%% true %.1f | aligned lates %.2f%% true %.1f waits %.0f%%\n", n.lates, n.trueMean / 1000, a.lates, a.trueMean / 1000, a.waitShare);
        Require(a.lates <= n.lates + 1.0, "aligned: a tight pipeline costs at most 1 % more late pictures");
        Require(a.trueMean <= n.trueMean + 500.0, "aligned: a tight pipeline, never worse on average");
    }
    // the plan does not leave the room when the controller stops delivering (a learned arrival that no longer comes); a clock that does not advance does not hold the thread
    {
        npl16::Latch l;
        for (int i = 0; i < 40; ++i) l.Observe(true, 30000, 11000, false);   // room 8 ms
        l.pt.End(true, false, 7000);   // an arrival learned at 7 ms
        uint64_t t = 1000;
        const uint32_t w = npl16::DoWait(l, [&]() -> uint64_t { return t; }, []() -> uint64_t { return 77; }, [&](uint32_t us) { t += us; });
        Require(w > 0 && w <= 8000 + 300, "a learned arrival that does not come: the wait ends with the room (8 ms), not after it");
        uint64_t t2 = 5000;
        npl16::Latch l2;
        const uint32_t w2 = npl16::DoWait(l2, [&]() -> uint64_t { return t2; }, [&]() -> uint64_t { return 77; }, [&](uint32_t us) { t2 += us; });
        Require(w2 == 0, "no room learned yet: no wait");
        // an unchanged stamp is not a find
        npl16::Latch l3;
        for (int i = 0; i < 40; ++i) l3.Observe(true, 30000, 11000, false);
        uint64_t t3 = 9000;
        npl16::DoWait(l3, [&]() -> uint64_t { return t3; }, []() -> uint64_t { return 55; }, [&](uint32_t us) { t3 += us; });
        Require(l3.st.finds == 0, "a stamp that does not change is not a find");
    }
    {
        npl16::Latch l;
        for (int i = 0; i < 40; ++i) l.Observe(true, 30000, 14000, false);
        uint64_t t = 1000;
        const uint32_t w = npl16::DoWait(l, [&]() -> uint64_t { return t; }, []() -> uint64_t { return 77; }, [&](uint32_t) {});
        Require(w == 0, "a clock that never advances: the wait ends by itself");
    }
    if (g_fail) { std::printf("lat16_unit: %d FAILED\n", g_fail); return 1; }
    std::printf("lat16_unit: all checks passed (model figures are estimates)\n");
    return 0;
}
