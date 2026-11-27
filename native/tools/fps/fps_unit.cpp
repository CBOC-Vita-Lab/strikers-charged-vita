// Unit test of the MSC_FPS=auto controller (platform/compat/np_fps_ctl.h): synthetic frame sequences, no game.
//   clang++ -std=c++11 -O1 -Iplatform/compat tools/fps/fps_unit.cpp -o /tmp/fps_unit && /tmp/fps_unit
// Each scenario feeds frames with the cost every core would have and the interval the policy in force would give (a frame that does not
// fit in a refresh at 60 is held two, at 30 it is held two or more), the way the game would run, and checks the decisions.
#include "np_fps_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

using namespace npfps;

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what, const char* detail = "") {
    if (ok) { ++g_pass; printf("ok   %s %s\n", what, detail); }
    else { ++g_fail; printf("FAIL %s %s\n", what, detail); }
}

const double R = 1001.0 / 60000.0;

// Costs of a scene per frame, ms: game P + M per step, other cores.
struct Scene {
    double P, M, core1, core2, audio;   // audio: ms per refresh, follows time
    unsigned id;
    double gpu;                         // the GPU's time per frame (0 = none): not on any core's counter
    double gpuMin;                      // what the renderer reports as its GPU time at the smallest picture (0 = not reported)
};

struct Sim {
    Auto ctl;
    double now = 0;
    double lastBusy = 0;
    int frames = 0, upAt = -1;
    std::vector<double> ups, downs;
    explicit Sim(const Config& c = Config::V11()) : ctl(c) {}

    // one frame under the policy in force; `hitchMs` adds to the game core (a one-frame stall)
    void Step(const Scene& s, double hitchMs = 0, bool locked = false) {
        const Policy pol = ctl.Current();
        // the interval the previous frame lasted: the larger of the busiest core and the policy's refresh count
        const double stepsPerFrame = 0.0;
        (void)stepsPerFrame;
        double interval = R * 2;   // 30: two refreshes
        double busyGuess = s.P + s.M * 1.67;
        if (pol == kPolicy60) {
            // steps per frame at 60 follow time: 0.834 per refresh
            busyGuess = s.P + s.M * 0.834;
            const double need = busyGuess + hitchMs;
            const double c1 = s.core1, c2 = s.core2 + s.audio;
            double worst = need > c1 ? (need > c2 ? need : c2) : (c1 > c2 ? c1 : c2);
            if (s.gpu > worst) worst = s.gpu;
            interval = R * (worst <= R * 1000.0 ? 1.0 : (worst <= 2 * R * 1000.0 ? 2.0 : 3.0));
        } else {
            const double need = busyGuess + hitchMs;
            const double c1 = s.core1, c2 = s.core2 + s.audio * 2;
            double worst = need > c1 ? (need > c2 ? need : c2) : (c1 > c2 ? c1 : c2);
            if (s.gpu > worst) worst = s.gpu;
            interval = R * (worst <= 2 * R * 1000.0 ? 2.0 : 3.0);
        }
        FrameIn f;
        memset(&f, 0, sizeof f);
        f.now = now;
        f.interval = interval;
        f.scene = s.id;
        f.locked = locked;
        const double steps = interval / 0.02;
        f.steps = (int)(steps + 0.5);
        f.simMs = s.M * f.steps;
        f.costsValid = true;
        f.gameCore = 0;
        f.cpuMs[0] = s.P + s.M * f.steps + hitchMs;
        f.cpuMs[1] = s.core1;
        f.cpuMs[2] = s.core2 + s.audio * (interval / R);
        f.timeMs[2] = s.audio * (interval / R);
        f.gpuMinMs = s.gpuMin;
        const Decision d = ctl.Frame(f);
        if (d.switched) {
            if (d.policy == kPolicy60) ups.push_back(now); else downs.push_back(now);
        }
        now += interval;
        ++frames;
    }
    void Run(const Scene& s, double seconds, double hitchEvery = 0, double hitchMs = 0) {
        const double end = now + seconds;
        int n = 0;
        while (now < end) {
            ++n;
            Step(s, (hitchEvery > 0 && n % (int)hitchEvery == 0) ? hitchMs : 0.0);
        }
    }
};
} // namespace

