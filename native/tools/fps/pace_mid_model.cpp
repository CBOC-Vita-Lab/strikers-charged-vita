// Between 30 and 60: AUTO's held 30, policy 60, MSC_FPS=unlocked, MSC_FPS_BETWEEN=pace and pace with MSC_PRESENT_SYNC=2 on a model of the console's pipeline. A MODEL, not a console
// measurement: the stage costs are inputs (the default table's busiest stage goes through the range; docs' prediction for gameplay, np_fps.cpp kPredRows, is about 25 ms on the
// back-end core and has never been measured). The game thread's waits are copies of glxSwap.cpp swap_Post and np_fps.cpp (late wait, frames in flight), keep in step; PresentSync is
// the real class (renderer/src/vgx_present_sched.h), with its unit checks first. Content time is the start of the frame: the game interpolates its 50 Hz steps (FixedUpdateTask.cpp
// mInterpolationDeltaT), so the world on a picture is the world at the moment its frame began.
//   Prints per scene and policy: pictures a second, holds, judder (|vblanks held - vblanks of game time|, Galaxy's figure), motion error (rms of shown time - content time, around
//   its mean: 0 = every picture exactly where the clock says), delay from the frame's start (the pad read) to the screen, and the game's speed.
//   g++ -std=c++17 -O2 -Irenderer/src native/tools/fps/pace_mid_model.cpp -Inative/platform/compat -o /tmp/pace_mid_model && /tmp/pace_mid_model [--table]
#include "vgx_present_sched.h"
#include "vgx_upbudget.h"
#include "../../platform/sdk/input/np_feel.h"
#include "np_fps_between.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

using vgx::present::PresentSync;

