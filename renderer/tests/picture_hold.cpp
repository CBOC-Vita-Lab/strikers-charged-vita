// MSC_PICTURE_HOLD (vgx_hold.h, the shipped header): the FULL picture policy on simulated GPU-time traces. Pure: no GL, no clock.
//   picture_hold [--break=N]   N=1: a size step is taken although no render target of that size was made (the no-allocation check must fail),
//                              N=2: no proof and no block before a step is given back (the flapping checks must fail),
//                              N=3: the policy goes on measuring and moving while frozen (the freeze checks must fail)
//                              N=4: an anti-aliasing step is taken although the target with those samples was not made (check 13c must fail)
//                              N=6: the old warm-up gate (12288 KiB) instead of the trim floor (checks 14a)
//                              N=7: a stadium change re-arms only the targets marked skipped, not the ones that have gone (14b)
//                              N=8: the held size's target is never re-made when it is missing (14c)
//                              N=9: the measured cost of a step is not used when it is given back (check 15a/15b must fail)
//                              N=10: the flap test window equals the gap again (15c must fail)
//                              N=11: the memory plan lets 4x through whatever is free (15e must fail)
//                              N=12: the stage test is turned round (the period is used only when the GPU is the limit: 16a, 16b must fail)
//                              N=13: the budget is always 16.7 ms (16a, 16b must fail)
//                              N=14: the ladder is not a prefix of the kept sizes (16d must fail)
//                              N=15: nothing is let go before kick-off for room (16d must fail)
//                              N=16: the new order's drop order is the old one (17d must fail)
//                              N=17: the new order is not in force: the samples go before the sizes (17a, 17b must fail)
//                              N=18: the new ladder is not a prefix of the sizes kept (17d must fail)
//                              N=30: a held scene of one sample takes a size level whose 2x target is kept, not its one-sample target (heldstep23: 30a, 30b must fail)
//                              N=40: the GPU-busy gate is off: every late picture counts (MSC_PICTURE_HOLD_GPUBUSY: 40a must fail)
//                              N=41: the gate reads the GPU as the limit at 40 % of the period (40a must fail)
//                              N=42: a step is never given back while the gate is on (40d must fail)
//                              N=5: the samples are never given up before the size (a size step with samples still on: checks 13a, 13b, 13f must fail)
// The GPU time of a frame is base - (savings of the steps in force) with a size step buying pixPart x (1 - scale^2) of the pixel part, plus noise; the policy sees a measured frame at a
// time (the console's notifications come two frames late: the policy's own `ema` reset after a move stands for the frames that are at the old state).
// Every number is an estimate: nothing here ran on a console.
#include "vgx_hold.h"
#include "vgx_hold_bg.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vgx::hold;

namespace {
int g_fail = 0, g_pass = 0, g_break = 0;
void Check(bool ok, const char* what, const char* detail = "") {
    if (ok) { ++g_pass; std::printf("ok   %s %s\n", what, detail); }
    else { ++g_fail; std::printf("FAIL %s %s\n", what, detail); }
}
const float kBudget = 1001.0f / 60.0f;

struct Move { int at, from, to; };

struct Sim {
    Policy p;
    float base = 19.0f;       // ms at the held size with no step
    float pixPart = 8.0f;     // of it, what follows the pixels
    unsigned seed = 12345;
    float noise = 0.8f;
    float glowGain = 2.0f;    // pace13: what the glow step really buys (MSC_GLOW_HALF_PLAY=1), when p.glowStep
    int frame = 0;
    std::vector<Move> moves;
    int sizeWhileNotReady = 0;
    int aaIntoUnready = 0;
    bool sawNoRoom = false;
    Pacing* pace = nullptr;   // (MSC_PICTURE_HOLD_PACE: the budget the policy reads comes from the pictures' period)
    float period = 0.0f;      // ms between pictures when the pace is not the GPU's
    unsigned ready() const { return p.readyMask; }
    void Order(bool last) { p.SetOrder(g_break == 17 ? false : last, 3); }
    void Aa(int top, bool have2 = true) { p.SetAa(top, have2); if (g_break == 5) p.off[kAa1] = p.off[kAa2] = true; }
    Sim() { p.readyMask = 0xF; p.off[kFxSize] = p.off[kCrowd] = true; if (g_break == 2) { p.calmFrames = 1; p.sizeCalmFrames = 1; p.blockMax = 0; p.upAt = 5.0f; } }
    float Noise() { seed = seed * 1664525u + 1013904223u; return (static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * noise; }
    float Ms() const {
        float m = base * p.AaFactor(p.Samples(p.step)) / (p.aaTop >= 2 ? p.AaFactor(p.aaTop) : 1.0f);   // (base is the time with the configured samples; fewer samples cost less)
        for (int s = 1; s <= p.step && s <= kCrowd; ++s) m -= (p.glowStep && s == kFxSize ? glowGain : p.save[s]) * (s == kShadowFar ? 0.5f : 1.0f);   // the far step buys half of what the table says
        const int l = p.Lvl(p.step);
        if (l > 0) m -= pixPart * (1.0f - p.scale[l] * p.scale[l]);
        return m;
    }
    int One(float ms) {
        const int from = p.step;
        const unsigned real = p.readyMask, realAa = p.aaReady;
        if (g_break == 1) p.readyMask = 0xF;   // (the fault: the policy believes every target exists)
        if (g_break == 4) p.aaReady = 7;
        float budget = kBudget;
        if (pace) {
            pace->Add(period > 0.0f ? std::max(period, ms) : std::max(kBudget, ms), ms);   // (a GPU slower than the period delays the pictures: the period is then the GPU's)
            budget = pace->Budget();
            if (g_break == 12) budget = pace->limited ? std::max(kBudget, pace->per) : kBudget;   // (the stage test turned round)
            if (g_break == 13) budget = kBudget;
        }
        const int to = p.Frame(ms, budget, false);
        p.readyMask = real;
        p.aaReady = realAa;
        if (p.noRoom) sawNoRoom = true;
        ++frame;
        if (to >= 0) {
            moves.push_back({frame, from, to});
            if (p.Lvl(to) > 0 && p.IsSize(to) && !(real & (1u << p.Lvl(to)))) ++sizeWhileNotReady;
            if (to < kFirstSize && to > from && to != from && !p.AaReady(p.Samples(to)) && p.Samples(to) != p.Samples(from)) ++aaIntoUnready;
        }
        return to;
    }
    void Run(int frames) { for (int i = 0; i < frames; ++i) One(Ms() + Noise()); }
    int downs() const { int n = 0; for (const Move& m : moves) if (m.to > m.from) ++n; return n; }
    int ups() const { int n = 0; for (const Move& m : moves) if (m.to < m.from) ++n; return n; }
    int minGap() const { int g = 1 << 30; for (size_t i = 1; i < moves.size(); ++i) g = std::min(g, moves[i].at - moves[i - 1].at); return g; }
    // moves down and up that undo the one before (an oscillation: a step taken, given back, taken again)
    int reversals() const { int n = 0; for (size_t i = 2; i < moves.size(); ++i) if (moves[i].to > moves[i].from && moves[i - 1].to < moves[i - 1].from && moves[i - 2].to > moves[i - 2].from) ++n; return n; }
};
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (!std::strncmp(argv[i], "--break=", 8)) g_break = std::atoi(argv[i] + 8);

    // 1 an open-play load that fits: no step at all
    { Sim s; s.base = 14.5f; s.Run(1800); Check(s.p.step == 0 && s.moves.empty(), "a frame that fits takes no step"); }

    // 2 a mild overload is shed in the fixed order and the size is not touched while the cheap steps are enough
    { Sim s; s.base = 15.7f; s.p.fit = 0.93f; s.Run(2400);
      bool mono = true; for (size_t i = 0; i < s.moves.size(); ++i) if (s.moves[i].to <= s.moves[i].from) mono = false;
      char d[96]; std::snprintf(d, sizeof d, "(%lu moves, final %d, ms %.1f)", static_cast<unsigned long>(s.moves.size()), s.p.step, s.Ms());
      Check(mono && s.p.step >= 1 && s.p.step < kFirstSize && s.Ms() < 0.92f * kBudget, "a mild overload is shed by the cheap steps and the size is not touched", d); }

    // 3 a heavy load needs the size steps: they come last, in order, one level at a time, and the policy stops where the frame fits
    { Sim s; s.base = 19.0f; s.Run(3600);
      int firstSize = -1; for (const Move& m : s.moves) if (m.to >= kFirstSize) { firstSize = m.to; break; }
      bool shedFirst = s.moves.front().from == 0 && s.moves.front().to >= 1;
      bool seq = true; for (size_t i = 1; i < s.moves.size(); ++i) if (s.moves[i].from != s.moves[i - 1].to) seq = false;
      char d[128]; std::snprintf(d, sizeof d, "(final step %d, size level %d, ms %.1f, first size step %d)", s.p.step, SizeLevel(s.p.step), s.Ms(), firstSize);
      Check(shedFirst && seq && s.p.step >= kFirstSize && s.Ms() < 0.92f * kBudget && s.p.downs == static_cast<int>(s.moves.size()), "a heavy load takes the shed steps first, then size steps until the frame fits", d);
      Check(s.p.step < kOldLast, "and it does not go to the floor when a higher size fits"); }

