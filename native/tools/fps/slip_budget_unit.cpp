// Wave 9 (pacing): the slip budget (MSC_SLIP_BUDGET_MS, np_slip.h, np_fps.cpp SlipFrame / NpSlipDelta) against the stateless slip it replaces, on synthetic frame-time sequences.
// The REAL np_fps.cpp is included. For every pacing state the game can be in it prints how much game time advances per second of real time and how many fixed 50 Hz steps run, with the
// stateless rule (MSC_SLIP=20, MSC_SLIP_STEPS=2: the Vita plan before wave 9) and with the budget (30 ms, refill 2 %). Then it checks the bound: sustained overload never slows the game
// clock by more than the refill share, in any window of W seconds at most refill x W + the bucket, an on-time frame is never touched, a single hitch is still cut (and by no more than before),
// the tasks of one frame agree, and a LAN session is left alone.
//   clang++ -std=c++11 -O1 -DNP_NATIVE=1 [-DNP_SLIP_BREAK=1..4] -Inative/include -Inative/platform/compat native/tools/fps/slip_budget_unit.cpp -o /tmp/slip_budget_unit && /tmp/slip_budget_unit
// -DNP_SLIP_BREAK=1..4 must FAIL (np_slip.h: 1 refill x10, 2 a cut is not paid for, 3 no ceiling on the bucket, 4 an on-time frame is cut): run_slip_budget_test.sh.
#define NP_NATIVE 1
#include "../../platform/compat/np_fps.cpp"
#include "gs_stub.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static bool g_lan = false;
bool NpLanActive() { return g_lan; }   // (strong definition for the weak reference in np_fps.cpp)

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

struct Res {
    double realS = 0, advS = 0, lostMs = 0, speed = 1, stepsPerS = 0, worst2sPct = 0, worstLostMs2s = 0;
    unsigned maxStepsInFrame = 0, frames = 0;
    double onTimeTouched = 0;   // ms taken off frames that were within the limit
};

// One run: dts (seconds, as the real frame lengths; a value over 0.1 is clamped the way nlTask.cpp clamps it) with the display interval in force (1 = a 60 target, 2 = held 30 / paced).
// budgetMs 0: the stateless rule. The simulation clock is a 20 ms accumulator fed with what NpSlipDelta returns (the FixedUpdateTask's task: asked once per frame, and the way the
// real frame asks the other tasks is covered below).
Res Run(const std::vector<float>& dts, int interval, float budgetMs) {
    char b[16];
    snprintf(b, sizeof b, "%g", (double)budgetMs);
    setenv("MSC_SLIP", "20", 1);
    setenv("MSC_SLIP_STEPS", "2", 1);
    setenv("MSC_SLIP_BUDGET_MS", b, 1);
    setenv("MSC_SLIP_REFILL_PCT", "2", 1);
    g_slipPct = -1;   // read again (also a fresh bucket and ledger)
    g.policy = interval == 1 ? npfps::kPolicy60 : npfps::kPolicy30;
    g.paced = false;
    Res r;
    double acc = 0.0;
    std::vector<double> real, lost;
    const float limit = (float)interval * (1001.0f / 60000.0f) * 1.05f;
    for (float raw : dts) {
        const float dt = raw > 0.1f ? 0.1f : raw;
        SlipFrame(raw, 2);
        const float adv = NpSlipDelta(dt);
        if (dt <= limit && adv != dt) r.onTimeTouched += (double)(dt - adv) * 1000.0;
        r.realS += dt;
        r.advS += adv;
        acc += adv;
        unsigned n = 0;
        while (acc >= 0.02 - 1e-9) { acc -= 0.02; ++n; }
        if (n > r.maxStepsInFrame) r.maxStepsInFrame = n;
        real.push_back(dt);
        lost.push_back((double)(dt - adv));
        ++r.frames;
    }
    r.lostMs = (r.realS - r.advS) * 1000.0;
    r.speed = r.advS / r.realS;
    r.stepsPerS = (r.advS / 0.02) / r.realS;   // (steps = game time / 20 ms, up to the remainder)
    for (size_t i = 0; i < real.size(); ++i) {   // worst window of at least 2 s of real time
        double rr = 0, ll = 0;
        for (size_t j = i; j < real.size() && rr < 2.0; ++j) { rr += real[j]; ll += lost[j]; }
        if (rr >= 2.0 && ll / rr * 100.0 > r.worst2sPct) { r.worst2sPct = ll / rr * 100.0; r.worstLostMs2s = ll * 1000.0; }
    }
    return r;
}
std::vector<float> Steady(float ms, double seconds) { return std::vector<float>((size_t)(seconds * 1000.0 / ms + 0.5), ms * 0.001f); }
std::vector<float> Cat(std::vector<float> a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
std::vector<float> WithHitch(float base, double seconds, double atS, float hitchMs) {
    std::vector<float> v = Steady(base, seconds);
    double t = 0;
    for (size_t i = 0; i < v.size(); ++i) { t += v[i]; if (t >= atS) { v[i] = hitchMs * 0.001f; break; } }
    return v;
}
} // namespace

