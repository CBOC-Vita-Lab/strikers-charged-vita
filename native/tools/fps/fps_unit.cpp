// Unit test of the MSC_FPS=auto controller (platform/compat/np_fps_ctl.h): synthetic frame sequences, no game.
//   clang++ -std=c++11 -O1 -Iplatform/compat tools/fps/fps_unit.cpp -o /tmp/fps_unit && /tmp/fps_unit
// Each scenario feeds frames with the cost every core would have and the interval the policy in force would give (a frame that does not
// fit in a refresh at 60 is held two, at 30 it is held two or more), the way the game would run, and checks the decisions.
#include "np_fps_ctl.h"

#include <stdio.h>
#include <stdlib.h>
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
    explicit Sim(const Config& c = Config()) : ctl(c) {}

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
        Auto ctl;
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
        Auto heavy;
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
        Auto ctl;
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
        Auto fresh;
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
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