    // 3b the continuation: a load no shed step and no size above the floor can fix goes to the floor and stays, no more moves
    { Sim s; s.base = 27.0f; s.pixPart = 12.0f; s.Run(7200);
      char d[96]; std::snprintf(d, sizeof d, "(final %d, moves %lu, ms %.1f)", s.p.step, static_cast<unsigned long>(s.moves.size()), s.Ms());
      Check(s.p.step == kOldLast && s.p.noRoom, "a load that cannot fit goes to the floor of the ladder and says so", d);
      const int at = s.moves.back().at; const int before = static_cast<int>(s.moves.size());
      s.Run(3600);
      Check(static_cast<int>(s.moves.size()) == before && at < 1500, "and sits there without any move (no flapping at the floor), reached in under 25 s", ""); }

    // 3c a stadium that needs everything gets there fast: severity takes several steps in one decision
    { Sim s; s.base = 24.0f; s.pixPart = 11.0f; s.Run(1200);
      char d[96]; std::snprintf(d, sizeof d, "(step %d after 20 s, %d jumps, first move %d -> %d)", s.p.step, s.p.jumps, s.moves.empty() ? -1 : s.moves[0].from, s.moves.empty() ? -1 : s.moves[0].to);
      Check(s.p.jumps >= 1 && !s.moves.empty() && s.moves[0].to - s.moves[0].from >= 2 && s.p.step >= kFirstSize, "a big overload takes more than one step in a decision", d); }

    // 4 a size step needs a render target made ahead: none, no step (and the policy says there is no room)
    { Sim s; s.p.readyMask = 1; s.base = 21.5f; s.Run(2400);
      char d[96]; std::snprintf(d, sizeof d, "(final %d, taken while not ready %d)", s.p.step, s.sizeWhileNotReady);
      Check(s.sizeWhileNotReady == 0 && s.p.step == kShedSteps && s.p.noRoom, "no size step while no target of that size is ready (a step never allocates in play)", d); }
    // 4b only the first target is ready: one size step and no more
    { Sim s; s.p.readyMask = 3; s.base = 27.0f; s.pixPart = 12.0f; s.Run(2400);
      Check(s.p.step == kSize1 && s.sizeWhileNotReady == 0, "with one target ready the policy stops at its size"); }

    // 5 the load goes away: steps come back in the reverse order, none before its proof, the size steps slowest
    { Sim s; s.base = 24.0f; s.pixPart = 11.0f; s.Run(2400); const int top = s.p.step; s.base = 12.0f; s.moves.clear(); const int t0 = s.frame;
      for (int i = 0; i < 12000 && s.p.step > 0; ++i) s.One(s.Ms() + s.Noise());
      bool rev = !s.moves.empty(); for (const Move& m : s.moves) if (m.to >= m.from) rev = false;
      const int first = s.moves.empty() ? -1 : s.moves[0].at - t0;
      char d[128]; std::snprintf(d, sizeof d, "(from step %d: first step back after %d frames, %lu steps, min gap %d frames, all back %s)", top, first, static_cast<unsigned long>(s.moves.size()), s.minGap(), s.p.step == 0 ? "yes" : "no");
      Check(rev && s.p.step == 0 && first >= s.p.sizeCalmFrames && s.minGap() >= s.p.gap, "steps are given back one at a time, last taken first, the size steps only after their long proof", d); }

    // 6 a load that sits just between two states does not flap
    { Sim s; s.base = 17.3f; s.Run(3600 * 3);
      char d[128]; std::snprintf(d, sizeof d, "(%lu moves in 3 minutes, %d flaps, %d reversals, min gap %d)", static_cast<unsigned long>(s.moves.size()), s.p.flaps, s.reversals(), s.minGap());
      Check(s.moves.size() <= 12 && s.reversals() == 0, "a load between two states does not flap", d); }

    // 7 spikes: a short burst of late frames does not move a step, a long one does
    { Sim s; s.base = 14.0f; s.Run(600);
      for (int i = 0; i < 8; ++i) s.One(25.0f);
      Check(s.p.step == 0, "eight late frames in a row do not move a step");
      for (int i = 0; i < 40; ++i) s.One(22.0f);
      Check(s.p.step >= 1, "a long run of late frames moves a step"); }

    // 8 not live play: freeze keeps the state, measures nothing, and thaw starts afresh
    { Sim s; s.base = 21.5f; s.Run(1500); const int st = s.p.step; const int mv = static_cast<int>(s.moves.size());
      if (g_break != 3) s.p.Freeze();
      for (int i = 0; i < 3000; ++i) s.One(30.0f + s.Noise());   // a replay's time at 30 fps: far over the 60 budget
      Check(s.p.step == st && static_cast<int>(s.moves.size()) == mv && s.p.ema < 0.0f, "frozen: a replay's frame times move nothing and are not remembered");
      s.p.Thaw(); s.base = 21.5f;
      Check(!s.p.frozen && s.p.ema < 0.0f && s.p.wait >= s.p.gap / 2, "thawed: the measurement starts afresh and the first decision waits");
      s.Run(60); Check(s.p.step == st, "and the state at the cut back is the one before the cut"); }

    // 9 holdUp (the effects governor still has a step) blocks a move
    { Policy p; p.readyMask = 0xF; for (int i = 0; i < 100; ++i) p.Frame(25.0f, kBudget, true); Check(p.step == 0, "no step while the effects governor is working"); }

    // 9b no effects pass (or no compositor): the effects step buys nothing and is passed over, in both directions
    { Sim s; s.p.fxLive = false; s.base = 17.2f; s.Run(1200);
      bool no1 = true; for (const Move& m : s.moves) if (m.to == kFxSize) no1 = false;
      Check(no1 && s.p.step >= kShadowFar, "without an effects pass the effects step is passed over on the way down");
      s.base = 12.0f; s.moves.clear(); for (int i = 0; i < 6000 && s.p.step > 0; ++i) s.One(s.Ms() + s.Noise());
      no1 = true; for (const Move& m : s.moves) if (m.to == kFxSize) no1 = false;
      Check(no1 && s.p.step == 0, "and on the way up"); }

    // 9b2 MSC_PICTURE_HOLD_FX=1 (the effects target in the order, megafx4's rule): it is the FIRST step down and the LAST back
    { Sim s; s.p.off[kFxSize] = false; s.base = 17.2f; s.Run(1800);
      Check(!s.moves.empty() && s.moves.front().from == 0 && s.moves.front().to >= kFxSize && s.p.step >= kFxSize, "with the effects step in the order, the first move down includes the effects target");
      s.base = 12.0f; s.moves.clear(); for (int i = 0; i < 9000 && s.p.step > 0; ++i) s.One(s.Ms() + s.Noise());
      bool lastFx = !s.moves.empty() && s.moves.back().from == kFxSize && s.moves.back().to == 0;
      for (size_t i = 0; i + 1 < s.moves.size(); ++i) if (s.moves[i].to < kFxSize) lastFx = false;
      Check(lastFx && s.p.step == 0, "and the effects target is the last thing given back"); }

    // 9c the size step could not be made: back to the shed steps, and not tried again for a long time
    { Sim s; s.base = 24.0f; s.pixPart = 11.0f; s.Run(2400); Check(s.p.step >= kFirstSize, "(precondition) a size step is in force");
      s.p.SizeFailed(); Check(s.p.step == kShedSteps && s.p.sizeFail >= 3600, "a size step that could not be made is taken back and not tried again for a long time");
      int sizeAgain = 0; for (int i = 0; i < 1800; ++i) { const int to = s.p.Frame(s.Ms() + 5.0f, kBudget, false); if (to >= kFirstSize) ++sizeAgain; }
      Check(sizeAgain == 0, "not even when the renderer says a target is ready and the frames stay over");
      int late = 0; for (int i = 0; i < 9000 && !late; ++i) { const int to = s.p.Frame(s.Ms() + 5.0f, kBudget, false); if (to >= kFirstSize) late = i; }
      Check(late > 0, "after the wait it is tried again"); }

    // 9d a failed step up doubles the wait before the next try
    { Sim s; s.base = 17.0f; s.Run(1800); const int st = s.p.step; Check(st >= 1, "(precondition) a step is in force");
      // the load drops for a while, the step is given back, the load returns: the step is needed again soon
      s.base = 11.0f; for (int i = 0; i < 3000 && s.p.step == st; ++i) s.One(s.Ms() + s.Noise());
      const int back = s.p.step; s.base = 19.0f; int guard = 0; while (s.p.step <= back && guard++ < 600) s.One(s.Ms() + s.Noise());
      const int w1 = s.p.block[st];
      Check(s.p.fails[st] >= 1 && w1 >= s.p.calmFrames * 2 - 1, "a step taken again soon after its return is blocked for twice its proof"); }

