// Wave 11: back-pressure before the pad (np_fps_bp.h) against a pipeline model. NP_BP_BREAK 1-5 must fail.
// Model: the game thread (G ms) -> send/decode/back end (S ms) -> GPU (Q ms), each stage serial; the display flips every F = 16.683 ms and shows a picture at the first flip when it is
// ready and `interval` flips after the previous one. The game thread starts a picture at the first retrace at least `interval` retraces after the previous start (the late wait),
// and not before the gate lets it (picture n-1-depth shown). pad -> shown is measured from the start of the picture (the pad is read there or later).
#include "np_fps_bp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
static const double F = 16.683;
static double flipAtOrAfter(double t) { return std::ceil(t / F - 1e-9) * F; }

struct Res { double fps, latMean, latP95, latMax; unsigned depthEnd, raises, failed; unsigned maxInFlight; };
struct Pipe { double G, S1, S2, Q; };
// spikeAt/spikeMs: one picture whose GPU time is spikeMs (a cut or a stage load); heavyFrom/To/Q2: a stretch with a different GPU time.
// mode 0: the 1.3 gate (depth 2, one refresh a probe: waits only when more than 2 are in flight, at most 1 refresh);  1: back-pressure;  2: no gate at all.
static Res run(int interval, Pipe p, int n, int mode, int spikeAt = -1, double spikeMs = 0, int heavyFrom = 0, int heavyTo = 0, double Q2 = 0, bool held = false) {
    BackPressure bpc;
    unsigned maxInFlight = 0;
    std::vector<double> start(n + 2), shown(n + 2, 0.0), lat;
    double gameFree = 0, s1Free = 0, s2Free = 0, gpuFree = 0, prevShown = -1e9, prevStart = -1e9;
    int prevRet = 0;
    bpc.After(0, 0, false, interval);
    if (held) bpc.SetHeld(true);
    for (int i = 1; i <= n; ++i) {
        double t = flipAtOrAfter(std::max(gameFree, prevStart + interval * F));
        unsigned waited = 0;
        const unsigned depth = mode == 1 ? bpc.GateDepth() : 2;
        const unsigned cap = mode == 1 ? bpc.MaxRefreshes() : 1;
        const unsigned slack = mode == 1 ? (unsigned)std::max(0.0, std::floor((interval * F - p.G - 3.0) / F)) : 1000u;
        while (mode != 2) {
            const int need = i - 1 - (int)depth;
            if (need <= 0 || shown[need] <= t + 1e-9 || waited >= cap) break;
            const bool backlog = need + 1 <= i - 1 && shown[need + 1] > t + 1e-9;   // more than depth + 1 unshown
            if (mode == 1 && !backlog && waited >= slack) break;
            t = flipAtOrAfter(t + F); ++waited;
        }
        start[i] = t;
        { unsigned fl = 0; for (int j = 1; j < i; ++j) if (shown[j] > t + 1e-9) ++fl; if (fl > maxInFlight) maxInFlight = fl; }
        const double gDone = t + p.G;
        gameFree = gDone;
        const double a = std::max(gDone, s1Free); s1Free = a + p.S1;
        const double b = std::max(s1Free, s2Free); s2Free = b + p.S2;
        double q = (i >= heavyFrom && i < heavyTo) ? Q2 : p.Q;
        if (i == spikeAt) q = spikeMs;
        const double g0 = std::max(s2Free, gpuFree);
        gpuFree = g0 + q;
        shown[i] = std::max(flipAtOrAfter(gpuFree), prevShown + interval * F);
        prevShown = shown[i];
        const int ret = (int)std::lround(t / F);
        if (mode == 1) bpc.After(waited, (unsigned)(ret - prevRet), i > 1, interval);
        prevRet = ret; prevStart = t;
        lat.push_back(shown[i] - t);
    }
    Res r{};
    const int a0 = n / 3;
    std::vector<double> l(lat.begin() + a0, lat.end());
    double sm = 0; for (double x : l) sm += x;
    r.latMean = sm / l.size();
    std::sort(l.begin(), l.end());
    r.latP95 = l[(size_t)(l.size() * 0.95)];
    r.latMax = l.back();
    r.fps = 1000.0 * (n - a0) / (shown[n] - shown[a0]);
    r.maxInFlight = maxInFlight; r.depthEnd = bpc.Depth(); r.raises = bpc.Raises(); r.failed = bpc.Failed();
    return r;
}
static void show(const char* name, const Res& o, const Res& b) {
    std::printf("%-34s old: %.1f fps, pad->shown mean %.0f p95 %.0f max %.0f | back-pressure: %.1f fps, mean %.0f p95 %.0f max %.0f, depth %u\n", name, o.fps, o.latMean, o.latP95, o.latMax, b.fps, b.latMean, b.latP95, b.latMax, b.depthEnd);
}
int main() {
    // the gate's own rules
    {
        BackPressure b; b.After(0, 0, false, 2);
        check(b.Depth() == 2, "starts at 2 at an interval of 2 (r10: depth 1 from the start made the kick-off slower)");
        check(b.Waits(10, 7, 0) && !b.Waits(10, 8, 0), "waits while more than depth pictures are unshown");
        check(!b.Waits(10, 7, 0, 0) && b.Waits(10, 6, 0, 0), "with no slack in the period the first refresh over the depth is not waited for; a backlog of more than depth + 1 is");
        check(b.Waits(10, 7, 0, 1) && !b.Waits(10, 7, 1, 1), "with a slack of one refresh it waits one");
        check(!b.Waits(10, 0, 0), "nothing shown yet (shown 0): no wait");
        check(!b.Waits(10, 3, b.MaxRefreshes()), "a picture never waits more than the cap");
        BackPressure c; c.After(0, 0, false, 1);
        check(c.Depth() == 3, "starts at 3 at an interval of 1");
    }
    // heldpipe23 (MSC_LAT_BP_HELD): in a held scene the gate waits only on a backlog over maxDepth (4); the learned depth (1 here) is not used, not changed, and back at the cut
    {
        BackPressure b; b.After(0, 0, false, 2);
        for (int i = 0; i < 60 && b.Depth() > 1; ++i) b.After(0, 2, true, 2);
        check(b.Depth() == 1, "held test set-up: a clean 30 goes down to depth 1");
        check(b.Waits(4, 2, 0) && !b.Waits(4, 3, 0), "live at depth 1: waits when 2 or more are unshown");
        b.SetHeld(true);
        check(!b.Waits(4, 2, 0, 0) && !b.Waits(5, 3, 0, 1000) && !b.Waits(6, 4, 0, 1000), "held: 1-2 unshown pictures are not waited for (up to heldDepth 2)");
        check(b.Waits(10, 4, 0, 0) && b.Waits(7, 3, 0, 0) && !b.Waits(6, 3, 0, 0), "held: a backlog of more than heldDepth + 1 is waited for");
        check(b.Waits(6, 3, 0, 1000) && !b.Waits(6, 3, 0, 0), "held: depth + 1 unshown waits only while there is slack");
        for (int i = 0; i < 300; ++i) b.After(1, 3, true, 2);   // late pictures after a wait at the gate: live play would raise the depth
        check(b.Depth() == 1 && b.Raises() == 0, "held: the controller does not learn from held pictures (no raise)");
        b.SetHeld(false);
        check(b.Depth() == 1 && b.Waits(4, 2, 0) && !b.Waits(4, 3, 0), "the cut back to play: the learned depth 1 is in force again");
        // held24: the backlog of a held scene (5 unshown) is not waited for in one go at the cut: one refresh a picture for releasePics pictures, then the cap again
        check(b.ReleaseLeft() == 8 && b.Waits(9, 3, 0, 1000) && !b.Waits(9, 3, 1, 1000), "the cut back to play: a backlog is waited for one refresh at most (release window of 8 pictures)");
        for (int i = 0; i < 8; ++i) b.After(1, 3, true, 2);
        check(b.ReleaseLeft() == 0 && b.Waits(9, 3, 1, 1000), "after the release window the backlog wait is the usual again (up to the cap)");
        BackPressure d; d.After(0, 0, false, 2); d.SetHeld(true); d.SetHeld(false);
        unsigned w = 0; while (d.Waits(12, 3, w, 1000) && w < 20) ++w;
        check(w == 1, "release: the first live picture after a held scene waits 1 refresh, not 4-6 (the 67-100 ms spike)");
    }
    // held24: a slow held scene (the walk-out: stages 18 / 14 / 12 / GPU 40 ms, shown every 4 refreshes) keeps at most heldDepth + 1 = 3 pictures in flight (the gate at maxDepth 4 let 5-6 through: Fable review 24)
    {
        const Pipe PW{18, 12, 12, 40};
        Res h = run(2, PW, 600, 1, -1, 0, 0, 0, 0, true);
        std::printf("held slow scene: %.1f fps, at most %u pictures in flight\n", h.fps, h.maxInFlight);
        check(h.maxInFlight <= BpConfig().heldDepth + 1, "held scene: at most heldDepth + 1 pictures in flight (audio leads the picture by at most that many)");
        check(h.maxInFlight >= 2, "model check: the held pipeline does overlap (2 or more in flight)");
    }
    // a late picture the gate did not wait for is the game's or the GPU's own cost: depth stays
    {
        BackPressure b; b.After(0, 0, false, 2);
        for (int i = 0; i < 300; ++i) b.After(0, 3, true, 2);
        check(b.Depth() <= 2 && b.Raises() == 0, "late pictures without a wait at the gate never raise the depth");
        BackPressure c; c.After(0, 0, false, 1);
        for (int i = 0; i < 500; ++i) c.After(0, 1, true, 1);
        check(c.Depth() == 2, "a clean 60 goes down from its start depth of 3 to 2 and floors at 2 (never 1)");
    }
    // a display that stops reporting: the cap, then standing aside
    {
        BackPressure b; b.After(0, 0, false, 2);
        for (int i = 0; i < 8; ++i) b.After(b.MaxRefreshes(), 2, true, 2);
        check(b.Aside(), "eight pictures in a row at the cap: the gate stands aside");
        check(b.MaxRefreshes() == 6, "the cap is 6 refreshes (a stalled display delays a picture 100 ms at most)");
    }
    // steady 30: game 22, send/decode 15, back end 8, GPU 14: 59 ms. A cut at picture 5 (GPU 120 ms) leaves a picture queued, the old gate leaves it standing
    const Pipe P30{22, 15, 8, 14};
    Res o30 = run(2, P30, 900, 0, 5, 120), b30 = run(2, P30, 900, 1, 5, 120), c30 = run(2, P30, 900, 1);
    show("steady 30 after a 120 ms cut", o30, b30);
    check(std::fabs(b30.fps - 30.0) < 0.6 && std::fabs(o30.fps - 30.0) < 0.6, "30 fps held with and without");
    check(o30.latMean > 80.0, "model check: the old gate leaves the backlog of the cut standing (pad -> shown above 80 ms)");
    check(b30.latMean < o30.latMean - 25.0, "steady 30: back-pressure drains it: at least 25 ms less pad -> shown");
    check(b30.latMean < 75.0, "steady 30: pad -> shown near the pipeline floor plus the flip wait (under 75 ms)");
    check(b30.depthEnd <= 2 && c30.depthEnd <= 2, "steady 30 does not need more than depth 2");
    // steady 60: game 14, 10, 10, GPU 12 = 46 ms pipeline: three pictures in flight; the old gate had switched itself off there (no gate = mode 2)
    const Pipe P60{14, 10, 10, 12};
    Res o60 = run(1, P60, 1800, 2), b60 = run(1, P60, 1800, 1);
    show("steady 60 (14/10/10/12)", o60, b60);
    check(b60.fps > 59.0, "60 fps held with back-pressure (the depth went up as far as the pipeline needs)");
    check(b60.depthEnd >= 2, "60 fps needs more than one picture in flight");
    check(b60.latMean <= o60.latMean + 1.0, "60: no more delay than no gate");
    // heavy moment A: GPU 40 ms (2.4 refreshes a picture, the game's 22 + 23 ms before the GPU is longer than the GPU's turn): one picture less in flight costs rate, so the controller keeps two.
    // The 1.3 gate switched itself off here after 30 waits (mode 2 = no gate): the queue grows to the game's lead.
    const Pipe PH{22, 15, 8, 40};
    Res oh0 = run(2, PH, 600, 0, 5, 150), oh2 = run(2, PH, 600, 2, 5, 150), bh = run(2, PH, 600, 1, 5, 150);
    show("heavy, GPU 40 ms (gate on)", oh0, bh);
    show("heavy, GPU 40 ms (gate off)", oh2, bh);
    check(bh.fps >= oh0.fps - 0.5, "heavy 40: back-pressure costs no frame rate against the old gate");
    check(bh.latMean <= oh0.latMean + 45.0 && bh.latMean < oh2.latMean - 500.0, "heavy 40: within one picture of the old gate, and far better than the gate gone (unbounded)");
    check(bh.latMax <= 22 + 15 + 8 + 40 + 40 + 3 * F + 50, "heavy 40: bounded");
    // heavy moment B: GPU 70 ms (four refreshes a picture): the GPU alone limits, one picture in flight is enough, and the extra picture a raise made is taken back
    const Pipe PG{22, 15, 8, 70};
    Res og0 = run(2, PG, 900, 0, 5, 150), bg = run(2, PG, 900, 1, 5, 150);
    show("heavy, GPU 70 ms", og0, bg);
    check(bg.fps >= og0.fps - 0.5, "GPU 70: no frame rate lost");
    check(bg.latMean < og0.latMean - 1000.0 && bg.latMax < 400.0, "GPU 70: bounded (the wave 10 gate's queue grows without limit here)");
    check(bg.depthEnd <= 2, "GPU 70: the extra picture a raise made is taken back");
    // a pipeline of 80 ms at 30 needs depth 2: found, and no flapping afterwards
    const Pipe P80{30, 28, 8, 20};
    Res bf = run(2, P80, 3000, 1);
    std::printf("%-34s back-pressure: %.1f fps, depth %u, raises %u, failed descents %u\n", "pipeline of 86 ms at 30", bf.fps, bf.depthEnd, bf.raises, bf.failed);
    check(bf.fps > 29.4, "an 86 ms pipeline still holds 30");
    check(bf.depthEnd == 2 && bf.raises <= 8, "it needs depth 2, and does not flap (at most 8 raises in 3000 pictures)");
    // lessons17: the room at the gate with the pad late sleep that follows it (MSC_LAT_BP_SLEEP)
    check(GateRoomRefreshes(33.4, 12.0, 0, 3.0, 16.683) == 1, "12 ms busy, no sleep: one refresh of room");
    check(GateRoomRefreshes(33.4, 12.0, 10000, 3.0, 16.683) == 0, "12 ms busy and a 10 ms pad sleep after the gate: no room for a refresh at the gate");
    check(GateRoomRefreshes(33.4, 5.0, 4000, 3.0, 16.683) == 1, "a 4 ms sleep out of a 21 ms room leaves the refresh");
    check(GateRoomRefreshes(33.4, 40.0, 0, 3.0, 16.683) == 0, "no room, no refresh");
    std::printf("%s: bp_unit\n", g_fail ? "FAILED" : "passed");
    return g_fail ? 1 : 0;
}