int main() {
    setenv("MSC_FPS", "30", 1);
    setenv("MSC_BOOT_FPS", "0", 1);
    setenv("MSC_FPS_LOG", "0", 1);
    NpFpsBeginFrame(0.0334f, 2);   // starts the policy
    const float kOld = 0.0f, kNew = 30.0f;
    struct Row { const char* name; std::vector<float> dts; int interval; bool sustained; };
    std::vector<Row> rows;
    rows.push_back({"60 target met: 16.7 ms frames (interval 1)", Steady(16.683f, 30), 1, false});
    rows.push_back({"60 target missed: 20 ms frames (interval 1)", Steady(20, 30), 1, true});
    rows.push_back({"60 target missed: 25 ms frames (interval 1)", Steady(25, 30), 1, true});
    rows.push_back({"60 target missed: 33.4 ms frames (interval 1)", Steady(33.4f, 30), 1, true});
    rows.push_back({"held 30 on time: 33.4 ms frames (interval 2)", Steady(33.4f, 30), 2, false});
    rows.push_back({"held 30 misses: 40 ms frames (interval 2)", Steady(40, 30), 2, true});
    rows.push_back({"held 30 misses: 45 ms frames (interval 2)", Steady(45, 30), 2, true});
    rows.push_back({"held 30 misses: 50 ms frames (interval 2)", Steady(50, 30), 2, true});
    {   // free-running pace (interval 2 is its slip interval): 17-33 ms frames, all within 35 ms
        std::vector<float> v;
        const float cyc[4] = {16.7f, 25.0f, 33.4f, 20.0f};
        for (int i = 0; i < 1500; ++i) v.push_back(cyc[i % 4] * 0.001f);
        rows.push_back({"paced free run: 16.7/25/33.4/20 ms mix (interval 2)", v, 2, false});
    }
    rows.push_back({"single 70 ms hitch in 33.4 ms frames, 30 s (interval 2)", WithHitch(33.4f, 30, 10, 70), 2, false});
    rows.push_back({"single 70 ms hitch in 16.7 ms frames, 30 s (interval 1)", WithHitch(16.683f, 30, 10, 70), 1, false});
    rows.push_back({"single 200 ms hitch in 16.7 ms frames, 30 s (interval 1; clamped to 100)", WithHitch(16.683f, 30, 10, 200), 1, false});
    rows.push_back({"LADDERHOLD wait: 20 s at 16.7, then 8 s of 33.4 ms frames at interval 1, then 16.7 again", Cat(Cat(Steady(16.683f, 20), Steady(33.4f, 8)), Steady(16.683f, 10)), 1, false});
    {   // a 70 ms hitch every 150 pictures (the lessons8 model's), 120 s at 33.4
        std::vector<float> v = Steady(33.4f, 120);
        for (size_t i = 150; i < v.size(); i += 150) v[i] = 0.070f;
        rows.push_back({"70 ms hitch every 150 pictures at 33.4 ms (interval 2), 120 s", v, 2, false});
    }
    printf("     %-92s | stateless rule: speed  lost ms  worst 2 s  max steps/frame | budget 30 ms: speed  lost ms  worst 2 s  steps/s  max steps/frame\n", "state (30 s unless said)");
    std::vector<Res> olds, news;
    for (const Row& r : rows) {
        const Res o = Run(r.dts, r.interval, kOld), n = Run(r.dts, r.interval, kNew);
        olds.push_back(o);
        news.push_back(n);
        printf("     %-92s | %20.1f %% %8.0f %8.2f %% %6u | %19.1f %% %8.0f %8.2f %% %8.2f %6u\n", r.name, 100 * o.speed, o.lostMs, o.worst2sPct, o.maxStepsInFrame, 100 * n.speed, n.lostMs, n.worst2sPct,
               n.stepsPerS, n.maxStepsInFrame);
    }
    const double kRefill = 0.02, kBucketMs = 30.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const Res& o = olds[i];
        const Res& n = news[i];
        double span = n.realS;
        char what[200];
        if (r.sustained) {
            snprintf(what, sizeof what, "[%s] stateless rule: the sustained slow motion is real (>= 10 %% lost)", r.name);
            Check(o.speed < 0.90, what);
            snprintf(what, sizeof what, "[%s] budget: the game clock loses at most 2 %% + the bucket over 30 s", r.name);
            Check(n.lostMs <= (kRefill * span * 1000.0 + kBucketMs) * 1.01 + 0.5, what);
            snprintf(what, sizeof what, "[%s] budget: speed >= 97.5 %% of real time", r.name);
            Check(n.speed >= 0.975, what);
        }
        snprintf(what, sizeof what, "[%s] budget: any 2 s window loses at most refill x 2 s + bucket (= 3.5 %%)", r.name);
        Check(n.worstLostMs2s <= (kRefill * 2.0 * 1000.0 + kBucketMs) * 1.01 + 0.5, what);
        snprintf(what, sizeof what, "[%s] budget: never loses more than the stateless rule did", r.name);
        Check(n.lostMs <= o.lostMs + 0.5, what);
        snprintf(what, sizeof what, "[%s] budget: no frame within its limit is touched", r.name);
        Check(n.onTimeTouched == 0.0, what);
    }
    // an on-time scene is exactly 50 steps a second
    Check(fabs(news[0].stepsPerS - 50.0) < 0.1 && fabs(news[4].stepsPerS - 50.0) < 0.1 && fabs(news[8].stepsPerS - 50.0) < 0.1, "on-time scenes (60 met, held 30 on time, paced mix): 50 steps a second, nothing lost");
    // a single hitch is still cut, by what the old rule cut (the cap of two steps = 30 ms of a 70 ms frame), and the clock is back to exact afterwards
    {
        const Res& o = olds[9];
        const Res& n = news[9];
        Check(n.lostMs > 20.0 && n.lostMs <= o.lostMs + 0.5, "single 70 ms hitch (held 30): cut by the old rule's amount (a brief absorber), not more");
        Check(n.lostMs < 31.0, "single 70 ms hitch: the whole loss of a 30 s run is that one cut");
        Check(news[10].lostMs > 20.0 && news[10].lostMs < 31.0, "single 70 ms hitch at 60: the same");
        Check(news[11].lostMs <= 30.5 && olds[11].lostMs > 55.0, "200 ms hitch: the old rule loses 60 ms (cap 40 of the clamped 100), the budget 30 ms at most");
        // a hitch every 150 pictures (5 s): the bucket has filled again (100 ms at 2 % > 30)
        Check(news[13].lostMs < 0.02 * 120.0 * 1000.0 + kBucketMs, "a hitch every 5 s: the clock loses ~0.6 %, inside the 2 % bound");
        Check(news[13].lostMs > 0.9 * olds[13].lostMs, "... every hitch is cut as before (the bucket has refilled between them: spread out, not made up in one frame)");
    }
    // LADDERHOLD wait: 8 s of 33.4 ms frames at a 60 target: the stateless rule loses 17 % of 8 s = 1.3 s, the budget 2 % of 8 s + 30 ms
    {
        const Res& o = olds[12];
        const Res& n = news[12];
        printf("     LADDERHOLD wait (8 s at 33.4 ms against a 60 target): stateless %.0f ms lost, budget %.0f ms lost\n", o.lostMs, n.lostMs);
        Check(o.lostMs > 1000.0 && n.lostMs < 0.02 * 8000.0 + kBucketMs + 40.0, "LADDERHOLD wait: >1 s of game time lost by the stateless rule, under 0.2 s with the budget (even after 20 s of calm filled the bucket)");
    }
    // the tasks of one frame agree: three tasks of a frame ask with slightly different dt, one decision
    {
        setenv("MSC_SLIP_BUDGET_MS", "30", 1);
        g_slipPct = -1;
        g.policy = npfps::kPolicy60;
        SlipFrame(0.040f, 2);
        const float a = NpSlipDelta(0.040f), b = NpSlipDelta(0.0402f), c = NpSlipDelta(0.0399f);
        Check(fabsf(a / 0.040f - b / 0.0402f) < 1e-5f && fabsf(a / 0.040f - c / 0.0399f) < 1e-5f && a < 0.040f, "every task of a frame is advanced by the same fraction of its own dt");
        SlipFrame(0.040f, 3);   // paused: not a gameplay frame
        Check(NpSlipDelta(0.040f) == 0.040f, "a paused / non-gameplay frame is not slipped");
        SlipFrame(0.0165f, 2);
        Check(NpSlipDelta(0.0165f) == 0.0165f, "an on-time frame after a late one is passed through exactly");
        g_lan = true;
        SlipFrame(0.100f, 2);
        Check(NpSlipDelta(0.100f) == 0.100f, "LAN / online session: dt unchanged (single player only)");
        g_lan = false;
        setenv("MSC_SLIP", "0", 1);
        g_slipPct = -1;
        SlipFrame(0.100f, 2);
        Check(NpSlipDelta(0.100f) == 0.100f, "MSC_SLIP=0: off whatever the budget says");
        setenv("MSC_SLIP", "20", 1);
        setenv("MSC_SLIP_BUDGET_MS", "0", 1);
        g_slipPct = -1;
        SlipFrame(0.100f, 2);
        Check(NpSlipDelta(0.100f) < 0.0401f, "budget 0: the stateless rule, as before wave 9");
    }
    printf("%d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