    // 10 traces (an estimate of the load, not a console recording): a steady 17.5, a 15/19 alternation, a slow drift, a spike train.
    //    No oscillation, and never more than one move per `gap` frames going down or per proof going up.
    struct Trace { const char* name; };
    {   // 10a steady 17.5 ms at the held size (open play 960x544 in the budget model)
        Sim s; s.base = 17.5f; s.noise = 0.6f; s.Run(3600 * 4);
        char d[128]; std::snprintf(d, sizeof d, "(final %d, %lu moves, %d reversals, min gap %d frames, ms %.1f)", s.p.step, static_cast<unsigned long>(s.moves.size()), s.reversals(), s.minGap(), s.Ms());
        Check(s.reversals() == 0 && s.minGap() >= s.p.gap && s.Ms() < 0.95f * kBudget && s.moves.size() <= 10, "steady 17.5 ms: settles under the budget with no oscillation", d); }
    {   // 10b 15 / 19 alternation, every other frame (mean 17: the mean is over the budget, the frames alternate)
        Sim s; s.base = 17.0f; s.noise = 0.0f; int k = 0;
        for (int i = 0; i < 3600 * 4; ++i) { const float alt = (k++ & 1) ? 2.0f : -2.0f; s.One(s.Ms() + alt); }
        char d[128]; std::snprintf(d, sizeof d, "(final %d, %lu moves, %d reversals, min gap %d frames)", s.p.step, static_cast<unsigned long>(s.moves.size()), s.reversals(), s.minGap());
        Check(s.reversals() == 0 && s.minGap() >= s.p.gap && s.moves.size() <= 10, "15/19 alternation: no oscillation", d); }
    {   // 10c slow drift: the load climbs from 13 to 20 ms over 4 minutes and falls back over 4 minutes
        Sim s; s.noise = 0.5f;
        for (int i = 0; i < 3600 * 8; ++i) { const float t = static_cast<float>(i) / 3600.0f; s.base = 13.0f + 7.0f * (t < 4.0f ? t / 4.0f : (8.0f - t) / 4.0f); s.One(s.Ms() + s.Noise()); }
        char d[128]; std::snprintf(d, sizeof d, "(final %d, %lu moves: %d down %d up, %d reversals, min gap %d)", s.p.step, static_cast<unsigned long>(s.moves.size()), s.downs(), s.ups(), s.reversals(), s.minGap());
        Check(s.reversals() <= 1 && s.minGap() >= s.p.gap && s.moves.size() <= 24, "slow drift up and down: steps follow it without oscillation", d);
        Check(s.p.step <= 1, "and are given back as the load goes away"); }
    {   // 10d spike train: 8 ms late frames (25 ms) every 3 seconds on a 14 ms load
        Sim s; s.base = 14.0f; s.noise = 0.4f;
        for (int i = 0; i < 3600 * 4; ++i) { const float ms = (i % 180) < 3 ? 25.0f : s.Ms() + s.Noise(); s.One(ms); }
        Check(s.moves.empty(), "a spike train on a load that fits moves nothing"); }
    {   // 10e spike train on a load at the edge (16 ms): late frames count, but moves stay few
        Sim s; s.base = 16.0f; s.noise = 0.4f;
        for (int i = 0; i < 3600 * 4; ++i) { const float ms = (i % 120) < 4 ? 24.0f : s.Ms() + s.Noise(); s.One(ms); }
        char d[96]; std::snprintf(d, sizeof d, "(final %d, %lu moves, %d reversals)", s.p.step, static_cast<unsigned long>(s.moves.size()), s.reversals());
        Check(s.reversals() == 0 && s.moves.size() <= 8 && s.minGap() >= s.p.gap, "a spike train at the edge: few moves, no oscillation", d); }
    {   // 10f the stadiums of the budget table (estimates at 960x544): Sand Tomb 18.5, Pipeline 15.9, Storm Ship 15.2, Mega 16.9
        const float load[4] = {18.5f, 15.9f, 15.2f, 16.9f};
        const char* nm[4] = {"Sand Tomb", "Pipeline", "Storm Ship", "Mega"};
        for (int k = 0; k < 4; ++k) {
            Sim s; s.base = load[k]; s.noise = 0.7f; s.Run(3600 * 3);
            char d[128]; std::snprintf(d, sizeof d, "%s %.1f ms: final step %d (%s), %.1f ms, %lu moves", nm[k], load[k], s.p.step, StepName(s.p.step), s.Ms(), static_cast<unsigned long>(s.moves.size()));
            Check(s.Ms() < 0.95f * kBudget && s.reversals() == 0 && s.moves.size() <= 8, "the table's load settles under the budget, few moves", d);
        } }

    // 11 per-stadium memory: what a match learned, where the next one starts, and never a size step assumed
    { unsigned fr[kSteps + 1] = {}; fr[0] = 200; fr[2] = 3000; fr[3] = 500;
      Check(Learned(fr) == 2, "the step a match spent most of its play in is what it learned");
      unsigned few[kSteps + 1] = {}; few[3] = 600;
      Check(Learned(few) == -1, "a short match learns nothing");
      unsigned deep[kSteps + 1] = {}; deep[kSize2] = 5000;
      Check(Learned(deep) == kSize2 && StartStep(kSize2, false) == kShedSteps, "a match that lived at a size step starts the next at the last shed step, not at a size");
      Check(StartStep(-1, true) == kShadowFar && StartStep(-1, false) == 0, "a heavy stadium starts with two shed steps taken, others with none");
      Check(StartStep(1, true) == kShadowFar && StartStep(3, true) == 3 && StartStep(kAa1, true) == kAa1, "a heavy stadium starts at the higher of its default and what was learned");
      StadiumMemory m; Check(m.Get(8) == -1, "no memory at first"); m.Put(8, 3); m.Put(40, 2); m.Put(-1, 2);
      Check(m.Get(8) == 3 && m.Get(40) == -1 && m.Get(-1) == -1, "remembered per stadium, unknown stadiums are not remembered"); }

    // 11b Start() takes the step and forgets old failures
    { Policy p; p.readyMask = 0xF; p.fails[3] = 4; p.block[3] = 999; p.Start(2);
      Check(p.step == 2 && p.fails[3] == 0 && p.block[3] == 0 && p.wait >= p.gap, "a match starts at its step with a clean record"); }

    // 13 anti-aliasing (MSC_PICTURE_HOLD_MSAA): the samples are the cheapest thing given up after the shed steps and before any size; they come back on the same proof, size first
    //    13a 4 samples: a mild overload takes 4 -> 2 first (not a size), a bigger one 2 -> 1, and a size step comes only once the samples are 1
    { Sim s; s.Aa(4); s.base = 19.5f; s.p.fit = 0.93f; s.Run(300);
      int first = s.moves.empty() ? -1 : s.moves.front().to;
      Check(first >= kAa1 && s.p.Samples(s.p.step) <= 2 && SizeLevel(s.p.step) == 0, "4 samples: a mild overload is met by fewer samples, not by a smaller picture", "");
      s.Run(7200);
      bool ok = true; for (const Move& m : s.moves) if (m.to >= kFirstSize && s.p.Samples(m.to) != 1) ok = false;
      for (const Move& m : s.moves) if (m.to >= kFirstSize) { bool passedAa = false; for (const Move& q : s.moves) if (q.at <= m.at && q.to >= kAa2) passedAa = true; if (!passedAa) ok = false; }
      Check(ok && s.p.Samples(kAa2) == 1 && s.p.Samples(kAa1) == 2 && s.p.Samples(kCrowd) == 4 && s.p.Samples(kSize1) == 1, "and a size step is taken only after the samples are given up (4 -> 2 -> 1), always at 1 sample", ""); }
    //    13b 2 samples: the only AA step is 2 -> 1
    { Sim s; s.Aa(2); s.base = 19.0f; s.Run(3600);
      Check(s.p.off[kAa1] && !s.p.off[kAa2] && s.p.Samples(kCrowd) == 2 && s.p.Samples(kAa1) == 2 && s.p.Samples(kAa2) == 1 && s.p.Samples(kSize3) == 1 && s.p.step >= kAa2, "2 samples: one step (2 -> 1) before any size, and the overload takes it"); }
    //    13c a sample count without its made target is not entered; with 4 and no 2-sample target the step goes 4 -> 1 at once
    { Sim s; s.Aa(4); s.p.aaReady = 1; s.base = 19.0f; s.p.readyMask = 0xF; s.Run(2400);
      Check(s.aaIntoUnready == 0 && s.sawNoRoom && s.p.step < kAa1 && s.p.Samples(s.p.step) == 4, "no AA step into a sample count whose target was not made: the policy waits and says there is no room (the renderer then makes the target, or takes the count out of the order)");
      Sim t; t.Aa(4, false); t.p.aaReady = 1; t.base = 19.0f; t.Run(2400);
      Check(t.p.off[kAa1] && t.p.Samples(kAa2) == 1 && t.p.Samples(kCrowd) == 4 && t.p.step >= kAa2 && t.aaIntoUnready == 0, "with no 2-sample target the order is 4 -> 1");
      Sim u; u.Aa(4); u.p.aaReady = 1u | 4u; u.base = 19.0f; u.Run(2400);
      Check(u.aaIntoUnready == 0 && u.sawNoRoom && u.p.step < kAa1, "with 4 made and 2 not, the policy waits for the 2-sample target rather than skipping it"); }
    //    13d the way back: size steps first, then 1 -> 2 -> 4, none before its proof, the samples slowest of the shed steps (they cost a target swap)
    { Sim s; s.Aa(4); s.base = 25.0f; s.pixPart = 11.0f; s.Run(3000); const int top = s.p.step; s.base = 13.5f; s.moves.clear();
      for (int i = 0; i < 20000 && s.p.step > 0; ++i) s.One(s.Ms() + s.Noise());
      bool order = !s.moves.empty(); int lastTo = 99; for (const Move& m : s.moves) { if (m.to >= m.from) order = false; lastTo = m.to; }
      char d[96]; std::snprintf(d, sizeof d, "(from step %d, %lu moves back, samples at the end %d)", top, static_cast<unsigned long>(s.moves.size()), s.p.Samples(s.p.step));
      Check(order && top >= kFirstSize && s.p.step == 0 && lastTo == 0 && s.p.Samples(0) == 4, "the samples come back last, after the sizes, one stop at a time", d); }
    //    13e no AA configured: nothing lands on or is counted at the AA steps, and the picture's behaviour is the old one
    { Sim s; s.Aa(0); s.base = 24.0f; s.pixPart = 11.0f; s.Run(2400);
      bool none = true; for (const Move& m : s.moves) if (m.to == kAa1 || m.to == kAa2) none = false;
      Check(none && s.p.Samples(0) == 1 && s.p.Samples(kSize3) == 1 && s.p.off[kAa1] && s.p.off[kAa2] && s.p.step >= kFirstSize, "with no multisampling the order is the old one (shed steps, then sizes)"); }
    //    13f whatever the configuration, a size step has one sample and a load that fits keeps the top samples
    { for (int top : {0, 2, 4}) { Sim s; s.Aa(top); s.base = 14.5f; s.Run(1800);
        char d[48]; std::snprintf(d, sizeof d, "(%d)", top);
        Check(s.p.Samples(kSize1) == 1 && s.p.Samples(kSize2) == 1 && s.p.Samples(kSize3) == 1 && s.moves.empty() && s.p.Samples(s.p.step) == (top ? top : 1), "a load that fits keeps the samples; a size step is always one sample", d); } }
    //    13g the estimate of what a sample step buys: 4 -> 2 saves more than 2 -> 1 per sample step down? (the model: relative cost 1 / 1.3 / 1.9 of the pixel part)
    { Policy p; p.SetAa(4); const float a = p.Save(kAa1, 16.0f), b = p.Save(kAa2, 16.0f); const float add = p.Add(kAa1, 14.0f);
      char d[96]; std::snprintf(d, sizeof d, "(4 -> 2 %.2f ms, 2 -> 1 %.2f ms, giving back 2 -> 4 at 14 ms adds %.2f ms; estimates)", a, b, add);
      Check(a > 0.5f && b > 0.5f && a > b * 0.9f && add > a * 0.7f && add < a * 1.6f, "the saving model of the sample steps is positive and the way back adds about what the way down saved", d); }