namespace {
constexpr double kV = 1001.0 / 60.0;   // ms per vblank
constexpr double kStep = 20.0;         // the simulation's fixed step

int g_failures = 0;
void Require(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL: %s\n", why); ++g_failures; }
}

#ifndef PACE_BREAK
#define PACE_BREAK 0   // wave 9 mutants of the hold-mix checks: 1 the aligned policy keeps the plain frames-in-flight depth 2, 2 it is only measured (no alignment)
#endif
enum class Swap { Held30, Sixty, Free };
struct Policy { const char* name; Swap swap; int fifDepth; int sync; bool gate = false; int pls = 0; bool adapt = false; };   // wave 10: pls: phase-locked start (margin in tenths of a sigma + 1 ms), adapt: depth 3/4 by the work of the last second
double g_jitter = 0.12;   // the spread of every stage's cost (a fraction of its mean)   // sync: 0 no, 1 measured, 2 aligned; gate: np_fps_between.h moves between held 30 and free
const Policy kPolicies[] = {
    {"held 30 (AUTO today)", Swap::Held30, 2, 0},
    {"policy 60 (MSC_FPS=60)", Swap::Sixty, 0, 0},
    {"MSC_FPS=unlocked", Swap::Free, 0, 0},
    {"free, depth 3, measured", Swap::Free, 3, 1},
    {"free, depth 2, aligned", Swap::Free, 2, 2},
    {"free, depth 3, aligned", Swap::Free, PACE_BREAK == 1 ? 2 : 3, PACE_BREAK == 2 ? 1 : 2},
    {"MSC_FPS_BETWEEN=pace (the gate)", Swap::Free, 3, 2, true},
};

struct Scene { const char* name; double game, front, back, gpu; };   // mean ms per picture: game thread, send + decode, back end, GPU

struct Result {
    double fps, judder, motion, delay, delayP95, speed, late, pacedShare;
    int entries;
    uint64_t hold[6];
    unsigned L;
};

uint32_t Vc(double t) { return static_cast<uint32_t>(std::floor(t / kV)); }
double NextVblank(double t) { return (Vc(t) + 1) * kV + 0.02; }   // (a wait returns just after the vblank)

Result Run(const Policy& pol, const Scene& sc, unsigned seed, double seconds = 120.0) {
    std::mt19937 rng(seed);
    auto cost = [&](double mean) {
        std::normal_distribution<double> d(mean, g_jitter * mean);
        const double spike = std::uniform_real_distribution<double>(0, 1)(rng) < 0.03 ? 0.4 * mean : 0.0;   // (a goal flash, a shader, the collector)
        const double v = d(rng) + spike;
        return v < 0.4 * mean ? 0.4 * mean : v;
    };
    PresentSync sync;
    sync.Configure(pol.sync == 2, 1, 6, 4, 4);
    double t = 0.3, frontFree = 0, beFree = 0, gpuFree = 0, cbFree = 0;
    uint32_t retraceAtSwap = 0, anchor = 0, lastShown = 0, sendRef = 0;
    bool haveAnchor = false, haveShown = false, fifHave = false;
    int fifPaused = 0;
    uint32_t fifLast = 0;
    std::deque<double> cbReturn;
    std::vector<double> shownAt;        // when frame k's picture was reported shown (the display callback's return)
    std::vector<double> err, delays;
    uint64_t pictures = 0, late = 0, sent = 0;
    uint64_t hold[6] = {};
    double judderSum = 0, firstStart = -1, lastStart = 0, acc = 0, prevStart = -1;
    uint64_t pairs = 0, ticks = 0;
    int prevShownV = -1, prevStamp = -1;
    npfps::Between between;
    bool free = pol.swap == Swap::Free && !pol.gate;
    uint64_t pacedFrames = 0;
    double lastLen = 0, lastBusy = 0;
    double wEma = 0, wDev = 0, workEma = 0;
    int adaptDepth = 3;
    int plsHold = 0;
    while (t < seconds * 1000.0) {
        const double start = t;
        if (pol.gate) {
            const bool was = free;
            free = between.Frame(start / 1000.0, 2, true, lastBusy, lastLen);
            if (was != free) { haveAnchor = false; fifHave = false; }
        }
        if (free) ++pacedFrames;
        const Swap swap = pol.gate ? (free ? Swap::Free : Swap::Held30) : pol.swap;
        if (prevStart >= 0) lastLen = start - prevStart;
        const uint32_t stamp = Vc(start);
        if (firstStart < 0) firstStart = start;
        lastStart = start;
        if (prevStart >= 0) {   // the simulation: fixed steps from the frame's real length
            acc += start - prevStart;
            while (acc >= kStep) { acc -= kStep; ++ticks; }
        }
        prevStart = start;
        const double gameMs = cost(sc.game), frontMs = cost(sc.front), backMs = cost(sc.back), gpuMs = cost(sc.gpu);
        lastBusy = std::fmax(gameMs, std::fmax(frontMs, backMs));   // (what the per-core counters see; not the GPU)
        {   // wave 10: what the pipeline needs for one picture when it is empty (the measured submit-to-done time), and the busiest stage as a fraction of a refresh
            const double g = gpuMs;
            const double w = gameMs + frontMs + std::fmax(backMs, g);
            wDev = wEma > 0 ? 0.9 * wDev + 0.1 * std::fabs(w - wEma) : 0;
            wEma = wEma > 0 ? 0.9 * wEma + 0.1 * w : w;
            const double busy = std::fmax(std::fmax(gameMs, frontMs), std::fmax(backMs, g)) / kV;
            workEma = workEma > 0 ? 0.98 * workEma + 0.02 * busy : busy;
            if (pol.adapt) { if (adaptDepth == 3 && workEma >= 0.97) adaptDepth = 4; else if (adaptDepth == 4 && workEma < 0.88) adaptDepth = 3; }
        }
        const double built = start + gameMs;
        double hand = built;
        if (swap == Swap::Sixty) hand = NextVblank(built);
        else if (swap == Swap::Free) {   // swap mode 3
            const double sinceRetrace = built - Vc(built) * kV;
            if (sinceRetrace > 12.0 || Vc(built) == retraceAtSwap) hand = NextVblank(built);
        }
        retraceAtSwap = Vc(hand);
        // The front end takes one frame at a time; the hand-over of the next waits for the one before it.
        const double front0 = std::fmax(hand, frontFree), front1 = front0 + frontMs;
        const double be0 = std::fmax(front1, beFree);
        frontFree = be0;   // (a finished frame waits in the front end until the back end takes it: no stage has a queue of its own)
        double be1 = be0 + backMs;
        if (cbReturn.size() >= 2) be1 = std::fmax(be1, cbReturn[cbReturn.size() - 2]);   // two pictures in the display queue
        beFree = be1;
        const double gpu0 = std::fmax(be0, gpuFree), gpu1 = std::fmax(gpu0 + gpuMs, be1);
        gpuFree = gpu1;
        const double cb = std::fmax(gpu1, cbFree);
        const uint32_t earliest = Vc(cb) + 1;
        uint32_t target = earliest;
        if (swap == Swap::Held30 && haveShown && target < lastShown + 2) target = lastShown + 2;   // GateDisplay
        if (swap == Swap::Sixty && haveShown && target < lastShown + 1) target = lastShown + 1;
        if (pol.sync) {
            sync.SetAlign(pol.sync == 2 && swap == Swap::Free);   // (np_fps.cpp NpFpsPresentSyncMode: policy 30 is only measured)
            const uint32_t s = sync.Target(earliest, true, static_cast<uint16_t>(stamp));
            if (s > target) target = s;
        }
        if (pol.sync) sync.Shown(target, target != earliest);
        if (haveShown && swap == Swap::Held30 && target > lastShown + 2) ++late;
        if (pol.pls && haveShown && static_cast<int>(target) - static_cast<int>(lastShown) >= 2) plsHold = 120;   // a picture a refresh late: no waiting for 120 pictures (np_feel.h PadLate's rule)
        lastShown = target;
        haveShown = true;
        const double shownTime = target * kV;
        cbFree = target != earliest ? (target - 1) * kV + 0.2 : cb + 0.1;
        cbReturn.push_back(cbFree);
        if (cbReturn.size() > 4) cbReturn.pop_front();
        shownAt.push_back(cbFree);
        ++sent;
        ++pictures;
        err.push_back(shownTime - start);
        delays.push_back(shownTime - start);
        if (prevShownV >= 0) {
            const int h = static_cast<int>(target) - prevShownV, st = static_cast<int>(stamp) - prevStamp;
            ++hold[h < 5 ? h : 5];
            judderSum += std::abs(h - st);
            ++pairs;
        }
        prevShownV = static_cast<int>(target);
        prevStamp = static_cast<int>(stamp);
        // The game thread goes on: the send took front0 (the fork waits for the job before it).
        double next = front0;
        if (swap == Swap::Held30) {   // NpFpsLateWaitAfterSend
            sendRef = haveAnchor ? anchor + 2 : retraceAtSwap + 2;
            while (Vc(next) < sendRef) next = NextVblank(next);
        }
        const uint32_t preGate = Vc(next);
        const int depth = pol.gate && !free ? 2 : (pol.adapt ? adaptDepth : pol.fifDepth);
        if (depth) {   // FramesInFlightGate (the cap-hit cut-off is left out: it never fires here)
            const bool lateFrame = fifHave && static_cast<int>(Vc(next) - fifLast) > 2;
            if (lateFrame) fifPaused = 150;
            if (fifPaused) { --fifPaused; }
            else {
                size_t shownTag = 0;
                for (size_t i = shownAt.size(); i > 0; --i) if (shownAt[i - 1] <= next) { shownTag = i; break; }
                if (sent > shownTag + depth) next = NextVblank(next);
            }
            fifLast = preGate;
            fifHave = true;
        }
        anchor = preGate;
        haveAnchor = true;
        if (pol.pls && swap == Swap::Free && wEma > 0) {   // wave 10: begin the next picture as late as still makes the refresh after this one (ready <= the vblank before it), by the measured pipeline time and its spread
            if (plsHold > 0) --plsHold;
            else {
                const double margin = 0.1 * pol.pls * wDev + 1.0;
                const double startAt = lastShown * kV - wEma - margin;
                if (next < startAt) next = startAt;
            }
        }
        t = next;
    }
    Result r{};
    const double span = (lastStart - firstStart) / 1000.0;
    r.fps = pictures / span;
    r.judder = pairs ? judderSum / pairs : 0;
    double mean = 0;
    for (double e : err) mean += e;
    mean /= err.size();
    double sq = 0;
    for (double e : err) sq += (e - mean) * (e - mean);
    r.motion = std::sqrt(sq / err.size());
    r.delay = mean;
    std::sort(delays.begin(), delays.end());
    r.delayP95 = delays[delays.size() * 95 / 100];
    r.speed = ticks / span / 50.0;
    r.late = pairs ? 100.0 * late / pairs : 0;
    std::memcpy(r.hold, hold, sizeof hold);
    r.L = sync.Latency();
    r.pacedShare = static_cast<double>(pacedFrames) / pictures;
    r.entries = between.Entries();
    return r;
}

Result Mean(const Policy& p, const Scene& s) {
    Result m{};
    const unsigned seeds = 8;
    for (unsigned i = 1; i <= seeds; ++i) {
        const Result r = Run(p, s, i);
        m.fps += r.fps; m.judder += r.judder; m.motion += r.motion; m.delay += r.delay; m.delayP95 += r.delayP95; m.speed += r.speed; m.late += r.late;
        for (int h = 0; h < 6; ++h) m.hold[h] += r.hold[h];
        m.L = r.L;
        m.pacedShare += r.pacedShare;
        m.entries += r.entries;
    }
    m.fps /= seeds; m.judder /= seeds; m.motion /= seeds; m.delay /= seeds; m.delayP95 /= seeds; m.speed /= seeds; m.late /= seeds; m.pacedShare /= seeds; m.entries /= static_cast<int>(seeds);
    return m;
}

void UnitChecks() {   // (Galaxy's checks of the class; L's range as this port configures it)
    PresentSync s;
    s.Configure(true, 1, 5, 3);
    Require(s.Target(1003, true, 1000) == 1003, "a picture ready at its slot is not held");
    s.Shown(1003, false);
    Require(s.Target(1004, true, 1002) == 1005, "an early picture is held to stamp + latency");
    s.Shown(1005, true);
    Require(s.Target(1005, true, 1001) == 1006, "never two pictures in one vblank");
    s.Shown(1006, true);
    Require(s.Target(1007, true, static_cast<uint16_t>(1007 - 200)) == 1007, "an implausible stamp is shown when ready");
    s.Shown(1007, false);
    Require(s.Target(1008, false, 0) == 1008, "an unstamped picture is shown when ready");
    PresentSync m;
    m.Configure(false, 1, 5, 3);
    Require(m.Target(1004, true, 1002) == 1004, "measure only: never held");
    m.Shown(1004, false);
    m.SetAlign(true);
    Require(m.Aligned() && m.Target(1104, true, 1102) == 1105 && m.Latency() == 3, "SetAlign keeps L");
    PresentSync a;
    a.Configure(true, 1, 5, 3);
    uint32_t vb = 5000;
    for (int i = 0; i < 4; ++i, vb += 2) { const uint32_t t = a.Target(vb + 4, true, static_cast<uint16_t>(vb)); a.Shown(t, false); }
    Require(a.Latency() == 4, "raised after four late pictures");
    for (int i = 0; i < 2000; ++i, vb += 2) { const uint32_t t = a.Target(vb + 1, true, static_cast<uint16_t>(vb)); a.Shown(t, t != vb + 1); }
    Require(a.Latency() == 1, "lowered again when every picture is ready after one vblank");
    for (int i = 0; i < 50; ++i, vb += 2) { const uint32_t t = a.Target(vb + 7, true, static_cast<uint16_t>(vb)); a.Shown(t, false); }
    Require(a.Latency() == 1, "pictures beyond the largest latency do not raise it");
    {   // wave 7, MSC_PRESENT_SYNC_START: starts at 3 and settles in 32-picture windows; a raise ends the quick descent for good
        PresentSync f;
        f.Configure(true, 1, 6, 3, 4, true);
        uint32_t v = 9000;
        int n = 0;
        for (; n < 400 && f.Latency() > 1; ++n, v += 2) { const uint32_t t = f.Target(v + 1, true, static_cast<uint16_t>(v)); f.Shown(t, t != v + 1); }
        Require(f.Latency() == 1 && n <= 70, "fast start: from 3 to 1 in two windows of 32 pictures when every picture is ready after one vblank");
        PresentSync o;
        o.Configure(true, 1, 6, 4, 4, false);
        v = 9000;
        n = 0;
        for (; n < 2000 && o.Latency() > 1; ++n, v += 2) { const uint32_t t = o.Target(v + 1, true, static_cast<uint16_t>(v)); o.Shown(t, t != v + 1); }
        Require(o.Latency() == 1 && n >= 3 * 170, "(without it the same descent from 4 takes three windows of 180 pictures)");
        PresentSync r;   // a raise: the quick descent is over
        r.Configure(true, 1, 6, 3, 4, true);
        v = 9000;
        for (int i = 0; i < 4; ++i, v += 2) { const uint32_t t = r.Target(v + 4, true, static_cast<uint16_t>(v)); r.Shown(t, false); }
        Require(r.Latency() == 4, "fast start: raised after four late pictures");
        int m2 = 0;
        for (; m2 < 330 && r.Latency() > 3; ++m2, v += 2) { const uint32_t t = r.Target(v + 1, true, static_cast<uint16_t>(v)); r.Shown(t, t != v + 1); }
        Require(r.Latency() == 4 || m2 >= 300, "after a raise it does not come down inside the hold-off (300 pictures), and then at the slow rate");
        PresentSync q;   // a window with a late picture too many does not lower
        q.Configure(true, 1, 6, 3, 4, true);
        v = 9000;
        for (int i = 0; i < 32; ++i, v += 2) { const uint32_t t = q.Target(v + (i % 8 == 0 ? 3 : 1), true, static_cast<uint16_t>(v)); q.Shown(t, t != v + 1); }
        Require(q.Latency() >= 3, "a window in which pictures needed L - 1 is not lowered");
    }
    PresentSync w;   // the 16-bit stamp across its wrap
    w.Configure(true, 1, 5, 3);
    Require(w.Target(0x10001, true, 0xFFFF) == 0x10002, "a stamp across the 16-bit wrap");
}
}  // namespace

