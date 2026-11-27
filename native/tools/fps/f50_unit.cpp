// f50_unit.cpp (wave 24, f50, 24 Nov 2026): the strict 50 tier (MSC_LOCK60_USE50): 5 pictures in 6 refreshes, 1-1-1-1-2 (np_fps45.h Pattern45(5, 6)), generalised from the 45's 3 in 4. The REAL np_fps45.h and
// np_fps_ctl.h / np_fps_tier.h on virtual vblanks; the pipeline-level checks of the pattern (clean, console pipeline, hitches, a stall, the lead, the fallback) are f45_unit.cpp built with
// -DNP_UNIT_N=5 -DNP_UNIT_M=6 (run_pace_tests.sh builds both). This file: the pattern itself (and that the 3 / 4 is the old 45 formula for formula), the gate, the clock's display-time game time, the tier
// choice (USE50 / USE45 / MSC_FPS=45), the ladder 60 -> 50 -> 30 and back (the controller's relief, Fallback45's fall, the trial and the proof), and the seams between the tiers.
//   c++ -std=c++11 -O1 -Inative/platform/compat native/tools/fps/f50_unit.cpp native/tools/fps/gsplit_stub.cpp -o f50_unit && ./f50_unit
// Mutants: -DNP_F45_BREAK=11 (IsSlot is the 45's), 12 (Hold is the 45's), 13 (the entry phase is s & 3); -DNP_F50_BREAK=1 (MSC_FPS=45 follows the 50 flag), 2 (the 50 never falls to 30: its Fallback45 is
// not fed the 50's late pictures), 3 (the seam: a tier entry starts with a 2-refresh first hold at 60 -> 50). Each must FAIL this file.
#include "np_fps_ctl.h"
#include "np_fps_tier.h"
#include "np_fps45.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static const double R = 1001.0 / 60000.0, Rms = R * 1000.0;

// the 45's pattern as it was before f50 (np_fps45.h of integ/w14), formula for formula: the reference the 3 / 4 has to equal
struct Old45 {
    static unsigned Pos(uint32_t v, unsigned phase) { return phase >= 4u ? 0u : (v - phase) & 3u; }
    static bool IsSlot(uint32_t v, unsigned phase) { return Pos(v, phase) != 3u; }
    static uint32_t AtOrAfter(uint32_t v, unsigned phase) { return IsSlot(v, phase) ? v : v + 1u; }
    static uint32_t After(uint32_t v, unsigned phase) { return AtOrAfter(v + 1u, phase); }
    static unsigned Hold(uint32_t slot, unsigned phase) { return Pos(slot, phase) == 2u ? 2u : 1u; }
};