    // 14 the warm-up bookkeeping HoldTick uses (pure functions of vgx_hold.h): the gate, the re-arm at a stadium change, the held size's own target
    //    14a a target is made ahead only when the free memory after it stays at the trim floor, or TrimPool drops it within a second and it is marked unusable for a match
    { const long floorKib = 16384; bool agrees = true; int madeLow = 0;
      for (int freeKib = 0; freeKib < 60000; freeKib += 512) {
          const bool ok = g_break == 6 ? (freeKib >= 12288 + 2200) : WarmRoomOk(freeKib, 2200, static_cast<int>(floorKib));
          if (ok && freeKib < floorKib + 2200) { agrees = false; ++madeLow; }
      }
      Check(agrees && WarmRoomOk(-1, 2200) && !WarmRoomOk(14000, 2200) && WarmRoomOk(19000, 2200) && kTrimFloorKib == 16384, "a target is made ahead only with the trim floor left over (12 to 16 MiB free: not made, not dropped a second later)"); }
    //    14b a stadium change re-arms the skipped targets and the ones gone since
    { int st[5] = {1, 2, 3, 3, 0}; const bool kept[5] = {false, false, true, false, false};
      if (g_break == 7) { for (int i = 0; i < 5; ++i) if (st[i] == 2) st[i] = 0; } else RearmAtStadium(st, kept, 5);
      Check(st[0] == 0 && st[1] == 0 && st[2] == 3 && st[3] == 0 && st[4] == 0, "a stadium change asks again for a target that was skipped or has gone, and keeps one that is there"); }
    //    14c the held size's target missing while the picture is at a ladder size is made again; a size step then has a way back
    { bool remake = g_break == 8 ? false : HeldTargetMissing(false, false);
      Check(remake && !HeldTargetMissing(true, false) && !HeldTargetMissing(false, true), "the held size's target is made again when it is gone and the picture is at a smaller size");
      Policy p; p.readyMask = 0xE; p.Start(0); p.step = kSize1; for (int i = 0; i < 4000; ++i) p.Frame(8.0f, kBudget, false);
      Check(p.step == kSize1, "(why) without that target the policy cannot give the size step back: it stays low");
      p.readyMask = 0xF; for (int i = 0; i < 4000 && p.step >= kSize1; ++i) p.Frame(8.0f, kBudget, false);
      Check(p.step < kSize1, "and with it the step is given back"); }