int main(int argc, char** argv) {
    UnitChecks();
    const Scene scenes[] = {
        {"busiest stage 18 ms (55 fps)", 12, 16, 18, 15},
        {"busiest stage 21 ms (48 fps)", 13, 18, 21, 17},
        {"busiest stage 24 ms (42 fps)", 14, 21, 24, 20},
        {"busiest stage 25 ms (38 fps)", 15, 23, 25, 22},
        {"busiest stage 30 ms (33 fps)", 16, 25, 30, 24},
        {"busiest stage 32 ms (31 fps)", 17, 26, 32, 25},
        {"busiest stage 38 ms (26 fps)", 18, 30, 38, 30},
        {"game thread 22 ms (45 fps)", 22, 18, 19, 17},
        {"GPU 31 ms, cores 20 ms", 14, 18, 20, 31},
    };
    for (const Scene& s : scenes) {
        if (argc > 1) std::printf("%s:\n", s.name);
        for (const Policy& p : kPolicies) {
            const Result r = Mean(p, s);
            if (argc > 1) {
                const double n = static_cast<double>(r.hold[0] + r.hold[1] + r.hold[2] + r.hold[3] + r.hold[4] + r.hold[5]);
                std::printf("  %-30s %5.1f fps  held 1/2/3/4+ %3.0f/%3.0f/%3.0f/%3.0f %%  judder %.2f  motion %4.1f ms  delay %5.1f p95 %5.1f ms  speed %.3f  L %u  paced %.0f %%\n", p.name, r.fps, 100 * r.hold[1] / n,
                            100 * r.hold[2] / n, 100 * r.hold[3] / n, 100 * (r.hold[4] + r.hold[5]) / n, r.judder, r.motion, r.delay, r.delayP95, r.speed, r.L, 100 * r.pacedShare);
            }
        }
    }
    // What the switch is for, on the model.
    const Policy& held = kPolicies[0];
    const Policy& sixty = kPolicies[1];
    const Policy& measured = kPolicies[3];
    const Policy& depth2 = kPolicies[4];
    const Policy& aligned = kPolicies[5];
    const Policy& gate = kPolicies[6];
    for (int i : {1, 2, 3}) {
        const Result h = Mean(held, scenes[i]), a = Mean(aligned, scenes[i]), m = Mean(measured, scenes[i]);
        Require(a.fps > h.fps * (i == 3 ? 1.15 : 1.25), "free running shows clearly more pictures than held 30 in a 37-48 fps scene");
        Require(a.delay <= h.delay + kV, "at no more than a refresh of added delay");
        Require(a.judder < 0.1 && a.judder < m.judder * 0.3, "sync takes the judder out of the free-running pictures");
        Require(a.motion <= m.motion, "and does not add motion error");
    }
    {
        const Result a = Mean(aligned, scenes[2]), d = Mean(depth2, scenes[2]);
        Require(d.fps < a.fps - 3.0, "the plain frames-in-flight depth throttles aligned pictures: why paced takes one more (break check: this row)");
    }
    {
        const Result a = Mean(aligned, scenes[7]), s = Mean(sixty, scenes[7]);
        Require(a.fps > s.fps * 1.2, "a game thread over a refresh: policy 60 falls towards whole refreshes, swap mode 3 does not");
    }
    for (int i : {4, 5}) {
        const Result h = Mean(held, scenes[i]), a = Mean(aligned, scenes[i]), g = Mean(gate, scenes[i]);
        Require(a.fps < h.fps * 1.1, "at 31-33 fps free running gives next to nothing");
        Require(a.hold[3] > h.hold[3], "and brings 3-refresh holds: why the gate stays at held 30 there");
        Require(g.pacedShare < 0.1, "the gate holds 30 under about 34 fps");
    }
    for (int i : {0, 1, 2, 3, 7}) Require(Mean(gate, scenes[i]).pacedShare > 0.9, "the gate paces from about 37 fps up");
    {
        const Result g = Run(gate, scenes[8], 1);
        Require(g.entries >= 2 && g.entries <= 4, "a GPU limit the core counters cannot see is tried, found out and left with a growing cool-down");
        Require(g.pacedShare < 0.2, "and spends its time held");
    }
    for (const Scene& s : scenes) {
        for (const Policy& p : kPolicies) {
            const Result r = Mean(p, s);
            Require(std::fabs(r.speed - 1.0) < 0.002, "the game's speed does not depend on the picture policy (no slip in this model)");
        }
    }
    {
        const Result h = Mean(held, scenes[0]);
        Require(h.fps > 29.5 && h.judder < 0.05 && h.hold[2] > 0 && h.hold[3] * 1000 < h.hold[2], "held 30 stays flat in a scene well over 30");
    }
    // Wave 9 (pacing, w8/pacing.md Q2 and Q4): the hold mix (1 / 2 / 3+ refreshes) the rules give when the busiest stage takes 1.0, 1.1, 1.2, 1.4 refreshes a picture, GPU-bound and game-thread-bound,
    // and when the GPU needs more than two refreshes (is the console's 15 Hz "exactly four refreshes" a GPU over two refreshes, or stacked waits?). Printed with the "hold" argument.
    {
        const Scene hs[] = {
            {"GPU 16.7 ms (1.0 refresh)", 10, 12, 14, 16.7}, {"GPU 18.4 ms (1.1)", 10, 12, 14, 18.4}, {"GPU 20 ms (1.2)", 10, 12, 14, 20.0}, {"GPU 23.4 ms (1.4)", 10, 12, 14, 23.4},
            {"game thread 16.7 ms (1.0)", 16.7, 12, 14, 14}, {"game thread 18.4 ms (1.1)", 18.4, 12, 14, 14}, {"game thread 20 ms (1.2)", 20.0, 12, 14, 14}, {"game thread 23.4 ms (1.4)", 23.4, 12, 14, 14},
            {"GPU 36 ms (2.2 refreshes), cores 20 ms", 14, 18, 20, 36}, {"GPU 40 ms (2.4 refreshes), cores 20 ms", 14, 18, 20, 40}, {"GPU 56 ms (3.4 refreshes), cores 20 ms", 14, 18, 20, 56},
        };
        const bool print = argc > 1 && argv[1] && std::strcmp(argv[1], "hold") == 0;
        double worst4[7] = {};
        for (const Scene& h : hs) {
            if (print) std::printf("%s:\n", h.name);
            for (size_t pi = 0; pi < sizeof kPolicies / sizeof kPolicies[0]; ++pi) {
                const Result r = Mean(kPolicies[pi], h);
                const double n = static_cast<double>(r.hold[0] + r.hold[1] + r.hold[2] + r.hold[3] + r.hold[4] + r.hold[5]);
                const double p3 = 100 * (r.hold[3]) / n, p4 = 100 * (r.hold[4] + r.hold[5]) / n;
                if (p4 > worst4[pi]) worst4[pi] = p4;
                if (print)
                    std::printf("  %-32s %5.1f fps  held 1/2/3/4+ %3.0f/%3.0f/%3.0f/%3.0f %%  judder %.2f  delay %5.1f ms  speed %.3f\n", kPolicies[pi].name, r.fps, 100 * r.hold[1] / n, 100 * r.hold[2] / n, p3, p4, r.judder, r.delay, r.speed);
            }
        }
        // Q2: no rule here turns a GPU of up to 2.4 refreshes into four-refresh holds: the display waits are absolute (due = last shown + interval, the sync target), they take the larger, they do not add
        for (int i : {8, 9}) {
            const Result h = Mean(kPolicies[0], hs[i]);
            const double n = static_cast<double>(h.hold[0] + h.hold[1] + h.hold[2] + h.hold[3] + h.hold[4] + h.hold[5]);
            Require(100 * (h.hold[4] + h.hold[5]) / n < 3.0 && h.fps > 23.0, "held 30, a GPU of 2.2-2.4 refreshes: holds of 3, not 4 (no stacked waits in the model): the console's 15 Hz needs more than that");
        }
        {
            const Result h = Mean(kPolicies[0], hs[10]);
            const double n = static_cast<double>(h.hold[0] + h.hold[1] + h.hold[2] + h.hold[3] + h.hold[4] + h.hold[5]);
            Require(100 * (h.hold[3] + h.hold[4] + h.hold[5]) / n > 90.0 && h.fps < 19.0, "held 30, a GPU of 3.4 refreshes: holds of 3 and 4 and about 18 pictures a second (a 15 Hz median of exactly four needs a GPU of about 3.9 refreshes = 65 ms, not 2.1)");
        }
        // Q4: free running with the aligned sync (what the plan's pace does from 37 pictures a second up) never holds 3+ in a scene that takes 1.0-1.4 refreshes: 1s and 2s only
        for (int i : {0, 1, 2, 3, 4, 5, 6, 7}) {
            const Result a = Mean(kPolicies[5], hs[i]);
            const double n = static_cast<double>(a.hold[0] + a.hold[1] + a.hold[2] + a.hold[3] + a.hold[4] + a.hold[5]);
            Require(100 * (a.hold[3] + a.hold[4] + a.hold[5]) / n < 2.0, "free running, aligned: under 2 % of pictures held 3+ refreshes at 1.0-1.4 refreshes of work (only 1s and 2s)");
            Require(a.fps > 60.0 / (i % 4 == 0 ? 1.0 : (i % 4 == 1 ? 1.1 : (i % 4 == 2 ? 1.2 : 1.4))) * 0.80, "... and within 20 % of the pictures the work allows (not the whole-refresh cliff)");
        }
        // policy 60 (a retrace wait after every swap) is the cliff: 1.1 refreshes of work falls towards 2-refresh holds
        {
            const Result s60 = Mean(kPolicies[1], hs[5]);
            Require(s60.fps < 40.0, "policy 60 with 1.1 refreshes of game-thread work: the retrace wait rounds every picture up to two refreshes (why MSC_FPS_BETWEEN=pace exists)");
        }
    }
    // Wave 9 (coordinator C): the ladder's budget under a free run (vgx_upbudget.h)
    Require(vgx::pacedup::UpBudgetMs(0, 33.4, 24.0) == 24.0, "a free run (interval 0) is judged by the cap, not the 30 budget");
    Require(vgx::pacedup::UpBudgetMs(2, 33.4, 24.0) == 33.4, "a held 30 keeps its budget");
    Require(vgx::pacedup::UpBudgetMs(1, 16.7, 24.0) == 16.7, "60 keeps its budget");
    Require(vgx::pacedup::UpBudgetMs(0, 33.4, 0.0) == 33.4, "cap 0 = off");
    Require(vgx::pacedup::UpBudgetMs(0, 16.7, 24.0) == 16.7, "a cap never raises the budget");
    // Wave 9 (coordinator C): what a player sees as the busiest stage takes 1.05-1.6 refreshes: a clean 30 lock against the free run with aligned sync (what the gate gives under its ENTER limit).
    // Printed with the "between" argument. Columns: pictures a second, share held 1/2/3+ refreshes, judder (unevenness of the time between pictures), motion error, delay.
    {
        const bool print = argc > 1 && argv[1] && std::strcmp(argv[1], "between") == 0;
        const double works[] = {1.05, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.8};
        double fps[8] = {}, h30j[8] = {};
        for (int wi = 0; wi < 8; ++wi) {
            const double w = works[wi] * kV;
            const Scene sc{"x", 0.75 * w, 0.9 * w, w, 0.9 * w};
            const Result a = Mean(kPolicies[5], sc), h = Mean(kPolicies[0], sc);
            fps[wi] = a.fps; h30j[wi] = h.judder;
            if (print)
                for (const Result* r : {&h, &a}) {
                    const double n = static_cast<double>(r->hold[0] + r->hold[1] + r->hold[2] + r->hold[3] + r->hold[4] + r->hold[5]);
                    std::printf("work %.2f refreshes (%4.1f ms)  %-13s %5.1f fps  held 1/2/3+ %3.0f/%3.0f/%3.0f %%  judder %.2f  motion %4.1f ms  delay %5.1f ms\n", works[wi], w, r == &h ? "clean 30" : "free, aligned", r->fps, 100 * r->hold[1] / n,
                                100 * r->hold[2] / n, 100 * (n - r->hold[1] - r->hold[2]) / n, r->judder, r->motion, r->delay);
                }
        }
        Require(h30j[0] < 0.05 && h30j[4] < 0.05, "a clean 30 lock stays even from 1.05 to 1.4 refreshes of work (at 1.6, 26.7 ms, a spike misses a 33 ms frame: judder 0.26, as the free run)");
        Require(fps[0] > 50.0 && fps[6] < 45.0, "the free run gives about 55 pictures a second at 1.05 refreshes and under 45 at 1.6 (what a player gets from it)");
    }
    // Wave 9 (coordinator B): the frames-in-flight depth of the aligned free-running policy (plan: MSC_LAT_DEPTH 2 + MSC_LAT_DEPTH_PACED = 1 -> 3) against work of 0.8-1.1 refreshes, GPU-bound and
    // game-thread-bound; the delay each depth adds. Printed with the "depth" argument.
    {
        const bool print = argc > 1 && argv[1] && std::strcmp(argv[1], "depth") == 0;
        const double works[] = {0.8, 0.9, 1.0, 1.05, 1.1};
        double fps[2][6][5] = {};
        for (int gpuBound = 1; gpuBound >= 0; --gpuBound) {
            for (int wi = 0; wi < 5; ++wi) {
                const double w = works[wi] * kV;
                const Scene sc = gpuBound ? Scene{"gpu", 10, 12, 14, w} : Scene{"game", w, 12, 14, 14};
                if (print) std::printf("%s %.2f refreshes (%.1f ms):\n", gpuBound ? "GPU" : "game thread", works[wi], w);
                for (int d = 3; d <= 5; ++d) {
                    const Policy p{"free, aligned", Swap::Free, d, 2};
                    const Result r = Mean(p, sc);
                    const double n = static_cast<double>(r.hold[0] + r.hold[1] + r.hold[2] + r.hold[3] + r.hold[4] + r.hold[5]);
                    fps[gpuBound][d][wi] = r.fps;
                    if (print) std::printf("  depth %d  %5.1f fps  held 1/2/3+ %3.0f/%3.0f/%3.0f %%  judder %.2f  delay %5.1f ms\n", d, r.fps, 100 * r.hold[1] / n, 100 * r.hold[2] / n, 100 * (n - r.hold[1] - r.hold[2]) / n, r.judder, r.delay);
                }
            }
        }
        Require(fps[1][4][2] > fps[1][3][2] + 3.0, "GPU 1.0 refresh: depth 4 shows clearly more pictures than depth 3");
        Require(fps[1][5][2] < fps[1][4][2] + 1.0, "and depth 5 adds next to nothing over 4");
        Require(fps[1][4][4] > fps[1][3][4] - 0.5, "GPU 1.1: depth 4 is no worse than 3");
    }
    // Wave 10 (coordinator f, Galaxy item 5): the frames-in-flight depth at a held 30 (what a clean 30 and the 30 fallback run): pictures a second, 2-refresh share and frame start to screen at depth 1, 2, 3.
    {
        const bool print = argc > 1 && argv[1] && std::strcmp(argv[1], "held") == 0;
        const Scene scs[] = {{"light, busiest 16 ms", 9, 12, 16, 13}, {"busiest 24 ms", 14, 21, 24, 20}, {"busiest 30 ms", 16, 25, 30, 24}, {"GPU 33 ms", 14, 18, 20, 33}};
        double d[4][4] = {}, f[4][4] = {};
        for (int si = 0; si < 4; ++si) {
            if (print) std::printf("held 30, %s:\n", scs[si].name);
            for (int dep = 1; dep <= 3; ++dep) {
                const Policy p{"held 30", Swap::Held30, dep, 0};
                const Result r = Mean(p, scs[si]);
                d[si][dep] = r.delay; f[si][dep] = r.fps;
                const double n = static_cast<double>(r.hold[0] + r.hold[1] + r.hold[2] + r.hold[3] + r.hold[4] + r.hold[5]);
                if (print) std::printf("  depth %d  %5.1f fps  held 2/3+ %3.0f/%3.0f %%  judder %.2f  frame start to screen %5.1f p95 %5.1f ms\n", dep, r.fps, 100 * r.hold[2] / n, 100 * (r.hold[3] + r.hold[4] + r.hold[5]) / n, r.judder, r.delay, r.delayP95);
            }
        }
        Require(d[0][3] >= d[0][2] - 0.5 && d[0][2] >= d[0][1] - 0.5, "held 30: a deeper limit never gives less delay");
    }
    // Wave 10 (pacing, coordinator): the queue in the pipeline. Frame start to screen (the model's delay) and pictures a second, for the aligned free run at depth 3, depth 4, depth 4 with a phase-locked
    // frame start, depth 3 with it, and an adaptive depth (3 below 0.88 refreshes of work over the last second, 4 from 0.97). Jitter: the model's own (12 % and 3 % spikes) and a rougher 20 %. Printed with "queue".
    {
        const bool print = argc > 1 && argv[1] && std::strcmp(argv[1], "queue") == 0;
        const Policy ps[] = {
            {"depth 3", Swap::Free, 3, 2},
            {"depth 4", Swap::Free, 4, 2},
            {"depth 4 + phase-locked start", Swap::Free, 4, 2, false, 30},
            {"depth 3 + phase-locked start", Swap::Free, 3, 2, false, 30},
            {"adaptive 3/4", Swap::Free, 3, 2, false, 0, true},
            {"adaptive 3/4 + phase-locked start", Swap::Free, 3, 2, false, 20, true},
        };
        const double works[] = {0.8, 0.9, 1.0, 1.05, 1.1};
        double fpsT[2][6][5] = {}, dlyT[2][6][5] = {};
        for (int jit = 0; jit < 2; ++jit) {
            g_jitter = jit ? 0.20 : 0.12;
            for (int gpuBound = 1; gpuBound >= 0; --gpuBound)
                for (int wi = 0; wi < 5; ++wi) {
                    const double w = works[wi] * kV;
                    const Scene sc = gpuBound ? Scene{"gpu", 10, 12, 14, w} : Scene{"game", w, 12, 14, 14};
                    if (print) std::printf("%s %.2f refreshes, jitter %.0f %%:\n", gpuBound ? "GPU" : "game thread", works[wi], 100 * g_jitter);
                    for (int pi = 0; pi < 6; ++pi) {
                        const Result r = Mean(ps[pi], sc);
                        const double n = static_cast<double>(r.hold[0] + r.hold[1] + r.hold[2] + r.hold[3] + r.hold[4] + r.hold[5]);
                        if (jit == 0 && gpuBound) { fpsT[0][pi][wi] = r.fps; dlyT[0][pi][wi] = r.delay; }
                        if (jit == 0 && !gpuBound) { fpsT[1][pi][wi] = r.fps; dlyT[1][pi][wi] = r.delay; }
                        if (print)
                            std::printf("  %-34s %5.1f fps  held 1/2/3+ %3.0f/%3.0f/%3.0f %%  judder %.2f  delay %5.1f p95 %5.1f ms\n", ps[pi].name, r.fps, 100 * r.hold[1] / n, 100 * r.hold[2] / n,
                                        100 * (n - r.hold[1] - r.hold[2]) / n, r.judder, r.delay, r.delayP95);
                    }
                }
        }
        g_jitter = 0.12;
        Require(fpsT[0][1][2] > fpsT[0][0][2] + 3.0, "(check) depth 4 still buys pictures at 1.0 refresh of GPU work");
        Require(dlyT[0][2][1] < dlyT[0][1][1] - 3.0, "the phase-locked start takes 3-5 ms out of the delay at 0.9 refresh (GPU-bound)");
        Require(fpsT[0][2][1] > fpsT[0][1][1] - 0.5, "... without losing pictures there");
        Require(std::fabs(dlyT[0][2][2] - dlyT[0][1][2]) < 3.0 && fpsT[0][2][2] > fpsT[0][1][2] - 1.0, "at 1.0 refresh it gains nothing: a GPU that needs the whole refresh keeps its queue (Little's law; the model says so, the console decides)");
        Require(fpsT[0][2][4] > fpsT[0][0][4] - 0.5, "and at 1.1 refreshes it is no worse than depth 3 in pictures");
    }
    std::printf("%s: pace_mid_model\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