int main() {
    const Pattern45 p50(5, 6), p45(3, 4), pdef;
    // ---- 1. the pattern
    {
        bool ok = true;
        for (unsigned ph = 0; ph < 6; ++ph) {
            unsigned slots = 0, holdSum = 0;
            for (uint32_t v = 1000; v < 1006; ++v) if (p50.IsSlot(v, ph)) { ++slots; holdSum += p50.Hold(v, ph); }
            ok = ok && slots == 5 && holdSum == 6;
            // from a slot, the holds go 1, 1, 1, 1, 2 in this order
            uint32_t s = 1000; while (p50.Pos(s, ph) != 0) ++s;
            const unsigned want[5] = {1, 1, 1, 1, 2};
            for (int i = 0; i < 10; ++i) { ok = ok && p50.IsSlot(s, ph) && p50.Hold(s, ph) == want[i % 5] && p50.After(s, ph) == s + want[i % 5]; s = p50.After(s, ph); }
        }
        check(ok, "50: 5 slots in every 6 refreshes, holds 1-1-1-1-2 in this order from position 0, the next slot is the hold away (6 phases)");
        check(std::fabs(p50.Fps() - 49.95) < 0.001 && std::fabs(p45.Fps() - 44.955) < 0.001 && std::fabs(p50.PeriodRefreshes() - 1.2) < 1e-12, "the rates: 49.95 and 44.96 a second, 1.2 and 1.33 refreshes a picture");
        bool phok = true;
        for (uint32_t v = 3000; v < 3040; ++v) phok = phok && p50.Pos(v, p50.PhaseAt(v)) == 0u;
        check(phok, "50: a stretch that starts at vblank s takes the phase s mod 6 (its first picture is position 0)");
        bool same = true;
        for (unsigned ph = 0; ph < 4; ++ph) for (uint32_t v = 4000; v < 4400; ++v)
            same = same && p45.IsSlot(v, ph) == Old45::IsSlot(v, ph) && pdef.IsSlot(v, ph) == Old45::IsSlot(v, ph) && p45.After(v, ph) == Old45::After(v, ph) && p45.AtOrAfter(v, ph) == Old45::AtOrAfter(v, ph)
                 && p45.Hold(v, ph) == Old45::Hold(v, ph) && pdef.Hold(v, ph) == Old45::Hold(v, ph) && p45.PhaseAt(v) == (v & 3u);
        check(same, "45 UNCHANGED: Pattern45(3, 4) and the default equal the 45 of before f50, formula for formula (slot, next slot, hold, entry phase; the four phases and 400 vblanks; the every-vblank phase is the next check)");
        bool ev = true;
        for (uint32_t v = 5000; v < 5100; ++v) ev = ev && p50.IsSlot(v, Pattern45::kEvery) && p50.Hold(v, Pattern45::kEvery) == 1 && p50.After(v, Pattern45::kEvery) == v + 1 && p45.After(v, Pattern45::kEvery) == v + 1;
        check(ev, "the 60 tier's 'every vblank' phase is a slot at every vblank, held 1, for either pattern");
    }
    // ---- 2. the gate: pictures ready in time go up one slot after the other: 1-1-1-1-2; a late one costs its slot, the phase is kept; a stall starts the pattern again at that picture
    {
        Gate45 g(Gate45Config(), false, p50);
        std::vector<uint32_t> sh; uint32_t t = 7001;   // ready one vblank after the one before: always in time
        for (int i = 0; i < 60; ++i) { const uint32_t due = g.Due(t - 1u < 7001u ? 7001u : sh.empty() ? t : sh.back() + 1u, false, 0, false, 0); sh.push_back(due); g.Shown(due); t = due + 1u; }
        bool ok = g.offPattern_ == 0 && g.slips_ == 0 && g.resyncs_ == 1;
        const unsigned want[5] = {1, 1, 1, 1, 2};
        for (size_t i = 1; i < sh.size(); ++i) ok = ok && sh[i] - sh[i - 1] == want[(i - 1) % 5];
        check(ok, "50 gate: pictures ready in time go up 1, 1, 1, 1, 2 refreshes apart from the first (position 0), no slip, no picture off the pattern");
        Gate45 g2(Gate45Config(), false, p50); uint32_t last = 0; bool o2 = true; unsigned late = 0;
        for (int i = 0; i < 200; ++i) {
            const uint32_t earliest = i == 0 ? 9000u : (i % 37 == 20 ? last + 3u : last + 1u);   // every 37th picture comes ready two refreshes late
            const uint32_t due = g2.Due(earliest, false, 0, false, 0);
            o2 = o2 && g2.Pat().IsSlot(due, g2.Phase()) && due >= earliest; late += g2.LastLate(); g2.Shown(due); last = due;
        }
        check(o2 && g2.offPattern_ == 0 && g2.resyncs_ == 1 && late >= 4 && g2.holds_[3] <= g2.slips_, "50 gate: a late picture takes the first slot it can make, never a vblank off the pattern; the phase is kept (taken once)");
        Gate45 g3(Gate45Config(), false, p50); uint32_t d = g3.Due(9100, false, 0, false, 0); g3.Shown(d); d = g3.Due(9101, false, 0, false, 0); g3.Shown(d);
        const unsigned ph0 = g3.Phase(); const uint32_t d3 = g3.Due(9101 + 40, false, 0, false, 0);   // a long stall
        check(d3 == 9141 && g3.stalls_ == 1 && g3.Phase() == p50.PhaseAt(9141) && ph0 == p50.PhaseAt(9100), "50 gate: a stall of over 8 refreshes starts the pattern again at that picture (it goes up at once; phase = its vblank mod 6)");
    }
    {   // the entry on the game's slot (f50, enterOnTarget): a first picture whose game time stands for a slot a little after its earliest vblank goes up on that slot, and the phase is taken from it
        Gate45Config gc; gc.enterOnTarget = true;
        Gate45 ge(gc, false, p50), gn(Gate45Config(), false, p50);
        const uint32_t de = ge.Due(9000, true, 9002, false, 0), dn = gn.Due(9000, true, 9002, false, 0);
        check(de == 9002 && ge.Phase() == p50.PhaseAt(9002) && dn == 9000 && gn.Phase() == p50.PhaseAt(9000), "50 gate: with enterOnTarget the first picture goes up on its game slot (2 refreshes after its earliest) and takes the phase there; without it the entry is as before (the 45's)");
    }
    // ---- 3. the clock, closed loop with the gate and a pipeline that is always in time (ready L = 3 refreshes after the start): the game advances by exactly the refreshes between two shown pictures
    {
        Gate45 g(Gate45Config(), false, p50); Clock45 c(p50);
        const int L = 3; std::vector<uint32_t> shown(400, 0); std::vector<uint32_t> stamp(400, 0); std::vector<int> adv(400, -1);
        uint32_t vc = 10000; int fb = 0; bool bad = false; unsigned advSeq[5] = {0, 0, 0, 0, 0}; (void)advSeq;
        double T = 0; std::vector<double> game(400, 0);
        for (int k = 1; k < 300; ++k) {
            // start the frame: the gate (lead 5 before the slot it is planned for)
            uint32_t slot = 0;
            if (k > 2 && c.NextSlot(L, fb > 0, (uint64_t)fb, fb > 0 ? shown[fb] : 0u, fb > 0 ? (int)g.Phase() : -1, &slot)) { if ((int32_t)(slot - 5u - vc) > 0) vc = slot - 5u; }
            stamp[k] = vc;
            adv[k] = c.Begin(true, vc, L, fb > 0, (uint64_t)fb, fb > 0 ? shown[fb] : 0u, fb > 0 ? (int)g.Phase() : -1);
            uint32_t gs = 0; const bool ht = c.NewestTarget(&gs);
            c.Handed((uint64_t)k);
            const uint32_t earliest = vc + (uint32_t)L;
            const uint32_t due = g.Due(earliest, ht, gs, false, 0);
            shown[k] = due; g.Shown(due);
            // the reports the game thread has seen by its next start: pictures whose slot is before the next start
            vc += 1;
            while (fb + 1 <= k && shown[fb + 1] <= vc) ++fb;
            game[k] = (adv[k] >= 0 ? (k > 1 ? game[k - 1] + adv[k] : 0) : (k > 1 ? game[k - 1] + (stamp[k] - stamp[k - 1]) : 0));
        }
        (void)T; (void)bad;
        unsigned errs = 0, n = 0, preds = 0;
        for (int k = 40; k < 299; ++k) { ++n; if (adv[k] >= 0) ++preds; if (std::fabs((game[k] - game[k - 1]) - (double)(shown[k] - shown[k - 1])) > 1e-9) ++errs; }
        check(errs == 0 && preds == n, "50 clock: every picture's game time is its display time (the game advances by the refreshes between shown pictures: 1, 1, 1, 1, 2), every frame predicted");
    }
    // ---- 4. which pattern: USE50, USE45, MSC_FPS=45
    {
        check(MidPattern(false, true).n == 5 && MidPattern(false, true).m == 6, "MSC_LOCK60_USE50=1: the mid tier is the 50 (5 in 6)");
        check(MidPattern(false, false).n == 3 && MidPattern(false, false).m == 4, "USE50=0 (USE45=1 or nothing): the mid tier is the 45 (3 in 4), exactly as before");
        check(MidPattern(true, false).n == 3 && MidPattern(true, true).n == 3, "MSC_FPS=45 is the 45 whatever the 50 flag says");
        check(!MidAllowed(false, false, false) && MidAllowed(false, true, false) && MidAllowed(false, false, true) && MidAllowed(true, false, false), "the mid tier is allowed by USE45, USE50 or MSC_FPS=45 only: with none of them live play is 60 <-> 30 as of p98");
    }
    // ---- 5. the ladder 60 -> 50 -> 30 and back
    {
        // 5a. the controller's relief is the mid tier: TierOf
        check(TierOf(true, true) == kTier60 && TierOf(false, true) == kTier45 && TierOf(false, false) == kTier30, "tier of a picture: 60 under the controller's 60, the mid tier under its relief while the mid tier holds, the 30 once it fell");
        // 5b. the 50 holds a scene that fits it (mean 15 ms in a 20 ms slot, 1.5 ms noise) and falls when it does not (mean 24): the model draws a picture's cost, late when over its slot
        struct Rng { uint64_t s; double U() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (double)(s >> 11) / 9007199254740992.0; } double N() { double a = U(), b = U(); if (a < 1e-12) a = 1e-12; return std::sqrt(-2 * std::log(a)) * std::cos(6.283185307 * b); } };
        const double means[4] = {15.0, 17.0, 19.5, 24.0};
        double fellAt[4] = {-1, -1, -1, -1};
        for (int c = 0; c < 4; ++c) {
            Fallback45Config fc = Fallback45Config::Hold(); fc.fps = p50.Fps();
            Fallback45 f(fc); Rng rng{(uint64_t)(77 + c)}; double now = 0; unsigned falls = 0;
            for (int i = 0; i < 50 * 120; ++i) { const double cost = means[c] + 1.5 * rng.N(); const bool late = cost > 20.0; now += 1.0 / 49.95;
#if NP_F50_BREAK == 2
                (void)late; if (f.At45() && f.Picture45(false, now)) { ++falls; }
#else
                if (f.At45() && f.Picture45(late, now)) { ++falls; if (fellAt[c] < 0) fellAt[c] = now; }
#endif
            }
            std::printf("     50 against a scene of %.1f ms a picture (sd 1.5, slot 20 ms): %u falls to 30 in 120 s%s\n", means[c], falls, falls ? "" : " (holds)");
        }
        check(fellAt[0] < 0 && fellAt[1] < 0, "the 50 HOLDS scenes that fit it (15 and 17 ms a picture): no fall to 30 in 120 s");
        check(fellAt[3] > 0 && fellAt[3] < 6.0, "the 50 falls to the 30 within 6 s when the scene cannot make 20 ms (24 ms a picture)");
        check(fellAt[2] < 0 || fellAt[2] > 1.0, "a marginal scene (19.5 ms a picture, 40 % over its slot) is judged over seconds, not at one picture");
        // 5c. 30 -> 50 by TRIAL and by PROOF; the trial's length follows the 50's rate
        {
            Fallback45Config fc = Fallback45Config::Hold(); fc.fps = p50.Fps(); fc.trialAfter = 4.0; fc.trialSecs = 2.0;
            Fallback45 f(fc); double now = 0; unsigned started = 0, ok = 0;
            while (f.At45() && now < 10) { f.Picture45(true, now); now += 1.0 / 49.95; }   // every picture late: falls
            check(!f.At45(), "(setup) the 50 fell to the 30");
            const double fell = now; double started_at = -1;
            while (now < fell + 30.0 && started_at < 0) { if (f.Picture30(true, now, 16.0, true) && f.LastTrialStart()) { started_at = now; ++started; } now += 1.0 / 30.0; }
            check(started == 1 && started_at - fell > 3.5 && started_at - fell < 5.5, "30 -> 50 trial: the 30 tries the 50 after about 4 s");
            unsigned n = 0; while (f.TrialOn() && n < 1000) { f.Picture45(false, now); now += 1.0 / 49.95; ++n; }
            ok = f.TrialsOk();
            check(ok == 1 && n >= 100 + fc.trialSkip - 1 && n <= 100 + fc.trialSkip + 1 && f.At45(), "a clean trial is kept after the skipped pictures and 2 s of the 50's pictures (100 pictures, not the 45's 90)");
            Fallback45 f2(fc); double t2 = 0; while (f2.At45() && t2 < 10) { f2.Picture45(true, t2); t2 += 1.0 / 49.95; }
            unsigned m = 0; bool failed = false; double t3 = t2; while (t3 < t2 + 30 && !failed) { if (f2.Picture30(true, t3, 16.0, true) && f2.LastTrialStart()) { while (f2.TrialOn() && m < 1000) { f2.Picture45(m % 10 == 3, t3); t3 += 1.0 / 49.95; ++m; } failed = f2.TrialsFailed() > 0; } t3 += 1.0 / 30.0; }
            check(failed && !f2.At45(), "a trial in which 10 % of the pictures miss their slot fails: back to the 30, the wait doubles");
        }
        {   // by proof (no trial): after the hold and 8 s of pictures that fit
            Fallback45Config fc = Fallback45Config::Hold(); fc.fps = p50.Fps();
            Fallback45 f(fc); double now = 0; while (f.At45() && now < 10) { f.Picture45(true, now); now += 1.0 / 49.95; }
            const double fell = now; bool back = false; double backAt = 0;
            while (now < fell + 60 && !back) { back = f.Picture30(true, now, 15.0, true); now += 1.0 / 30.0; backAt = now; }
            check(back && backAt - fell >= 8.0 && backAt - fell < 20.0, "30 -> 50 by proof: after the hold (10 s) and the proof (8 s of pictures that fit)");
            Fallback45 f2(fc); now = 0; while (f2.At45() && now < 10) { f2.Picture45(true, now); now += 1.0 / 49.95; }
            bool b2 = false; const double fl = now; while (now < fl + 60 && !b2) { b2 = f2.Picture30(((int)(now * 30)) % 4 != 0, now, 16.0, true); now += 1.0 / 30.0; }
            check(!b2, "30 -> 50: no return while a quarter of the pictures do not fit the 50");
        }
        // 5d. the controller (p98's rules) gives the relief to the mid tier: 60 falls to the relief on a sustained overload, not on a spike
        {
            Config c = Config::Lock60Hold(); c.p98 = true; c.p98Trials = true; Auto ctl(c);
            double now = 0; unsigned scene = 2;
            auto pic = [&](double ms, bool busy) { FrameIn f; std::memset(&f, 0, sizeof f); now += ms / 1000.0; f.now = now; f.interval = ms / 1000.0; f.steps = 1; f.simMs = 5.0; f.costsValid = true; f.gameCore = 0;
                f.cpuMs[0] = busy ? 17.5 : 10.0; f.cpuMs[1] = 6.0; f.scene = scene; f.workMs = 12.0; f.feMs = 8.0; f.beMs = 8.0; f.gpuMinMs = 9.0; f.p98 = true; return ctl.Frame(f); };
            while (now < 28.0) pic(Rms, false);
            const double t0 = now; int i = 0; bool fell = false; while (now - t0 < 20.0 && !fell) { pic(i++ % 3 == 2 ? 2 * Rms : Rms, true); fell = ctl.Current() != kPolicy60; }
            check(fell && TierOf(ctl.Current() == kPolicy60, true) == kTier45, "60 -> 50: a sustained overload at 60 sends it to the relief, and the relief is the mid tier (the 50) while it holds");
        }
    }
    // ---- 6. the seams: a tier change is no repeat, no 2-step picture, no phase jump. The model is the display's: the 60 tier's gate (every vblank), the 50 gate, the 30's rule (2 refreshes after the one before)
    {
        // pictures per tier; the display holds: 60: next vblank; 50: next slot; 30: lastShown + 2. Each entry follows np_fps.cpp's calls (Gate45::Due with havePrev / prevShown like NpFps45Due).
        Gate45 g60(Gate45Config(), true, p50), g50(Gate45Config(), false, p50);
        std::vector<uint32_t> sh; std::vector<char> tier; uint32_t last = 20000;
        auto at60 = [&](int n) { for (int i = 0; i < n; ++i) { const bool first = !g60.Have(); if (g50.Have()) g50.Leave(); const uint32_t e = last + 1u; const uint32_t d = g60.Due(e, false, 0, !sh.empty() && first, last - 1u); g60.Shown(d); sh.push_back(d); tier.push_back(60); last = d; } };
        auto at50 = [&](int n) { for (int i = 0; i < n; ++i) { const bool first = !g50.Have(); if (g60.Have()) g60.Leave(); const uint32_t e = last + 1u; const uint32_t d = g50.Due(e, false, 0, !sh.empty() && first, last); g50.Shown(d); sh.push_back(d); tier.push_back(50); last = d; } };
        auto at30 = [&](int n) { if (g50.Have()) g50.Leave(); if (g60.Have()) g60.Leave(); for (int i = 0; i < n; ++i) { const uint32_t d = last + 2u; sh.push_back(d); tier.push_back(30); last = d; } };
        at60(30); at50(30); at30(10); at50(30); at60(20); at50(25); at60(10);
        bool ok = true; unsigned maxHold = 0, three = 0;
        for (size_t i = 1; i < sh.size(); ++i) { const unsigned h = sh[i] - sh[i - 1]; if (h > maxHold) maxHold = h; if (h >= 3) { ++three; ok = false; } if (h == 0) ok = false; }
        check(ok && maxHold <= 2, "tier changes 60 -> 50 -> 30 -> 50 -> 60 -> 50 -> 60: no picture is held 3 refreshes (no repeat beyond the pattern's own 2) and none is skipped");
        // the 2-refresh holds inside the 50 stretches are only the pattern's: one in six refreshes; the entry never adds one beyond the pattern's
        unsigned twos50 = 0, n50 = 0; for (size_t i = 1; i + 1 < sh.size(); ++i) if (tier[i] == 50 && tier[i + 1] == 50) { ++n50; if (sh[i + 1] - sh[i] == 2) ++twos50; }
        check(n50 > 0 && twos50 * 5 <= n50 + 5, "50 stretches: at most one picture in five held 2 refreshes, including their entries");
        // no phase jump: inside one stretch every picture is on a slot of ONE phase (the gate keeps it); the entry takes the phase from its first picture
        bool phok = true; { Gate45 g(Gate45Config(), false, p50); uint32_t l = 30000; const uint32_t d0 = g.Due(l, false, 0, true, l - 2); g.Shown(d0); l = d0; const unsigned ph = g.Phase();
            for (int i = 0; i < 100; ++i) { const uint32_t d = g.Due(l + 1u, false, 0, false, 0); g.Shown(d); phok = phok && g.Phase() == ph && g.Pat().IsSlot(d, ph); l = d; } }
        check(phok, "50 stretch after a tier change: one phase throughout (no phase jump), every picture on a slot");
    }
    // ---- 7. the seam with the clock: the frames of a tier change show their display time (np_fps.cpp seeds the new tier's clock from the old tier's newest frame, as modelled here), and the 60 -> 50 entry holds no picture 2
    {
        Gate45Config gc; gc.enterOnTarget = true;
        Gate45 g50(gc, false, p50), g60(gc, true);
        Clock45 c50(p50), c60;
        const int L = 3; const int N = 520;
        std::vector<uint32_t> shown(N + 1, 0), stamp(N + 1, 0); std::vector<int> tierOf(N + 1, 60); std::vector<double> game(N + 1, 0);
        for (int k = 1; k <= N; ++k) tierOf[k] = (k <= 80) ? 60 : (k <= 220) ? 50 : (k <= 300) ? 60 : (k <= 420) ? 50 : 60;
        uint32_t vc = 40000; int fb = 0; bool prevIs60 = false;
        for (int k = 1; k <= N; ++k) {
            const int t = tierOf[k], tp = k > 1 ? tierOf[k - 1] : t;
            Clock45& c = t == 50 ? c50 : c60;
            if (t == 50) {
                uint32_t slot = 0;
                if (c50.NextSlot(L, fb > 0 && tierOf[fb] == 50, (uint64_t)fb, fb > 0 ? shown[fb] : 0u, fb > 0 && tierOf[fb] == 50 ? (int)g50.Phase() : -1, &slot) && (int32_t)(slot - 5u - vc) > 0) vc = slot - 5u;
            }
            stamp[k] = vc;
            uint32_t seedG = 0; bool seeded = false;
            if (k > 1 && t != tp) {
                Clock45& prev = tp == 50 ? c50 : c60;
                if (prev.NewestTarget(&seedG)) { c.Seed(stamp[k - 1], seedG, t == 60 ? (int)Pattern45::kEvery : -1); seeded = true; }
                (t == 50 ? c60 : c50).Begin(false, 0, 0, false, 0, 0, -1);
            }
            const bool haveFb = fb > 0 && tierOf[fb] == t;
            const int fbPhase = haveFb ? (t == 50 ? (int)g50.Phase() : (int)Pattern45::kEvery) : -1;
            const int adv = c.Begin(true, vc, L, haveFb, (uint64_t)fb, haveFb ? shown[fb] : 0u, fbPhase);
            uint32_t gs = 0; const bool ht = c.NewestTarget(&gs); c.Handed((uint64_t)k);
            const uint32_t earliest = vc + (uint32_t)L;
            uint32_t due;
            const uint32_t lastShown = k > 1 ? shown[k - 1] : 0u;
            if (t == 50) {
                const bool first = !g50.Have(); if (g60.Have()) g60.Leave();
#if NP_F50_BREAK == 3
                const uint32_t entryPrev = lastShown;
#else
                const uint32_t entryPrev = prevIs60 ? lastShown - 1u : lastShown;
#endif
                due = g50.Due(earliest, ht, gs, k > 1 && first, entryPrev); g50.Shown(due); prevIs60 = false;
            } else {
                const bool first = !g60.Have(); if (g50.Have()) g50.Leave();
                due = g60.Due(earliest, ht, gs, k > 1 && first, lastShown - 1u); g60.Shown(due); prevIs60 = true;
            }
            (void)seeded;
            shown[k] = due; vc += 1;
            while (fb + 1 <= k && shown[fb + 1] <= vc) ++fb;
            game[k] = k > 1 ? game[k - 1] + (adv >= 0 ? (double)adv : (double)(stamp[k] - stamp[k - 1])) : 0.0;
        }
        unsigned errAll = 0, errEntry = 0, holds3 = 0, entryTwo = 0; unsigned n = 0;
        for (int k = 40; k <= N; ++k) {
            ++n; const double dg = game[k] - game[k - 1]; const uint32_t ds = shown[k] - shown[k - 1];
            const bool entry = k > 2 && (tierOf[k] != tierOf[k - 1] || tierOf[k] != tierOf[k - 2]);   // the first two pictures of a stretch
            if (std::fabs(dg - (double)ds) > 1e-9) { ++errAll; if (entry) ++errEntry; }
            if (ds >= 3) ++holds3;
            if (tierOf[k] == 50 && tierOf[k - 1] == 60 && ds != 1) ++entryTwo;
            if (tierOf[k] == 50 && tierOf[k - 1] == 60 && k + 1 <= N && shown[k] - shown[k - 1] >= 2) ++entryTwo;
        }
        std::printf("     seams: %u pictures over 5 tier changes, %u with a game time off their display time (%u of them in the first two pictures of a stretch), %u held 3+, %u 60 -> 50 entries held 2\n", n, errAll, errEntry, holds3, entryTwo);
        check(errEntry == 0, "seams: the FIRST pictures of a stretch after a tier change (60 -> 50 and 50 -> 60) show their display time: no motion error at the entries");
        check(errAll == 0 && holds3 == 0, "seams: every picture over five tier changes shows its display time and none is held 3 refreshes");
        check(entryTwo == 0, "seams: the 60 -> 50 entry holds the last 60 picture 1 refresh (no repeat at the seam)");
    }
    if (g_fail) { std::printf("f50_unit: %d FAILED\n", g_fail); return 1; }
    std::printf("f50_unit: all checks passed (5 / 6 pattern, 3 / 4 unchanged, gate, display-time clock, tier choice, ladder 60 -> 50 -> 30, trial and proof back, seams)\n");
    return 0;
}