int main() {
    char buf[200];
    // 1. an easy scene (title/main menu): up after the hold time, then stays at 60
    {
        Sim s;
        const Scene menu = {4.0, 0.0, 6.0, 6.0, 1.5, 1};
        s.Run(menu, 10.0);
        snprintf(buf, sizeof buf, "(ups %zu, downs %zu, first up at t=%.2f)", s.ups.size(), s.downs.size(), s.ups.empty() ? -1.0 : s.ups[0]);
        check(s.ctl.Current() == kPolicy60 && s.ups.size() == 1 && s.downs.empty(), "easy scene goes to 60 and stays", buf);
        check(!s.ups.empty() && s.ups[0] >= 1.0 && s.ups[0] < 1.4, "up after about one second of evidence", buf);
    }
    // 2. a heavy scene (open play on the unmodified build: 23 ms on the game core at 60): never goes up
    {
        Sim s;
        const Scene play = {9.0, 5.9, 8.5, 19.6, 1.5, 2};
        s.Run(play, 60.0);
        check(s.ctl.Current() == kPolicy30 && s.ups.empty(), "a scene that cannot hold 60 stays at 30 for a minute");
    }
    // 3. the loading scene: 38 ms: stays at 30
    {
        Sim s;
        const Scene load = {38.0, 0.0, 2.0, 6.0, 1.5, 3};
        s.Run(load, 20.0);
        check(s.ctl.Current() == kPolicy30 && s.ups.empty(), "loading stays at 30");
    }
    // 4. borderline: predicts 13 ms (under the 14.2 ms margin), but the real 60 Hz cost is higher on the back end core (16.5 ms): the first try
    //    misses, the controller learns (penalty), and stops trying: at most 4 switches in ten minutes
    {
        Sim s;
        const Scene border = {6.0, 5.0, 16.4, 6.0, 1.5, 4};   // game core 6 + 5 x 0.834 = 10.2; the back end core 16.4 busy
        // the prediction looks at every core, so it sees the 16.4 ms and never goes up: that is the correct behaviour; make the misprediction
        // by hiding the back end's cost at 30 (it grows at 60 because two frames' work arrives in one refresh pipeline)
        (void)border;
        Sim t;
        // frames at 30 cost (as seen at 30) 12 ms on the busiest core; at 60 the real cost is 17 ms: model it with a hitch only in 60 mode
        int tries = 0;
        const Scene base = {5.0, 0.0, 12.0, 6.0, 1.5, 5};
        for (int n = 0; n < 40000 && t.now < 600.0; ++n) {
            const Policy pol = t.ctl.Current();
            Scene sc = base;
            if (pol == kPolicy60) sc.core1 = 17.0;   // reality at 60 is worse than the prediction made at 30
            t.Step(sc);
            (void)tries;
        }
        snprintf(buf, sizeof buf, "(ups %zu downs %zu in 600 s, penalty %d)", t.ups.size(), t.downs.size(), t.ctl.Penalty());
        check(t.ups.size() <= 4 && t.downs.size() <= 4 && t.ctl.Current() == kPolicy30, "a wrong prediction is tried a few times, then left alone", buf);
        // and the gaps between tries grow
        bool grow = true;
        for (size_t i = 2; i < t.ups.size(); ++i)
            if (t.ups[i] - t.ups[i - 1] <= t.ups[i - 1] - t.ups[i - 2]) grow = false;
        check(grow, "the time between tries grows");
    }
    // 5. a single hitch after the probation does not flip; a burst of them does
    {
        Sim s;
        const Scene menu = {4.0, 0.0, 6.0, 6.0, 1.5, 1};
        s.Run(menu, 8.0);   // up, probation over
        const size_t downs0 = s.downs.size();
        s.Step(menu, 30.0);  // one 30 ms hitch (a frame of 2+ refreshes)
        for (int i = 0; i < 100; ++i) s.Step(menu);
        check(s.ctl.Current() == kPolicy60 && s.downs.size() == downs0, "one long frame after the probation does not send it down");
        for (int i = 0; i < 6; ++i) { s.Step(menu, 30.0); s.Step(menu); }
        check(s.ctl.Current() == kPolicy30 && s.downs.size() == downs0 + 1, "three long frames within a window do");
    }
    // 6. a miss inside the probation goes down at once
    {
        Sim s;
        const Scene menu = {4.0, 0.0, 6.0, 6.0, 1.5, 1};
        s.Run(menu, 1.3);
        const double upAt = s.ups.empty() ? -1 : s.ups[0];
        s.Step(menu, 30.0);
        snprintf(buf, sizeof buf, "(up at %.2f, downs %zu)", upAt, s.downs.size());
        check(upAt > 0 && s.ctl.Current() == kPolicy30 && s.downs.size() == 1, "a miss right after going up goes down at once", buf);
    }
    // 7. a scene change starts the evidence again; a cut-scene lock gives no evidence
    {
        Sim s;
        const Scene a = {4.0, 0.0, 6.0, 6.0, 1.5, 1};
        s.Run(a, 0.9);
        const Scene b = {4.0, 0.0, 6.0, 6.0, 1.5, 9};
        s.Run(b, 0.9);
        check(s.ups.empty(), "a scene change resets the evidence (0.9 s + 0.9 s is not a second in one scene)");
        s.Run(b, 0.5);
        check(!s.ups.empty(), "and a second in the new scene is enough");
        Sim l;
        for (int i = 0; i < 400; ++i) l.Step(a, 0.0, true);
        check(l.ups.empty() && l.ctl.Current() == kPolicy30, "locked cut-scene frames are no evidence");
    }
    // 8. cool-down after a down: goes up only after it, even with good frames
    {
        Sim s;
        const Scene menu = {4.0, 0.0, 6.0, 6.0, 1.5, 1};
        s.Run(menu, 1.3);
        s.Step(menu, 30.0);   // down now
        const double downAt = s.downs.empty() ? 0 : s.downs[0];
        s.Run(menu, 10.0);
        const double up2 = s.ups.size() >= 2 ? s.ups[1] : -1;
        snprintf(buf, sizeof buf, "(down at %.2f, next up at %.2f)", downAt, up2);
        check(up2 >= downAt + 4.0 && up2 < downAt + 6.5, "the cool-down (4 s) is kept after a down", buf);
    }
    // 9. without a probe (only the game thread's busy time is known): easy goes up, heavy does not
    {
        Auto ctl(Config::V11());
        double now = 0;
        int ups = 0;
        for (int i = 0; i < 300; ++i) {
            FrameIn f;
            memset(&f, 0, sizeof f);
            f.now = now;
            f.interval = (ctl.Current() == kPolicy60 ? 1 : 2) * R;
            f.scene = 1;
            f.gameBusyMs = 9.0;   // unthrottled game thread time at 30
            f.steps = 0;
            const Decision d = ctl.Frame(f);
            if (d.switched && d.policy == kPolicy60) ++ups;
            now += (ctl.Current() == kPolicy60 ? R : 2 * R);
        }
        check(ups == 1 && ctl.Current() == kPolicy60, "without a probe: a light game thread goes to 60");
        Auto heavy((Config::V11()));
        now = 0;
        for (int i = 0; i < 300; ++i) {
            FrameIn f;
            memset(&f, 0, sizeof f);
            f.now = now; f.interval = (heavy.Current() == kPolicy60 ? 1 : 2) * R; f.scene = 1; f.gameBusyMs = 22.0;
            heavy.Frame(f);
            now += (heavy.Current() == kPolicy60 ? 1 : 2) * R;
        }
        check(heavy.Current() == kPolicy30, "without a probe: a heavy game thread stays at 30");
    }
    // 10. prediction: the simulation part is priced at ONE step (the frame most refreshes hold); the audio part follows time
    {
        Auto ctl((Config::V11()));
        FrameIn f;
        memset(&f, 0, sizeof f);
        f.now = 1; f.interval = 2 * R; f.scene = 1; f.costsValid = true; f.gameCore = 0;
        f.steps = 2; f.simMs = 12.0;           // 6 ms per step at 30 (two steps per frame)
        f.cpuMs[0] = 20.0;                     // 8 ms outside the simulation
        f.cpuMs[2] = 8.0; f.timeMs[2] = 8.0;   // an 8 ms audio load in a 2-refresh frame: 4 ms per refresh
        const Prediction p = ctl.Predict(f);
        const double want0 = 8.0 + 6.0 * 1.0;   // 8 + one step of 6
        snprintf(buf, sizeof buf, "(max60 %.2f ms on core %d, wanted %.2f)", p.max60, p.core, want0);
        check(p.valid && p.core == 0 && fabs(p.max60 - want0) < 0.01, "prediction: simulation re-priced at one step per refresh", buf);
        Auto fresh((Config::V11()));
        FrameIn g2 = f;
        g2.cpuMs[0] = 1.0; g2.simMs = 0; g2.steps = 0;
        const Prediction q = fresh.Predict(g2);
        check(q.valid && q.core == 2 && fabs(q.max60 - 4.0) < 0.01, "prediction: audio scaled to a refresh (8 ms per 2 refreshes -> 4 ms)");
    }
    // 11. the limit is the GPU (the cores look idle, the pictures take two refreshes at 60): the miss rule does not see it (no core is busy), the sustained-slowness rule does
    {
        Sim s;
        const Scene gpuBound = {4.0, 0.0, 6.0, 6.0, 1.5, 7, 25.0};
        s.Run(gpuBound, 300.0);
        const double stay = (!s.ups.empty() && !s.downs.empty()) ? s.downs[0] - s.ups[0] : -1.0;
        snprintf(buf, sizeof buf, "(ups %zu, downs %zu, first stay at 60 %.2f s)", s.ups.size(), s.downs.size(), stay);
        check(!s.ups.empty() && stay > 0 && stay < 2.0, "a GPU-bound scene is sent back to 30 within two seconds of going up", buf);
        check(s.ctl.Current() == kPolicy30 && s.ups.size() <= 4, "and left at 30 after a few tries", buf);
    }
    // 12. a scene with an idle GPU and the same cores keeps 60 (the new rule does not touch a healthy 60)
    {
        Sim s;
        const Scene light = {4.0, 0.0, 6.0, 6.0, 1.5, 8, 10.0};
        s.Run(light, 120.0);
        check(s.ctl.Current() == kPolicy60 && s.ups.size() == 1 && s.downs.empty(), "a healthy 60 is kept with the GPU at 10 ms");
    }
    // 13. the GPU-bound back-off belongs to the scene: a light scene after it goes up within the ordinary time
    {
        Sim s;
        Scene gpuBound = {4.0, 0.0, 6.0, 6.0, 1.5, 7, 25.0};
        s.Run(gpuBound, 150.0);   // three tries: the next one is 128 s away
        const double t0 = s.now;
        Scene light = {4.0, 0.0, 6.0, 6.0, 1.5, 9, 10.0};
        s.Run(light, 40.0);
        const double after = s.ups.empty() ? -1.0 : s.ups.back() - t0;
        snprintf(buf, sizeof buf, "(the next up came %.1f s into the light scene)", after);
        check(s.ctl.Current() == kPolicy60 && after >= 0 && after < 8.0, "a new scene is not held back by the last scene's GPU back-off", buf);
    }
    // 14. the renderer reports the GPU (gpuMinMs): a scene whose GPU time at the smallest picture is over the budget is never tried at 60
    {
        Sim s;
        const Scene gpuBound = {4.0, 0.0, 6.0, 6.0, 1.5, 7, 25.0, 22.0};
        s.Run(gpuBound, 120.0);
        check(s.ups.empty() && s.ctl.Current() == kPolicy30, "a reported GPU-bound scene is not tried at 60");
        Sim t;
        const Scene fits = {4.0, 0.0, 6.0, 6.0, 1.5, 7, 12.0, 12.0};
        t.Run(fits, 60.0);
        check(t.ctl.Current() == kPolicy60 && t.ups.size() == 1 && t.downs.empty(), "a reported GPU time that fits keeps 60");
    }
    // 15. at 60 the reported GPU time rises over the budget (a heavier moment the picture size cannot absorb): down within the busy-frame count
    {
        Sim s;
        Scene light = {4.0, 0.0, 6.0, 6.0, 1.5, 7, 12.0, 12.0};
        s.Run(light, 8.0);
        const size_t d0 = s.downs.size();
        Scene heavy = light;
        heavy.gpuMin = 18.0;   // the frame still fits (gpu 12 on the clock), only the report says the GPU is over
        for (int i = 0; i < 12; ++i) s.Step(heavy);
        check(s.ctl.Current() == kPolicy30 && s.downs.size() == d0 + 1, "a GPU report over the budget for 8 frames sends it down");
    }
    // 16. a held scene (Mega Strike) goes to 30 at once, stays there while held, and 60 comes back after the cool-down
    {
        Sim s;
        const Scene light = {4.0, 0.0, 6.0, 6.0, 1.5, 7, 10.0, 10.0};
        s.Run(light, 5.0);
        const bool at60 = s.ctl.Current() == kPolicy60;
        const Decision d = s.ctl.Hold30(s.now);
        bool stayed = d.switched && s.ctl.Current() == kPolicy30;
        for (int i = 0; i < 150; ++i) { s.ctl.Hold30(s.now); s.now += 2 * R; if (s.ctl.Current() != kPolicy30) stayed = false; }
        const double releasedAt = s.now;
        const size_t ups0 = s.ups.size();
        s.Run(light, 10.0);
        const double back = s.ups.size() > ups0 ? s.ups.back() - releasedAt : -1.0;
        snprintf(buf, sizeof buf, "(60 again %.1f s after the hold)", back);
        check(at60 && stayed && back >= 1.0 && back < 6.0, "a held scene stays at 30, and 60 returns after it", buf);
    }

    // ============================ 1.2: asymmetric hysteresis (Config() against Config::V11()) =======================================================
    printf("\n-- 1.2 hysteresis --\n");
    // A borderline load: the busiest core costs mean +- sd of a refresh, the same at 30 and at 60 (a frame over the refresh at 60 is held two). The tries are
    // counted over ten minutes: the 1.1 controller keeps coming back, the 1.2 one waits longer after every failed try.
    struct Border { int switches, ups, downs; double at60, missPct; };
    auto borderline = [&](const Config& cfg, double mean, double sd, double amp, double period, double seconds, unsigned seed) {
        Sim t(cfg);
        unsigned long long rng = seed;
        auto rnd = [&]() { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (double)(rng >> 11) / 9007199254740992.0; };
        double at60 = 0; int n60 = 0, miss60 = 0;
        while (t.now < seconds) {
            // a sum of three uniforms: about normal
            const double z = (rnd() + rnd() + rnd() - 1.5) * 2.0;
            const double load = mean + amp * sin(6.283185307 * t.now / period) + sd * z;   // the action of a match: slow swings plus noise
            Scene sc = {4.0, 0.0, load * R * 1000.0, 6.0, 1.5, 21};
            const bool was60 = t.ctl.Current() == kPolicy60;
            const double t0 = t.now;
            t.Step(sc);
            if (was60) { at60 += t.now - t0; ++n60; if (t.now - t0 > 1.5 * R) ++miss60; }
        }
        Border b = {(int)(t.ups.size() + t.downs.size()), (int)t.ups.size(), (int)t.downs.size(), at60, n60 ? 100.0 * miss60 / n60 : 0.0};
        return b;
    };
    {
        struct Case { double mean, sd, amp, period; const char* name; } cases[] = {
            {0.85, 0.04, 0.10, 20.0, "85% swinging +-10% every 20 s, noise 4%"}, {0.82, 0.04, 0.12, 12.0, "82% swinging +-12% every 12 s, noise 4%"},
            {0.88, 0.05, 0.0, 1.0, "88% +- 5% steady"}, {0.60, 0.04, 0.10, 30.0, "60% swinging +-10% every 30 s (easy)"}};
        for (const Case& c : cases) {
            int sw11 = 0, sw12 = 0, up11 = 0, up12 = 0;
            double t60_11 = 0, t60_12 = 0, m11 = 0, m12 = 0;
            for (unsigned seed = 1; seed <= 5; ++seed) {
                const Border a = borderline(Config::V11(), c.mean, c.sd, c.amp, c.period, 600.0, seed), b = borderline(Config(), c.mean, c.sd, c.amp, c.period, 600.0, seed);
                sw11 += a.switches; sw12 += b.switches; up11 += a.ups; up12 += b.ups; t60_11 += a.at60; t60_12 += b.at60; m11 += a.missPct; m12 += b.missPct;
            }
            snprintf(buf, sizeof buf, "borderline %s, 5 runs of 10 min: switches 1.1 %d -> 1.2 %d; ups %d -> %d; time at 60: %.0f s -> %.0f s; missed refreshes while at 60: %.1f%% -> %.1f%%",
                     c.name, sw11, sw12, up11, up12, t60_11, t60_12, m11 / 5, m12 / 5);
            printf("info %s\n", buf);
            if (c.mean >= 0.80) check(sw12 <= sw11 && sw12 <= 5 * 8, "borderline load: 1.2 flips no more often than 1.1 and at most eight times in ten minutes", buf);
            else check(sw12 <= 5 * 2 && t60_12 > 0.9 * 5 * 600.0, "an easy load: 60 almost all the time", buf);
        }
    }
    // up needs 2 s of frames at 80 % or less: a steady 78 % goes up after about 2 s, a steady 83 % never does
    {
        Sim a{Config()}, b{Config()};
        const Scene s78 = {4.0, 0.0, 0.78 * R * 1000.0, 6.0, 1.5, 22}, s83 = {4.0, 0.0, 0.83 * R * 1000.0, 6.0, 1.5, 23};
        a.Run(s78, 6.0);
        b.Run(s83, 60.0);
        snprintf(buf, sizeof buf, "(78%%: first up at %.2f s; 83%%: ups %zu)", a.ups.empty() ? -1.0 : a.ups[0], b.ups.size());
        check(!a.ups.empty() && a.ups[0] >= 2.0 && a.ups[0] < 2.5 && b.ups.empty(), "1.2 goes up after 2 s at 78 % of the refresh and never at 83 %", buf);
        Sim c{Config::V11()};
        c.Run(s83, 10.0);
        check(!c.ups.empty() && c.ups[0] < 1.5, "(1.1 went up at 83 % after one second)");
    }
    // a few spikes in the evidence window do not restart it (10 % tolerated), a hovering load does
    {
        Sim a{Config()};
        const Scene lo = {4.0, 0.0, 0.70 * R * 1000.0, 6.0, 1.5, 24}, spike = {4.0, 0.0, 0.88 * R * 1000.0, 6.0, 1.5, 24};
        int n = 0;
        while (a.now < 6.0 && a.ups.empty()) { a.Step((++n % 20 == 0) ? spike : lo); }
        snprintf(buf, sizeof buf, "(up at %.2f s)", a.ups.empty() ? -1.0 : a.ups[0]);
        check(!a.ups.empty() && a.ups[0] < 3.0, "a spike every 20 frames (5 %) does not postpone 60", buf);
        Sim b{Config()};
        n = 0;
        while (b.now < 30.0) { b.Step((++n % 5 == 0) ? spike : lo); }
        check(b.ups.empty(), "a spike every 5 frames (20 %) keeps it at 30");
    }
    // down within four frames over 92 %, before any frame misses its refresh; 1.1 needed eight frames over 97 %
    {
        Sim s{Config()};
        const Scene easy = {4.0, 0.0, 6.0, 6.0, 1.5, 25};
        s.Run(easy, 8.0);
        const size_t d0 = s.downs.size();
        const bool at60 = s.ctl.Current() == kPolicy60;
        Scene hot = easy;
        hot.core1 = 0.94 * R * 1000.0;   // fits, but with no room left
        int frames = 0;
        while (s.ctl.Current() == kPolicy60 && frames < 30) { s.Step(hot); ++frames; }
        snprintf(buf, sizeof buf, "(down after %d frames at 94%%)", frames);
        check(at60 && s.ctl.Current() == kPolicy30 && s.downs.size() == d0 + 1 && frames <= 5, "at 60 a load of 94 % sends it down within five frames", buf);
        Sim t{Config::V11()};
        t.Run(easy, 8.0);
        int frames11 = 0;
        while (t.ctl.Current() == kPolicy60 && frames11 < 30) { t.Step(hot); ++frames11; }
        snprintf(buf, sizeof buf, "(1.1: %d frames, 94%% < its 97%% line)", frames11);
        check(frames11 >= 30, "(1.1 did not react to 94 % at all)", buf);
        // and 90 % does not
        Sim u{Config()};
        u.Run(easy, 8.0);
        Scene warm = easy;
        warm.core1 = 0.90 * R * 1000.0;
        for (int i = 0; i < 300; ++i) u.Step(warm);
        check(u.ctl.Current() == kPolicy60, "90 % of a refresh holds 60");
    }
    // the retry back-off: a scene whose 60 fails every time (reality at 60 worse than the prediction made at 30): the waits grow 12, 24, 48, 96 s
    {
        Sim t{Config()};
        const Scene base = {5.0, 0.0, 12.0, 6.0, 1.5, 26};
        for (int n = 0; n < 200000 && t.now < 900.0; ++n) {
            Scene sc = base;
            if (t.ctl.Current() == kPolicy60) sc.core1 = 17.0;
            t.Step(sc);
        }
        std::string gaps;
        bool grow = true;
        for (size_t i = 1; i < t.ups.size(); ++i) {
            char g[32];
            snprintf(g, sizeof g, "%.0f ", t.ups[i] - t.ups[i - 1]);
            gaps += g;
            if (i >= 2 && t.ups[i] - t.ups[i - 1] <= t.ups[i - 1] - t.ups[i - 2] && t.ups[i] - t.ups[i - 1] < 110.0) grow = false;
        }
        Sim o{Config::V11()};
        for (int n = 0; n < 200000 && o.now < 900.0; ++n) {
            Scene sc = base;
            if (o.ctl.Current() == kPolicy60) sc.core1 = 17.0;
            o.Step(sc);
        }
        snprintf(buf, sizeof buf, "(15 min: ups %zu (1.1: %zu); gaps between tries %ss; scene fails %d)", t.ups.size(), o.ups.size(), gaps.c_str(), t.ctl.SceneFails());
        check(t.ups.size() <= 7 && grow && t.ups.size() < o.ups.size(), "a failing scene is tried less and less often", buf);
    }
    // the retry back-off belongs to the scene: a new scene is tried after the ordinary cool-down
    {
        Sim t{Config()};
        const Scene base = {5.0, 0.0, 12.0, 6.0, 1.5, 26};
        for (int n = 0; n < 200000 && t.now < 120.0; ++n) {
            Scene sc = base;
            if (t.ctl.Current() == kPolicy60) sc.core1 = 17.0;
            t.Step(sc);
        }
        const int fails = t.ctl.SceneFails();
        const double t0 = t.now;
        const size_t ups0 = t.ups.size();
        Scene easy = {4.0, 0.0, 6.0, 6.0, 1.5, 27};
        t.Run(easy, 25.0);
        const double back = t.ups.size() > ups0 ? t.ups.back() - t0 : -1.0;
        snprintf(buf, sizeof buf, "(fails %d before; the new scene went up %.1f s after the change)", fails, back);
        check(fails >= 2 && back > 0 && back < 10.0, "a new scene is not held back by the last scene's failed tries", buf);
    }
    // a replay-class scene (sceneClass 1) that failed a try stays at 30 until it ends; an ordinary scene is tried again after its cool-down
    {
        auto run = [&](int cls, double* retryAfter) {
            Sim t{Config()};
            const Scene base = {5.0, 0.0, 12.0, 6.0, 1.5, 28};
            const size_t before = 0;
            (void)before;
            int n = 0;
            while (t.now < 40.0) {
                Scene sc = base;
                if (t.ctl.Current() == kPolicy60) sc.core1 = 17.0;
                // like Sim::Step but with the class set
                const Policy pol = t.ctl.Current();
                const double interval = pol == kPolicy60 ? R * (sc.core1 <= R * 1000.0 ? 1.0 : 2.0) : R * 2;
                FrameIn f;
                memset(&f, 0, sizeof f);
                f.now = t.now; f.interval = interval; f.scene = sc.id; f.steps = (int)(interval / 0.02 + 0.5); f.simMs = 0; f.costsValid = true;
                f.cpuMs[0] = sc.P; f.cpuMs[1] = sc.core1; f.cpuMs[2] = sc.core2 + sc.audio * (interval / R); f.timeMs[2] = sc.audio * (interval / R);
                f.sceneClass = cls;
                const Decision d = t.ctl.Frame(f);
                if (d.switched) { if (d.policy == kPolicy60) t.ups.push_back(t.now); else t.downs.push_back(t.now); }
                t.now += interval;
                ++n;
            }
            *retryAfter = t.ups.size() >= 2 ? t.ups[1] - t.ups[0] : -1.0;
            return (int)t.ups.size();
        };
        double r0, r1;
        const int u0 = run(0, &r0), u1 = run(1, &r1);
        snprintf(buf, sizeof buf, "(ordinary: %d tries in 40 s, 2nd after %.0f s; replay class: %d)", u0, r0, u1);
        check(u0 >= 2 && u1 == 1, "a replay-class scene gets one try, an ordinary one a second try after the cool-down", buf);
    }
    // the renderer's size change (dynGrace): ten late frames around it are not judged
    {
        Sim s{Config()};
        const Scene easy = {4.0, 0.0, 6.0, 6.0, 1.5, 29};
        s.Run(easy, 8.0);
        const size_t d0 = s.downs.size();
        bool graceDown = false;
        for (int i = 0; i < 12; ++i) {
            FrameIn f;
            memset(&f, 0, sizeof f);
            f.now = s.now; f.interval = 2 * R; f.scene = 29; f.costsValid = true; f.cpuMs[0] = 14.0; f.cpuMs[1] = 15.0; f.cpuMs[2] = 8.0; f.dynGrace = true;
            if (s.ctl.Frame(f).switched) graceDown = true;
            s.now += 2 * R;
        }
        check(!graceDown && s.downs.size() == d0 && s.ctl.Current() == kPolicy60, "twelve late frames during a size change do not send AUTO down");
        bool down = false;
        for (int i = 0; i < 12 && !down; ++i) {
            FrameIn f;
            memset(&f, 0, sizeof f);
            f.now = s.now; f.interval = 2 * R; f.scene = 29; f.costsValid = true; f.cpuMs[0] = 14.0; f.cpuMs[1] = 15.0; f.cpuMs[2] = 8.0;
            down = s.ctl.Frame(f).switched;
            s.now += 2 * R;
        }
        check(down, "and without the grace the same frames do");
    }
    // 1.2 ladder hold (MSC_FPS_LADDERHOLD): a GPU-limited miss at 60 waits while the renderer's dynamic resolution has picture steps left; a CPU-limited one does not
    {
        // Brings a controller to 60 on an easy scene, then feeds frames of two refreshes (the GPU's: cores nearly idle, or one core over the budget).
        // Returns the seconds until it went down (-1: not within `limit`). roomFor: seconds of the feed in which the ladder reports room.
        auto run = [&](bool hold, double cpuGame, double roomFor, double limit) {
            Config c;
            c.ladderHold = hold;
            Sim s{c};
            const Scene easy = {4.0, 0.0, 6.0, 6.0, 1.5, 31};
            s.Run(easy, 8.0);
            const double t0 = s.now;
            for (int i = 0; i < 4000 && s.now - t0 < limit; ++i) {
                FrameIn f;
                memset(&f, 0, sizeof f);
                f.now = s.now; f.interval = 2 * R; f.scene = 31; f.costsValid = true;
                f.cpuMs[0] = cpuGame; f.cpuMs[1] = 6.0; f.cpuMs[2] = 6.0 + 1.5;
                f.ladderRoom = s.now - t0 < roomFor;
                if (s.ctl.Frame(f).switched) return s.now - t0;
                s.now += 2 * R;
            }
            return -1.0;
        };
        const double off = run(false, 6.0, 1e9, 20.0);
        snprintf(buf, sizeof buf, "(down after %.2f s)", off);
        check(off > 0 && off < 3.0, "a GPU-limited miss at 60 goes to 30 within three seconds without the hold", buf);
        const double held = run(true, 6.0, 4.0, 20.0);
        snprintf(buf, sizeof buf, "(down after %.2f s; the ladder had room for 4 s)", held);
        check(held >= 3.9 && held < 5.5, "with the hold, AUTO waits while the ladder has room and goes down soon after it has none", buf);
        const double noRoom = run(true, 6.0, 0.0, 20.0);
        snprintf(buf, sizeof buf, "(down after %.2f s)", noRoom);
        check(noRoom > 0 && noRoom < 3.0, "with the hold and no room in the ladder it goes down as without it", buf);
        const double forever = run(true, 6.0, 1e9, 20.0);
        snprintf(buf, sizeof buf, "(down after %.2f s)", forever);
        check(forever >= 7.5 && forever < 10.0, "the hold ends after LADDER_HOLD_MAX (8 s) although the ladder reports room", buf);
        const double cpu = run(true, 17.0, 1e9, 20.0);
        snprintf(buf, sizeof buf, "(down after %.2f s)", cpu);
        check(cpu > 0 && cpu < 0.5, "a CPU-limited miss (the game core over the budget) goes to 30 at once, room in the ladder or not", buf);
    }
    // 1.2 ladder hold with a ladder model: the renderer's picture steps (each rung shortens the GPU time by 12 % and takes 0.6 s to be judged); the GPU time of the
    // scene is `gpu` ms at the top rung. While rungs are left a GPU-limited miss must step the ladder and not drop to 30; at the floor the old rules apply.
    {
        auto run = [&](bool hold, double gpu, int rungs, int* rungsUsed, bool* downed) {
            Config c;
            c.ladderHold = hold;
            Sim s{c};
            const Scene easy = {4.0, 0.0, 6.0, 6.0, 1.5, 32};
            s.Run(easy, 8.0);
            int rung = 0;
            double nextStep = s.now + 0.6;
            *downed = false;
            for (int i = 0; i < 3000 && s.now < 40.0 + 8.0; ++i) {
                double g = gpu;
                for (int r = 0; r < rung; ++r) g *= 0.88;
                const double rms = R * 1000.0;
                FrameIn f;
                memset(&f, 0, sizeof f);
                f.now = s.now; f.scene = 32; f.costsValid = true;
                f.interval = R * (g <= rms ? 1.0 : (g <= 2 * rms ? 2.0 : 3.0));   // a picture that does not fit a refresh is held for two
                f.cpuMs[0] = 6.0; f.cpuMs[1] = 6.0; f.cpuMs[2] = 7.5;
                f.ladderRoom = rung < rungs;
                f.dynGrace = false;
                if (s.ctl.Frame(f).switched) { *downed = true; break; }
                s.now += f.interval;
                if (rung < rungs && s.now >= nextStep && g > rms * 0.92) { ++rung; nextStep = s.now + 0.6; }
            }
            *rungsUsed = rung;
        };
        int used = 0;
        bool downed = false;
        run(true, 24.0, 3, &used, &downed);   // 24 ms -> 21.1 -> 18.6 -> 16.4 ms after three rungs: fits a refresh (16.68 ms) with the hold on
        snprintf(buf, sizeof buf, "(rungs used %d, went down: %d)", used, downed ? 1 : 0);
        check(used == 3 && !downed, "a GPU-limited scene that the ladder can fit: the ladder is stepped and AUTO stays at 60", buf);
        run(false, 24.0, 3, &used, &downed);
        check(downed, "the same scene without the hold goes to 30 before the ladder has finished", "");
        run(true, 40.0, 3, &used, &downed);   // 40 ms: even three rungs (27 ms) do not fit: the old rules apply once the ladder is at its floor
        snprintf(buf, sizeof buf, "(rungs used %d, went down: %d)", used, downed ? 1 : 0);
        check(used == 3 && downed, "a scene the ladder cannot fit: the ladder is used up, then AUTO goes to 30", buf);
    }
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