    // 15 the cost of the anti-aliasing step is learned: a policy that guesses 1.1x / 1.3x / 1.9x / 2.5x against a truth of 1.1x..2.5x must not flip the step on and off
    //    The scene: 11.5 ms at one sample, the pixel part 45 %, the held 2 samples cost 1 - ps + ps * T. (Estimates throughout: nothing ran on a console.)
    {   const float guesses[4] = {1.1f, 1.3f, 1.9f, 2.5f}, truths[4] = {1.1f, 1.3f, 1.9f, 2.5f};
        int worst = 0, worstG = 0, worstT = 0, oneChange = 0, cases = 0;
        for (float g : guesses) for (float t : truths) {
            Policy p; p.readyMask = 0xF; p.SetAa(2, true); p.off[kFxSize] = p.off[kCrowd] = p.off[kShadowFar] = p.off[kShadowAll] = true;
            p.aaCost[1] = g;
            if (g_break == 10) { p.probeFrames = 180; p.sizeProbeFrames = 1200; }
            p.Start(0, 4);
            unsigned seed = 7; int moves = 0;
            for (int f = 0; f < 5 * 60 * 60; ++f) {   // 5 minutes
                seed = seed * 1664525u + 1013904223u;
                const float noise = (static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 0.6f;
                const bool aa = p.Samples(p.step) >= 2;
                float ms = 11.5f * (aa ? 0.55f + 0.45f * t : 1.0f);
                if (p.step >= kFirstSize) ms -= 3.0f * (1.0f - p.scale[SizeLevel(p.step)] * p.scale[SizeLevel(p.step)]);
                const float real = p.learnAdd[kAa2];
                if (g_break == 9) p.learnAdd[kAa2] = 0.0f;   // (the fault: what was measured is thrown away before each frame)
                if (p.Frame(ms + noise, kBudget, false) >= 0) ++moves;
                if (g_break == 9) p.learnAdd[kAa2] = real;
            }
            ++cases;
            if (moves > worst) { worst = moves; worstG = static_cast<int>(g * 10); worstT = static_cast<int>(t * 10); }
            if (moves <= 1) ++oneChange;
            (void)worstG; (void)worstT;
        }
        char d[128]; std::snprintf(d, sizeof d, "(most moves in 5 minutes over 16 guess/truth pairs: %d at guess %.1f truth %.1f; pairs with at most one change: %d of %d)", worst, worstG / 10.0, worstT / 10.0, oneChange, cases);
        Check(worst <= 3, "15a a mis-modelled cost of the anti-aliasing step (guess 1.1/1.3/1.9/2.5 against truth 1.1/1.3/1.9/2.5) makes at most 3 moves in 5 minutes", d);
        // the reviewer's own case: guess 1.3, truth 1.9, base 11.5 ms (it made 55 moves in 5 minutes)
        Policy p; p.readyMask = 0xF; p.SetAa(2, true); p.off[kFxSize] = p.off[kCrowd] = p.off[kShadowFar] = p.off[kShadowAll] = true; p.Start(0, 4);
        int moves = 0;
        for (int f = 0; f < 5 * 60 * 60; ++f) {
            const float real = p.learnAdd[kAa2];
            if (g_break == 9) p.learnAdd[kAa2] = 0.0f;
            const float ms = 11.5f * (p.Samples(p.step) >= 2 ? 0.55f + 0.45f * 1.9f : 1.0f);
            if (p.Frame(ms, kBudget, false) >= 0) ++moves;
            if (g_break == 9) p.learnAdd[kAa2] = real;
        }
        char e[128]; std::snprintf(e, sizeof e, "(%d moves, %d flaps, %d vetoes, measured cost %.0f %% of the mean; the unmended policy made 55 moves)", moves, p.flaps, p.vetoes, p.learnAdd[kAa2] * 100.0f);
        Check(moves <= 1 && p.step == kAa2, "15b the reviewer's case (2x really costs 1.9x, the policy guesses 1.3x, 11.5 ms base): one change, then it stays down", e);
        // 15c the doubling engages: a step shed again soon after its return waits longer next time (the window is wider than gap + calm + hot window)
        Policy q; q.readyMask = 0xF;
        if (g_break == 10) { q.probeFrames = 180; q.sizeProbeFrames = 1200; }
        Check(q.probeFrames > q.gap + q.calmFrames + q.hotFrames && q.sizeProbeFrames > q.gap + q.sizeCalmFrames + q.hotFrames,
              "15c the flap window is wider than the gap, the proof and the hot window together (it can be true)");
        // the flap counter itself: a step returned on a model that is wrong and shed again at once counts and blocks (no measured memory: the old way of finding out)
        Policy r; r.readyMask = 0xF; r.SetAa(2, true); r.off[kFxSize] = r.off[kCrowd] = r.off[kShadowFar] = r.off[kShadowAll] = true; r.learnMargin = 0.0f; r.Start(0);
        std::vector<int> upAt;
        for (int f = 0; f < 6 * 60 * 60; ++f) {
            const float ms = 11.5f * (r.Samples(r.step) >= 2 ? 0.55f + 0.45f * 1.9f : 1.0f);
            const int before = r.step;
            const int to = r.Frame(ms, kBudget, false);
            if (to >= 0 && to < before) upAt.push_back(f);
            r.learnAdd[kAa2] = 0.0f;   // (nothing learned: only the flap block can hold the step)
        }
        const int n = static_cast<int>(upAt.size());
        const bool grows = n >= 3 && (upAt[n - 1] - upAt[n - 2]) > (upAt[1] - upAt[0]) * 3 / 2;
        char f2[96]; std::snprintf(f2, sizeof f2, "(%d flaps, %d returns in 6 minutes, first interval %d, last %d frames)", r.flaps, n, n >= 2 ? upAt[1] - upAt[0] : 0, n >= 2 ? upAt[n - 1] - upAt[n - 2] : 0);
        Check(r.flaps >= 2 && grows, "15d with nothing remembered the flap block engages and each return waits longer than the one before", f2); }
    //    15e the memory plan at the real size (960x544): 2x and 4x against the free video memory, one answer per case; the numbers of the notes (ESTIMATES)
    { const int lw[3] = {864, 776, 704}, lh[3] = {488, 440, 400};
      const MemPlan a = PlanMemory(960, 544, lw, lh, 3, 4, 60000), b = PlanMemory(960, 544, lw, lh, 3, 4, 40000), c = PlanMemory(960, 544, lw, lh, 3, 4, 27000), d2 = PlanMemory(960, 544, lw, lh, 3, 4, 20000);
      if (g_break == 11) { /* the fault is in the gate: the plan is asked with no floor */ }
      const MemPlan a2 = g_break == 11 ? PlanMemory(960, 544, lw, lh, 3, 4, 20000, 0) : d2;
      char m[160]; std::snprintf(m, sizeof m, "(held %ld, ladder %ld, 2x %ld, 4x %ld KiB; total all %ld)", a.held, a.ladder, a.aa2, a.aa4, a.total);
      Check(a.held == 4080 && a.ladder >= 8100 && a.ladder <= 8200 && a.aa2 == 6120 && a.aa4 == 10200, "the reserved sizes at 960x544: held 4080, ladder about 8161, 2x 6120, 4x 10200 KiB", m);
      Check(a.aa2ok && a.aa4ok && a.ladderKept == 3, "with 60000 KiB free everything is made");
      Check(b.aa2ok && !b.aa4ok && b.ladderKept == 3, "with 40000 KiB free the 4x target is dropped first and the 2x stays");
      Check(c.ladderKept == 3 && !c.aa2ok && !c.aa4ok, "with 27000 KiB free the ladder fits and neither sample count does");
      Check(!a2.aa2ok && !a2.aa4ok && d2.ladderKept < 3, "with 20000 KiB free only part of the ladder is made and no sample count (the trim floor of 16 MiB stays)"); }


    // 16 the budget follows what the pacing can use (MSC_PICTURE_HOLD_PACE), and the ladder is decided at kick-off (MSC_PICTURE_HOLD_COMMIT). Console r7 (13 Nov): Vice, the game thread at 21-22 ms, GPU 15-19
    //    ms: 960x544 + 2x and the 704x400 floor gave the same 44-47 fps; The Dump held at 30 sat at the floor. (Estimates: the GPU figures are the console's, the policy's response is the host's.)
    auto fast = [](Sim& s) { s.p.calmFrames = 90; s.p.sizeCalmFrames = 150; s.p.upGap = 45; s.p.Start(0, 4); };
    //    16a a CPU-limited producer (a picture every 22 ms, GPU 19 ms): nothing is shed
    { Sim s; s.Aa(2); fast(s); Pacing pc; s.pace = &pc; s.period = 22.0f; s.base = 19.0f; s.pixPart = 8.0f; s.Run(5 * 60 * 60 / 2 * 2);
      char d[96]; std::snprintf(d, sizeof d, "(%d moves down, step %d, budget %.1f ms, GPU mean %.1f)", s.downs(), s.p.step, pc.Budget(), pc.gpu);
      Check(s.downs() == 0 && s.p.step == 0, "16a a CPU-limited producer (a picture every 22 ms, GPU 19 ms) keeps the full picture and its 2x: no step is shed", d); }
    //    16a' the same producer with the old 16.7 ms budget sheds (the console's finding)
    { Sim s; s.Aa(2); fast(s); s.base = 19.0f; s.Run(3600);
      Check(s.downs() >= 1, "(why) with a 16.7 ms budget the same load sheds", ""); }
    //    16b held at 30 (a picture every 33.3 ms): from the floor the picture returns to the full size within 40 seconds
    { Sim s; s.Aa(2); fast(s); Pacing pc; s.pace = &pc; s.period = 1001.0f / 30.0f; s.base = 30.0f; s.pixPart = 12.0f; s.p.Start(kSize3, 4); s.p.step = kSize3;
      s.Run(2400);
      char d[96]; std::snprintf(d, sizeof d, "(step %d, size level %d, %d moves up, %d down, budget %.1f ms)", s.p.step, SizeLevel(s.p.step), s.ups(), s.downs(), pc.Budget());
      Check(SizeLevel(s.p.step) == 0 && s.p.step <= kAa2 && s.downs() == 0, "16b held at 30 (33.3 ms pictures, GPU 30 ms at the full size): the picture is back at 960x544 in 40 s, with the samples when they fit", d); }
    //    16c GPU-limited at 60 (the pictures wait for the GPU): sheds, samples first, then sizes
    { Sim s; s.Aa(2); fast(s); Pacing pc; s.pace = &pc; s.period = 0.0f; s.base = 24.0f; s.pixPart = 11.0f; s.Run(3000);
      const bool aaFirst = !s.moves.empty() && s.moves[0].to >= kAa2;
      char d[96]; std::snprintf(d, sizeof d, "(first move to step %d, %d moves down, step %d)", s.moves.empty() ? -1 : s.moves[0].to, s.downs(), s.p.step);
      Check(s.downs() >= 1 && aaFirst && s.p.step >= kAa2, "16c GPU-limited at 60: the GPU is the stage that delays pictures, so it sheds, the samples before any size", d);
      char e[96]; std::snprintf(e, sizeof e, "(GPU %.1f ms at step %d)", s.Ms(), s.p.step);
      Check(s.Ms() < 16.7f * 0.93f, "16c and it sheds until the GPU fits the 60 budget (the pictures' own slowness is not mistaken for the pace)", e); }
    //    16c' fast recovery: a scene that was too heavy comes back in small steps in seconds once the load goes
    { Sim s; s.Aa(2); fast(s); Pacing pc; s.pace = &pc; s.base = 24.0f; s.pixPart = 11.0f; s.Run(1500); const int low = s.p.step; s.base = 13.0f; s.Run(1200);
      char d[96]; std::snprintf(d, sizeof d, "(step %d after the load, %d 20 s later, %d flaps)", low, s.p.step, s.p.flaps);
      Check(low >= kAa2 && s.p.step == 0, "16c' the load goes: everything is back within 20 seconds, no flaps", d); }
    //    16d the ladder is decided at kick-off from what is kept; room is made BEFORE the match; nothing changes in it
    { const unsigned allReady = 0xF, noSize3 = 0x7, hole = 0xB;   // bit L = size level L kept (bit 0 the held size)
      int a = LadderMaxStep(allReady, kSteps), b = LadderMaxStep(noSize3, kSteps), c = LadderMaxStep(hole, kSteps);
      if (g_break == 14) c = kShedSteps + 3;
      Check(a == kSize3 && b == kSize2 && c == kSize1, "16d the steps are a prefix of the sizes kept: sizes 1 and 2 give size 2, a missing size 2 ends the ladder at size 1 (no step into a target that is not there)");
      const long kib[5] = {3294, 2667, 2200, 6120, 10200};
      unsigned d1 = DropForRoom(21144, 12288, 0x1F, kib), d2 = DropForRoom(8000, 12288, 0x0F, kib), d3 = DropForRoom(-1, 12288, 0x1F, kib);
      if (g_break == 15) d2 = 0;
      char dd[96]; std::snprintf(dd, sizeof dd, "(21 MiB free: 0x%x; 8000 KiB free: 0x%x; unknown: 0x%x)", d1, d2, d3);
      Check(d1 == 0x10 || d1 == 0, "(21144 KiB free with the 12288 wanted: only 4x goes, and only if it does not fit)", dd);
      Check(d2 == 0x06 && d3 == 0, "16d with 8000 KiB free the smallest ladder sizes go first (704x400, then 776x440), 2x last; an unknown free figure drops nothing", dd);
      Sim s; s.Aa(2); s.p.maxStep = kSize1; s.base = 40.0f; s.Run(3000);
      Check(s.p.step <= kSize1, "and a ladder cut at kick-off is never exceeded in the match"); }


    // 17 the owner's decision (13 Nov): the picture SIZE goes first in small steps, each with 2x; the samples go last, at the bottom of the ladder (MSC_PICTURE_HOLD_AALAST)
    //    17a a heavy load: every move before the bottom keeps the samples, the one-sample step is the last
    { Sim s; s.Aa(2); s.Order(true); fast(s); s.p.SetOrder(g_break == 17 ? false : true, 3); s.p.Start(0, 4); s.base = 26.0f; s.pixPart = 12.0f; s.Run(4000);
      bool aaEarly = false, sizeFirst = !s.moves.empty() && s.moves[0].to >= 5 && s.moves[0].to <= 7;
      for (const Move& m : s.moves) if (m.to > m.from && s.p.Samples(m.to) == 1 && m.to < s.p.aa2) aaEarly = true;
      char d[96]; std::snprintf(d, sizeof d, "(first move to step %d, final step %d, %d samples, size level %d)", s.moves.empty() ? -1 : s.moves[0].to, s.p.step, s.p.Samples(s.p.step), s.p.Lvl(s.p.step));
      Check(!aaEarly && sizeFirst && s.p.Samples(s.p.firstSize) == 2 && s.p.step <= s.p.aa2, "17a a heavy load sheds picture size first, with the 2x kept on every size step", d); }
    //    17b a moderate load: only sizes, never the samples; the picture stays 2x at a smaller size
    { Sim s; s.Aa(2); s.Order(true); fast(s); s.p.SetOrder(g_break == 17 ? false : true, 3); s.p.Start(0, 4); s.base = 17.5f; s.pixPart = 9.0f; s.Run(3000);
      char d[96]; std::snprintf(d, sizeof d, "(step %d, %d samples, size level %d, %d moves)", s.p.step, s.p.Samples(s.p.step), s.p.Lvl(s.p.step), static_cast<int>(s.moves.size()));
      Check(s.p.Samples(s.p.step) == 2 && s.p.Lvl(s.p.step) >= 1 && s.p.Lvl(s.p.step) <= 2 && s.moves.size() <= 4, "17b a moderate load ends at a smaller size with 2x, in a few small steps, the samples untouched", d); }
    //    17c the load goes: everything comes back, the samples first of all (they went last), no flaps
    { Sim s; s.Aa(2); s.Order(true); fast(s); s.p.SetOrder(true, 3); s.p.Start(0, 4); s.base = 30.0f; s.pixPart = 13.0f; s.Run(2400); const int low = s.p.step; s.base = 11.0f; s.Run(1800);
      char d[96]; std::snprintf(d, sizeof d, "(step %d after the load, %d after 30 s, %d flaps)", low, s.p.step, s.p.flaps);
      Check(low >= 6 && s.p.step == 0 && s.p.flaps == 0, "17c the load goes: back to 960x544 with 2x within 30 s, no flaps", d); }
    //    17d the memory side: the new order's drop order (4x, the bottom one-sample target, the sizes from the smallest, 2x last) and its prefix
    { const long kib[6] = {5558, 4941, 4275, 6120, 10200, 2850};
      unsigned d1 = DropForRoom(1000, 12288, 0x2F, kib, g_break == 16 ? false : true), d2 = DropForRoom(8000, 12288, 0x3F, kib, true);
      Check(d1 == (0x20u | 0x4u | 0x2u) && d2 == 0x10u, "17d room is made before kick-off: 4x first, then the bottom one-sample target and the smallest sizes, never the held 2x until last");
      int n1 = LadderSizes(0xB), n2 = LadderSizes(0xF), n3 = LadderSizes(0x1);
      if (g_break == 18) n1 = 2;
      Check(n1 == 1 && n2 == 3 && n3 == 0, "and the ladder of the match is a prefix of the sizes kept (a missing size 2 ends it at size 1)");
      Policy p; p.SetAa(2, true); p.SetOrder(true, 2);
      Check(p.firstSize == 5 && p.aa2 == 7 && p.maxStep == 7 && p.Lvl(7) == 2 && p.Samples(6) == 2 && p.Samples(7) == 1, "a ladder of two sizes puts the one-sample step at 7, at the bottom size"); }

    // 18 hold10 (the r10 console run): a safe start, a margin at a held 30, sizes below the sample step, scenes at the live level
    //    18a the ladder has five levels: three with 2x, the sample step, two one-sample sizes below it
    { Policy p; p.SetAa(2, true); p.SetOrder(true, 5, 3);
      const bool lay = p.firstSize == 5 && p.aa2 == 8 && p.maxStep == 10 && p.Lvl(7) == 3 && p.Lvl(8) == 3 && p.Lvl(9) == 4 && p.Lvl(10) == 5 && p.Samples(7) == 2 && p.Samples(8) == 1 && p.Samples(9) == 1 && p.Samples(10) == 1
                      && p.IsSize(9) && p.IsSize(10) && !p.IsSize(8) && p.IsAa(8);
      Check(lay, "18a five levels: 2x at 912x520, 864x488, 800x456, then one sample there, then 752x424 and 704x400 with one sample");
      Sim s; s.Aa(2); s.p.SetOrder(true, 5, 3); s.p.readyMask = 0x3F; fast(s); s.p.SetOrder(true, 5, 3); s.p.Start(0, 4); s.base = 46.0f; s.pixPart = 30.0f; s.Run(9000);
      char d[96]; std::snprintf(d, sizeof d, "(final step %d, level %d, ms %.1f)", s.p.step, s.p.Lvl(s.p.step), s.Ms());
      Check(g_break == 19 ? s.p.step <= 8 : s.p.step >= 9 && s.p.step <= 10 && s.p.Samples(s.p.step) == 1, "18a' a load the 800x456 step cannot carry goes below the sample step to 752x424 or 704x400, in order", d); }
    //    18b the safe start: what the last match here learned; else the stadium's class from the table (the worst stadiums start low, Vice class at full size)
    { const int five = StartLevel(-1, StadiumClass(1), 10, 5, 8, 5), three = StartLevel(-1, StadiumClass(4), 8, 5, 8, 3), vice = StartLevel(-1, StadiumClass(0), 10, 5, 8, 5), learn = StartLevel(6, StadiumClass(1), 10, 5, 8, 5);
      char d[96]; std::snprintf(d, sizeof d, "(The Dump %d, a three-size ladder %d, Vice %d, learned 6 -> %d)", five, three, vice, learn);
      Check(g_break == 20 ? false : (five == 9 && three == 8 && vice == 0 && learn == 6), "18b a first match at a heavy stadium starts at 752x424, one size short of that on a short ladder, Vice class at full size, a learned level wins", d); }
    //    18c the start is excused from proof: no move up in the first seconds, but a move down is still allowed; then the climb is quick
    { Sim s; s.Aa(2); s.p.SetOrder(true, 5, 3); s.p.readyMask = 0x3F; fast(s); s.p.SetOrder(true, 5, 3); s.p.Start(9, 4, g_break == 21 ? 0 : 480); s.base = 12.0f; s.pixPart = 5.0f; s.Run(400);
      const int at400 = s.p.step; s.Run(1500);
      char d[96]; std::snprintf(d, sizeof d, "(step %d at 400 frames, %d after 31 s, %d flaps)", at400, s.p.step, s.p.flaps);
      Check(at400 == 9 && s.p.step <= 6 && s.p.flaps == 0, "18c a start at step 9 stays there for the excused 480 frames, then climbs within seconds, no flaps", d); }
    //    18d a held 30: the budget is a share of the period (26.6 ms), the free run's is the period; the hold sheds until the p95 fits
    { Pacing a, b; a.margin = g_break == 22 ? 1.0f : 0.80f; b.margin = 0.80f;
      for (int i = 0; i < 200; ++i) { a.Add(33.3f, 27.0f); b.Add(16.7f, 12.0f); }
      const float ba = a.Budget(), bb = b.Budget();
      char d[96]; std::snprintf(d, sizeof d, "(held 30: %.1f ms, free run: %.1f ms)", ba, bb);
      Check(ba > 26.0f && ba < 27.2f && bb < 17.0f, "18d at a held 30 the budget is 80 per cent of the period, never the period itself; at 60 it is 16.7", d);
      Sim s; s.Aa(2); s.p.SetOrder(true, 5, 3); s.p.readyMask = 0x3F; fast(s); s.p.SetOrder(true, 5, 3); Pacing pc; pc.margin = g_break == 22 ? 1.0f : 0.80f; s.pace = &pc; s.period = 1001.0f / 30.0f; s.base = 31.0f; s.pixPart = 18.0f; s.p.Start(0, 4); s.Run(5000);
      char e[96]; std::snprintf(e, sizeof e, "(step %d, GPU %.1f ms)", s.p.step, s.Ms());
      Check(s.Ms() < 0.92f * 26.7f + 0.5f, "and a GPU time of 31 ms at a held 30 is brought under 24.5 ms, so the pictures make their flip", e); }
    //    18e the drop order has room for the two low sizes: they go first of all
    { const long kib[8] = {5558, 4941, 4275, 6120, 10200, 2850, 2495, 2200};
      unsigned d = DropForRoom(10000, 12288, 0xDF, kib, true);   // all but 4x
      Check(g_break == 23 ? false : (d & 0xC0u) == 0xC0u && !(d & 0x1Fu), "18e short of room the sizes below the sample step go first, before the bottom target, the 2x sizes and the held 2x", d ? nullptr : "none"); }

    // 19 hold11: a pair of pictures (bg30): the hold judges the SUM of the full and the light picture against 33.3 ms x margin, one sample a pair
    { PairJudge pj; float sum = 0.0f;
      const bool a1 = pj.Add(1, 14.0f, &sum), a2 = pj.Add(2, 9.0f, &sum);
      const bool b2 = pj.Add(2, 9.0f, &sum);   // a light picture whose full one never came (a cut between them): nothing
      Check(!a1 && a2 && sum > 22.9f && sum < 23.1f && !b2, "19a a pair is judged once, as the sum of its two pictures; a light picture alone says nothing");
      const float bud = PairJudge::Budget(g_break == 24 ? 1.0f : 0.80f);
      Check(bud > 26.5f && bud < 26.8f, "19b the pair's budget is 80 per cent of 33.3 ms, not the 16.7 ms of one picture"); }
    //    19c pair GPU 31 ms (full 20 + light 11) at Vice-like cost: the ladder sheds the background until the pair fits 26.6 ms, the policy sees the sum, never one picture
    { Sim s; s.Aa(2); s.p.SetOrder(true, 5, 3); s.p.readyMask = 0x3F; fast(s); s.p.SetOrder(true, 5, 3); s.p.Start(0, 4); s.base = 31.0f; s.pixPart = 18.0f;
      for (int i = 0; i < 6000; ++i) s.One(s.Ms() + s.Noise());   // (One() uses the budget given: here the pair's)
      s.p.Start(0, 4);
      const float pb = PairJudge::Budget(0.80f);
      for (int i = 0; i < 6000; ++i) { const float ms = s.Ms() + s.Noise(); const int from = s.p.step; const int to = s.p.Frame(g_break == 25 ? ms * 0.5f : ms, pb, false); if (to >= 0) s.moves.push_back({i, from, to}); }
      char d[96]; std::snprintf(d, sizeof d, "(step %d, pair %.1f ms, budget %.1f)", s.p.step, s.Ms(), pb);
      Check(s.Ms() < 0.95f * pb + 1.0f && s.p.step > 0, "19c a pair of 31 ms is brought under its budget by stepping the background down, not left at 33.3", d); }

    // 20 hold12: held scenes (cut-scene, replay) have their own policy: start at a level known to fit, shed in a few pictures, climb on a short proof
    { const int c0 = SceneStartStep(0, 8, 5, 5), c2 = SceneStartStep(2, 8, 5, 5), c3 = SceneStartStep(0, 8, 3, 5);
      char d[96]; std::snprintf(d, sizeof d, "(table: %d, heavy %d, short ladder %d)", c0, c2, c3);
      Check(c0 == 9 && c2 == 10 && c3 == 8, "20a a scene with nothing learned starts one sample below the sample step (752x424), the heavy stadiums on 704x400, a short ladder at the sample step", d); }
    //    20b a Vice goal cut-scene (52 ms at the full picture with 2x): from the table's start it fits at once, from the full picture it is shed within 8 pictures
    { auto mk = [](Sim& s) { s.Aa(2); s.p.SetOrder(true, 5, 3); s.p.readyMask = 0x3F; s.p.SetOrder(true, 5, 3); if (g_break != 26) s.p.SceneTiming(); s.p.off[1] = s.p.off[2] = s.p.off[3] = s.p.off[4] = true; };
      const float bud = 1001.0f / 60.0f * 2.0f * 0.85f;
      Sim a; mk(a); a.base = 46.0f; a.pixPart = 30.0f; a.p.Start(g_break == 27 ? 0 : 9, 4); a.p.frozen = false;
      int first = -1; for (int i = 0; i < 120; ++i) { const int to = a.p.Frame(a.Ms(), bud, false); if (to >= 0 && first < 0) first = i; }
      char d[96]; std::snprintf(d, sizeof d, "(start step 9 costs %.1f ms of %.1f, first move at picture %d, final step %d)", a.Ms(), bud, first, a.p.step);
      Check(a.Ms() < bud && first < 0, "20b from the table's start a Vice cut-scene fits its 28.3 ms with no move at all", d);
      Sim b; mk(b); b.base = 52.0f; b.pixPart = 34.0f; b.p.Start(0, 4); b.p.frozen = false;
      int f2 = -1; for (int i = 0; i < 40; ++i) { const int to = b.p.Frame(b.Ms(), bud, false); if (to >= 0 && f2 < 0) f2 = i; }
      char e[96]; std::snprintf(e, sizeof e, "(first move at picture %d, step %d after 40, %.1f ms)", f2, b.p.step, b.Ms());
      Check(f2 >= 0 && f2 <= 8 && b.p.step >= 6, "20c from the full picture a scene sheds within 8 pictures, not 30", e); }
    //    20d the scene's load goes (a replay that is light): it climbs on its short proof, inside the scene, and does not flap
    { Sim s; s.Aa(2); s.p.SetOrder(true, 5, 3); s.p.readyMask = 0x3F; s.p.SetOrder(true, 5, 3); s.p.SceneTiming(); if (g_break == 28) { s.p.calmFrames = 900; s.p.sizeCalmFrames = 900; }
      s.p.off[1] = s.p.off[2] = s.p.off[3] = s.p.off[4] = true; s.base = 17.0f; s.pixPart = 8.0f; s.p.Start(9, 4); s.p.frozen = false;
      const float bud = 1001.0f / 60.0f * 2.0f * 0.85f;
      for (int i = 0; i < 240; ++i) s.p.Frame(s.Ms(), bud, false);
      char d[96]; std::snprintf(d, sizeof d, "(step %d after 240 pictures (4 s), %d flaps)", s.p.step, s.p.flaps);
      Check(s.p.step <= 6 && s.p.flaps == 0, "20d a light scene climbs back towards the full picture within its 4 s, no flaps", d); }

    // 30 heldstep23: held scenes of ONE sample enter a size level only if its one-sample target is kept (r17 goal 2: 912x520 1x made inside the cut, +11 targets)
    {   Policy P; P.SetOrder(true, 5, 3); P.SetAa(2, true);
        const int lvl2 = 1;                       // the live ladder's level 1 has a kept 2x target, no 1x one
        const unsigned live = 0x3u, one = 0x1u;   // live ready: levels 0 and 1; one-sample kept: level 0 only
        const unsigned m = g_break == 30 ? HeldSceneMask(false, live, one) : HeldSceneMask(true, live, one);
        int st = P.firstSize + lvl2 - 1; while (st < P.maxStep && P.Lvl(st) < lvl2) ++st;
        const int got = HeldReadyStep(P, st, m, 0);
        char d[96]; std::snprintf(d, sizeof d, "(wanted step %d level %d, got step %d)", st, P.Lvl(st), got);
        Check(P.Lvl(st) == lvl2 && got == 0, "30a a one-sample held scene does not take a level whose one-sample target is not kept; it uses level 0", d);
        const unsigned m2 = g_break == 30 ? HeldSceneMask(false, live, 0x3u) : HeldSceneMask(true, live, 0x3u);
        Check(HeldReadyStep(P, st, m2, 0) == st, "30b ... and takes it once the one-sample target is kept");
        Check(HeldSceneMask(false, live, one) == live, "30c held scenes with the live samples keep the live ready mask");
        Check((HeldSceneMask(true, 0x2u, 0x0u) & 1u) == 0, "30d the mask never invents the held size's bit"); }

    // 40 MSC_PICTURE_HOLD_GPUBUSY (the console's s27d1b: "GPU mean 26.9 ms of 26.5" stepped 960x544 -> 912x520 -> 864x488 while [lane] gpu read 16.4 ms): a step down only when the GPU's BUSY time is the
    // limit of the pictures. The pictures come every 33.5 ms (a held 30, the game thread sets the pace); the hold's budget follows (26.5 ms at margin 0.80).
    {
        struct Run { Policy p; Pacing pc; int frame = 0; int downs = 0, ups = 0; std::vector<int> stepAt;
            Run(bool gate_) : gate(gate_) {
                p.readyMask = 0xF; p.off[kFxSize] = p.off[kCrowd] = p.off[kShadowFar] = p.off[kShadowAll] = true; p.SetAa(2, true); p.SetOrder(true, 2, 2); p.aaReady = 7;
                p.calmFrames = 90; p.sizeCalmFrames = 150; p.upGap = 45;   // (the pacing's fast recovery, as the backend sets it)
                pc.margin = 0.80f;
                if (g_break == 41) pc.gpuAt = 0.40f;
            }
            bool gate;
            void One(float ms, float period) {
                pc.Add(period, ms);
                const float budget = pc.Budget();
                bool gpuLimit = true;
                if (gate && g_break != 40) gpuLimit = pc.GpuGate(budget, 1001.0f / 60.0f);
                const int before = p.step;
                const int to = p.Frame(ms, budget, false, gpuLimit);
                if (to >= 0) { if (to > before) ++downs; else ++ups; }
                ++frame;
                stepAt.push_back(p.step);
            }
        };
        // a: CPU-bound lateness: the GPU takes 19 ms most of the time (57 % of the 33.5 ms) with bursts of 27 ms for 60 pictures every 300 (the backlog the console showed): no step with the gate
        auto cpuBound = [&](bool gate) { Run r(gate); for (int i = 0; i < 6000; ++i) { const bool burst = (i % 300) >= 240; r.One(burst ? 27.0f : 19.0f, 33.5f); } return r; };
        Run a = cpuBound(true), a0 = cpuBound(false);
        char d[160]; std::snprintf(d, sizeof d, "(with the gate %d down, %d up; without it %d down; budget %.1f ms, slow GPU mean %.1f ms, late share %.1f %%)", a.downs, a.ups, a0.downs, a.pc.Budget(), a.pc.slow, a.pc.lateShare * 100.0f);
        Check(a.downs == 0 && a.p.step == 0, "40a CPU-bound lateness (GPU busy 19-27 ms of a 33.5 ms period) never steps the picture down with the gate", d);
        Check(a0.downs >= 1, "40a' (control) the same trace without the gate does step down: the gate is what keeps the picture", d);
        // b: the GPU IS the limit (busy 31 ms of 33.5, pictures late): the step comes
        { Run r(true); for (int i = 0; i < 3000; ++i) r.One(31.0f, 36.0f);
          std::snprintf(d, sizeof d, "(%d down, step %d, slow mean %.1f ms of %.1f)", r.downs, r.p.step, r.pc.slow, r.pc.Cadence());
          Check(r.downs >= 1, "40b a GPU that is busy 31 ms of every 33.5 ms period (the limit) still steps down with the gate", d); }
        // c: a GPU at 24 ms (72 %) with 10 % late pictures counts (the second rule); 24 ms with no late pictures does not
        { Run r(true); for (int i = 0; i < 3000; ++i) r.One(i % 10 == 0 ? 36.0f : 25.5f, i % 10 == 0 ? 50.0f : 33.5f);
          std::snprintf(d, sizeof d, "(%d down, late share %.1f %%, slow %.1f ms)", r.downs, r.pc.lateShare * 100.0f, r.pc.slow);
          Check(r.pc.GpuLimit() && r.pc.lateShare >= 0.08f, "40c busy 25 ms of 33.5 with 10 % of the pictures late is read as GPU-limited", d);
          Run q(true); for (int i = 0; i < 3000; ++i) q.One(25.5f, 33.5f);
          Check(!q.pc.GpuLimit(), "40c' ... the same 25 ms with every picture on time is not"); }
        // d: recovery: the GPU limit goes away (busy 31 -> 14 ms): every step is back within 25 s of pictures (750 at 30)
        { Run r(true); for (int i = 0; i < 2000; ++i) r.One(31.0f, 36.0f);
          const int low = r.p.step; int back = -1;
          for (int i = 0; i < 2400; ++i) { r.One(g_break == 42 ? 31.0f : 14.0f, 33.5f); if (r.p.step == 0 && back < 0) back = i; }
          std::snprintf(d, sizeof d, "(down to step %d, back at step 0 after %d pictures = %.1f s at 30)", low, back, back * 0.0335f);
          Check(low > 0 && back >= 0 && back <= 750, "40d recovery: the picture is back at the full size within 25 s of the GPU limit going away", d); }
        // e: no flapping: a load that sits on the limit (busy 28-30 ms, period 33.5) never makes more than 2 moves a minute, and no step is taken again within 5 s of being given back
        { Run r(true); unsigned seed = 7; for (int i = 0; i < 9000; ++i) { seed = seed * 1664525u + 1013904223u; const float ms = 28.0f + static_cast<float>((seed >> 8) & 0xFF) / 255.0f * 3.0f; r.One(ms, 34.0f); }
          int minutes = 9000 / 1800; const int moves = r.downs + r.ups;
          std::snprintf(d, sizeof d, "(%d moves in %d min at 30)", moves, minutes);
          Check(moves <= 2 * minutes + 2, "40e a load on the limit settles: at most 2 moves a minute", d); }
        // f: nothing known yet (a match just began: fewer than 60 pictures measured): the old behaviour
        { Pacing q; q.margin = 0.80f; for (int i = 0; i < 30; ++i) q.Add(33.5f, 10.0f); Check(q.GpuLimit(), "40f before the pacing is known the gate lets the old rule act (GpuLimit true)"); }
    }

    // 12 parsing and sizes
    { int w = 0, h = 0;
      Check(ParseSize("full", &w, &h) && w == 960 && h == 544, "full is the screen's size");
      Check(ParseSize("1", &w, &h) && w == 640 && h == 448, "1 is the game's own size");
      Check(ParseSize("native", &w, &h) && w == 640 && h == 448, "native is the game's own size");
      Check(ParseSize("800x560", &w, &h) && w == 800 && h == 560, "WxH is taken");
      Check(!ParseSize("0", &w, &h) && !ParseSize("", &w, &h) && !ParseSize(nullptr, &w, &h) && !ParseSize("garbage", &w, &h) && !ParseSize("10x10", &w, &h) && !ParseSize("800x", &w, &h), "0, empty, garbage and tiny sizes are no size");
      Check(StepDim(960, 0.90f) == 864 && StepDim(544, 0.90f) == 488 && StepDim(960, 0.81f) == 776 && StepDim(544, 0.81f) == 440 && StepDim(960, 0.73f) == 704 && StepDim(544, 0.73f) == 400 && StepDim(960, 1.0f) == 960,
            "the steps' sizes are multiples of 8: 864x488, 776x440, 704x400");
      Check(StepDim(20, 0.1f) == 16, "a size never goes below 16");
      // video memory of the three targets (colour + depth, 4 bytes a pixel each), KiB
      long kib = 0; const int sw[3] = {864, 776, 704}, sh[3] = {488, 440, 400}; for (int i = 0; i < 3; ++i) kib += static_cast<long>(sw[i]) * sh[i] * 8 / 1024;
      char d[64]; std::snprintf(d, sizeof d, "(%ld KiB = %.1f MiB)", kib, kib / 1024.0);
      Check(kib > 8000 && kib < 8300, "the three targets made ahead take about 8 MiB of video memory", d); }

    // 50 pace13 MSC_GLOW_HALF_PLAY=1: the glow chain at half rate is ladder step 1, before any size, and is kept only if it bought GPU time
    {   // 50a a GPU one frame over the budget (17.4 ms against 16.7), the glow step really buys 2 ms: it is the only step taken, no size, and it stays
        Sim s; s.p.glowStep = true; s.p.off[kFxSize] = false; s.p.save[kFxSize] = 0.8f; s.base = 16.5f; s.glowGain = 2.0f; s.p.fit = 0.93f; s.Run(2400);
        char d[96]; std::snprintf(d, sizeof d, "(final step %d, moves %lu, ms %.1f, noGain %d)", s.p.step, static_cast<unsigned long>(s.moves.size()), s.Ms(), s.p.glowNoGain ? 1 : 0);
        Check(s.p.step == kFxSize && !s.p.glowNoGain && s.moves.size() == 1 && s.moves[0].to == kFxSize && s.Ms() < 0.92f * kBudget, "50a the glow step is the first step taken and, bought 2 ms, it stays with no size step", d);
    }
    {   // 50b a stadium where the glow chain is not drawn (the step buys nothing): judged and dropped as an empty step, the policy goes on down the ladder (shadows, sizes) and the glow step is not counted again
        Sim s; s.p.glowStep = true; s.p.off[kFxSize] = false; s.p.save[kFxSize] = 0.8f; s.base = 19.0f; s.glowGain = 0.0f; s.Run(3600);
        char d[96]; std::snprintf(d, sizeof d, "(final step %d, noGain %d, ms %.1f)", s.p.step, s.p.glowNoGain ? 1 : 0, s.Ms());
        Check(s.p.glowNoGain && s.p.Empty(kFxSize) && s.p.step > kFxSize, "50b a glow step that bought nothing is empty for the rest of the match and the ladder goes on", d);
    }
    {   // 50c a gain under 3 % of the mean (0.2 ms of 17.4): not worth it either
        Sim s; s.p.glowStep = true; s.p.off[kFxSize] = false; s.p.save[kFxSize] = 0.8f; s.base = 17.4f; s.glowGain = 0.2f; s.p.fit = 0.93f; s.Run(3600);
        char d[96]; std::snprintf(d, sizeof d, "(final step %d, noGain %d)", s.p.step, s.p.glowNoGain ? 1 : 0);
        Check(s.p.glowNoGain, "50c a gain under 3 % of the mean is judged no gain", d);
    }
    {   // 50d without the mode the step is the old opt-in one: off, never taken
        Sim s; s.base = 17.4f; s.Run(2400);
        Check(s.p.off[kFxSize] && s.p.step != kFxSize && !s.p.glowStep, "50d without MSC_GLOW_HALF_PLAY step 1 stays off as before");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    if (g_break) { std::printf("(--break=%d: this run is expected to FAIL)\n", g_break); }
    return g_fail ? 1 : 0;
}
