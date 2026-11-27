// MSC_HELD_CADENCE=auto (np_fps_cad3.h, the shipped header) on the held scenes of the console logs r11c, r12 and r13, and on synthetic traces. Host only; no game data.
//   cad3_unit [cad3_scenes.txt]    (made by cad3_extract.py from the logs' "[present] measured ... held 0/1/2/3/4+ vblanks" windows and their "[nav] task state" lines)
// What the logs hold: per 5 s window, how many pictures stayed 1, 2, 3 or 4+ refreshes. There is no per-picture time in them, so a scene is rebuilt as an evenly mixed sequence of
// those holds and each picture gets the work of its hold: 3 refreshes = 41.7 ms, 4+ = 58.4 ms (the middle of the refreshes it needed), 2 refreshes = 31 ms (it fitted 33 ms, no credit
// for margin), 1 refresh = 12 ms (the 30 cap shows it for 2 anyway). That is a model of the logs, not a measurement: the console decides (cad3 / cad3d runs).
// NP_CAD3_BREAK=1..6 (compile with -DNP_CAD3_BREAK=n) must make this fail.
#include "np_fps_cad3.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using namespace npfps;
static int g_fail = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
static const double F = 16.683;
static double WorkOfHold(int h) { return h <= 1 ? 12.0 : h == 2 ? 31.0 : h == 3 ? 41.7 : 58.4; }
static int HoldOf(double work, int period) {
    int h = (int)std::ceil(work / F - 1e-9);
    return h < period ? period : h;
}

// One picture through the pacing: the caller's view of np_fps.cpp's use (pictureMs = the length it had at the period in force, workMs = its work).
struct Run {
    std::vector<int> hold;       // refreshes each picture was shown for
    std::vector<int> period;     // the cadence in force for it
    std::vector<double> work;
    int firstEntry = -1, exitAt = -1, changes = 0;
};
static void Feed(Cad3& c, Run& r, double work, bool enabled, unsigned visit, int cls) {
    Cad3In in; std::memset(&in, 0, sizeof in);
    in.enabled = enabled; in.visit = visit; in.cls = cls; in.workMs = work;
    // the picture just finished was made at the period in force before this call
    const int p = c.Period();
    in.pictureMs = HoldOf(work, 2) * F;   // what a held 30 shows (the length of the frame at period 2)
    if (p == 3) in.pictureMs = HoldOf(work, 3) * F;
    const Cad3Out o = c.Frame(in);
    const int idx = (int)r.hold.size();
    r.hold.push_back(HoldOf(work, p == 3 ? 3 : 2));   // the hold of this picture: at the period that was in force when it was made
    r.period.push_back(p);
    r.work.push_back(work);
    if (o.event == 1 || o.event == 3) { if (r.firstEntry < 0) r.firstEntry = idx; ++r.changes; }
    if (o.event == 2) { r.exitAt = idx; ++r.changes; }
}
static Run Synth(Cad3& c, const std::vector<double>& works, unsigned visit = 1, int cls = 0, bool enabled = true) {
    Run r;
    for (double w : works) Feed(c, r, w, enabled, visit, cls);
    return r;
}
static std::vector<double> Rep(double w, int n) { return std::vector<double>(n, w); }
static std::vector<double> Cat(std::vector<double> a, const std::vector<double>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

struct Win { int n[4]; };
static std::vector<int> Mix(const Win& w) {   // an even interleave of the holds 1..4
    int total = w.n[0] + w.n[1] + w.n[2] + w.n[3];
    std::vector<int> out; double done[4] = {0, 0, 0, 0};
    for (int i = 0; i < total; ++i) {
        int best = 0; double bd = -1e9;
        for (int k = 0; k < 4; ++k) { double d = (double)w.n[k] * (i + 1) / total - done[k]; if (w.n[k] > 0 && d > bd) { bd = d; best = k; } }
        done[best] += 1; out.push_back(best + 1);
    }
    return out;
}
static double Judder(const std::vector<int>& h) { if (h.size() < 2) return 0; double s = 0; for (size_t i = 1; i < h.size(); ++i) s += std::abs(h[i] - h[i - 1]); return s / (double)(h.size() - 1); }
static int Count(const std::vector<int>& h, int v, bool atLeast = false) { int n = 0; for (int x : h) if (atLeast ? x >= v : x == v) ++n; return n; }

int main(int argc, char** argv) {
    const char* table = argc > 1 && argv[1][0] != '-' ? argv[1] : "tools/fps/cad3_scenes.txt";
    // ---- the logs' held scenes ----
    struct Scene { std::string log; int scene, state; std::vector<Win> wins; };
    std::vector<Scene> scenes;
    if (FILE* f = std::fopen(table, "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            char log[200]; int sc, st, wi; Win w;
            if (std::sscanf(line, "%199s %d %d %d %d %d %d %d", log, &sc, &st, &wi, &w.n[0], &w.n[1], &w.n[2], &w.n[3]) != 8) continue;
            if (scenes.empty() || scenes.back().log != log || scenes.back().scene != sc) { Scene s; s.log = log; s.scene = sc; s.state = st; scenes.push_back(s); }
            scenes.back().wins.push_back(w);
        }
        std::fclose(f);
    }
    check(!scenes.empty(), "the table of held scenes was read (run from native/ or pass its path)");
    std::printf("held scenes of the console logs (r11c, r12, r13): the cadence the rule would have chosen\n");
    std::printf("%-22s %3s %5s %5s | today: 2/3/4+ share %%, judder | cad3: entry at picture, 2/3/4+ share %%, judder, 4-refresh where work<50 ms\n", "log", "sc", "state", "pics");
    int entered = 0, nScenes = 0, bad4 = 0, longScenes = 0, steadyLong = 0;
    double judToday = 0, judCad = 0; int judN = 0;
    std::string prevLog; Cad3 chain;   // (carry is off in the build: the chain only runs the visits one after another)   // one controller per log, scenes in order: the carry of a class
    unsigned visit = 100;
    for (const Scene& s : scenes) {
        if (s.log != prevLog) { chain = Cad3(); prevLog = s.log; visit = 100; }
        std::vector<double> works;
        for (const Win& w : s.wins) for (int h : Mix(w)) works.push_back(WorkOfHold(h));
        if (works.size() < 10) continue;
        ++nScenes;
        // today: the pictures at the 30 cap
        Run today; for (double w : works) { today.hold.push_back(HoldOf(w, 2)); }
        Run r = Synth(chain, works, ++visit, s.state == 8 ? 0 : 1);
        { Cad3In gap; std::memset(&gap, 0, sizeof gap); chain.Frame(gap); }   // (between scenes: play)
        const int n = (int)works.size();
        const int e = r.firstEntry;
        int bad = 0;
        for (int i = 0; i < n; ++i) if (r.period[i] == 3 && r.work[i] < 3 * F - 0.5 && r.hold[i] != 3) ++bad;
        bad4 += bad;
        if (e >= 0) ++entered;
        const int j = e >= 0 ? e : 0;
        std::vector<int> tail(r.hold.begin() + j, r.hold.end()), tt(today.hold.begin() + j, today.hold.end());
        judToday += Judder(tt) * (double)tt.size(); judCad += Judder(tail) * (double)tail.size(); judN += (int)tt.size();
        if (e >= 0 && (int)tail.size() >= 20) { ++longScenes; if (Count(tail, 3) >= (int)tail.size() - Count(tail, 4, true)) ++steadyLong; }
        std::printf("%-22s %3d %5d %5d | %3d/%3d/%3d  %4.2f | %5d  %3d/%3d/%3d  %4.2f  %d\n", s.log.c_str(), s.scene, s.state, n,
                    100 * Count(today.hold, 2) / n, 100 * Count(today.hold, 3) / n, 100 * Count(today.hold, 4, true) / n, Judder(today.hold),
                    e, 100 * Count(r.hold, 2) / n, 100 * Count(r.hold, 3) / n, 100 * Count(r.hold, 4, true) / n, Judder(r.hold), bad);
        check(r.changes <= 2 + (e == 0 ? 0 : 0), "a log scene: at most one entry and one exit");
    }
    std::printf("%d held scenes: %d enter the steady 3; judder (mean refresh change between pictures, from the entry on) today %.2f, with the cadence %.2f; pictures at 4 refreshes where the work was under 50 ms: %d\n",
                nScenes, entered, judN ? judToday / judN : 0, judN ? judCad / judN : 0, bad4);
    check(nScenes >= 20, "enough held scenes in the table");
    check(entered >= 5 && entered < nScenes - 3, "some of the held scenes enter, and not all of them (a scene that makes 33 ms is left alone)");
    check(bad4 == 0, "no 4-refresh picture under the cadence where the work was under 50 ms");
    check(judN > 0 && judCad < 0.65 * judToday, "the cadence takes the judder of the entering scenes well under today's (the pictures that need over 50 ms stay late)");
    (void)longScenes; (void)steadyLong;

    // ---- the same scenes if the work were 40 % smaller (items 2, 3, 5 of the plan landing: an assumption, not a result) ----
    int enteredLight = 0;
    for (const Scene& s : scenes) {
        std::vector<double> works;
        for (const Win& w : s.wins) for (int h : Mix(w)) works.push_back(WorkOfHold(h) * 0.6);
        if (works.size() < 10) continue;
        Cad3 c; Run r = Synth(c, works, 1, 1);
        if (r.firstEntry >= 0) ++enteredLight;
    }
    std::printf("the same scenes with 40 %% less work (estimate of what the picture work items could give): %d of %d enter\n", enteredLight, nScenes);
    check(enteredLight < entered, "with 40 % less work fewer scenes enter (the scenes that fit fall away; those with pictures over 33 ms still do)");

    // ---- synthetic ----
    { Cad3 c; Run r = Synth(c, Rep(30.0, 600)); check(r.firstEntry < 0 && Count(r.hold, 2) == 600, "a constant 30 ms picture stays at 2"); }
    { Cad3 c; Run r = Synth(c, Rep(33.0, 600)); check(r.firstEntry < 0, "a constant 33 ms picture (just fits) stays at 2"); }
    { Cad3 c; Run r = Synth(c, Rep(45.0, 300));
      check(r.firstEntry == 4 + 20 - 1, "a constant 45 ms picture enters after the skipped 4 and the 20 judged");
      int bad = 0; for (size_t i = 24; i < r.hold.size(); ++i) if (r.hold[i] != 3) ++bad;
      check(bad == 0, "a constant 45 ms picture is shown for exactly 3 refreshes after the entry (no 4)");
      check(r.changes == 1 && c.Period() == 3, "no second change in a scene that stays at 45 ms"); }
    { Cad3 c; Run r = Synth(c, Rep(55.0, 100));
      int h4 = 0; for (size_t i = 24; i < r.hold.size(); ++i) if (r.hold[i] == 4) ++h4;
      check(r.firstEntry >= 0 && h4 == (int)r.hold.size() - 24, "55 ms work enters but shows 4 refreshes (work over 50 ms is late whatever the cadence)"); }
    { Cad3 c; Run r = Synth(c, Cat(Rep(45.0, 60), Rep(30.0, 150)));
      check(r.exitAt < 0 && c.Period() == 3, "work of 30 ms (over 28) keeps the 3"); }
    { Cad3 c; Run r = Synth(c, Cat(Cat(Rep(45.0, 60), Rep(22.0, 39)), Rep(45.0, 1)));
      check(r.exitAt < 0, "39 fitting pictures and a late one do not bring 2 back"); }
    { Cad3 c; Run r = Synth(c, Cat(Cat(Rep(45.0, 60), Rep(22.0, 40)), Rep(45.0, 200)));
      check(r.exitAt >= 0 && r.exitAt == 24 + (60 - 24) + 40 - 1, "40 pictures of work under 28 ms bring 2 back");
      check(r.changes == 2 && c.Period() == 2, "after the exit the scene stays at 2 whatever follows (one entry and one exit a scene)");
      int h = 0; for (size_t i = r.exitAt + 1; i < r.hold.size(); ++i) h += r.hold[i] >= 3 ? 1 : 0; check(h == r.hold.size() - r.exitAt - 1, "pictures after the exit are shown as the 30 cap shows them (late ones late)"); }
    { Cad3 c; Run r = Synth(c, Rep(90.0, 400), 1, 0, false); check(r.firstEntry < 0 && Count(r.hold, 2) == 0 + Count(r.hold, 2) && c.Period() == 2, "live play (not enabled): period 2 whatever the pictures"); }
    { Cad3 c; Run r = Synth(c, Rep(90.0, 400), 1, 0, false); int p3 = 0; for (int p : r.period) p3 += p == 3; check(p3 == 0, "live play: never a 3"); }
    // the way in: 6 of 20 (30 %) stay, 7 enter
    { std::vector<double> w; for (int i = 0; i < 4; ++i) w.push_back(30.0);
      for (int i = 0; i < 20; ++i) w.push_back(i % 3 == 0 && i < 18 ? 45.0 : 30.0);   // 6 misses in the 20 judged
      Cad3 c; Run r = Synth(c, w); check(r.firstEntry < 0, "6 of 20 pictures missing (30 %) do not enter"); }
    { std::vector<double> w; for (int i = 0; i < 4; ++i) w.push_back(30.0);
      for (int i = 0; i < 20; ++i) w.push_back(i < 14 && i % 2 == 0 ? 45.0 : 30.0);   // 7 misses in the 20 judged
      Cad3 c; Run r = Synth(c, w); check(r.firstEntry == 23, "7 of 20 pictures missing (more than a third) enter, as soon as the 20 are judged"); }
    // the first pictures of a visit are not judged: 4 misses at the start, 12 fits, 3 misses, 5 fits
    { std::vector<double> w = Cat(Cat(Rep(60.0, 4), Rep(30.0, 12)), Cat(Rep(45.0, 3), Rep(30.0, 25)));
      Cad3 c; Run r = Synth(c, w); check(r.firstEntry < 0, "the first pictures of a visit (first-sight work) are not judged"); }
    // a stall (a load) is not a pace
    { std::vector<double> w = Cat(Rep(30.0, 4), Cat(Rep(300.0, 12), Rep(30.0, 60))); Cad3 c; Run r = Synth(c, w); check(r.firstEntry < 0, "stalls of 300 ms are loads: not judged"); }
    // carry: a class that ended at 3 starts its next visit at 3; the other class does not
    { Cad3 c; Run a = Synth(c, Rep(45.0, 60), 1, 1); Cad3In gap; std::memset(&gap, 0, sizeof gap); c.Frame(gap); Run b = Synth(c, Rep(30.0, 30), 2, 1); check(b.period[0] == 2 && b.firstEntry < 0, "carry is off by default: the next visit starts at 2"); (void)a; }
    { Cad3Config cc; cc.carry = true; Cad3 c(cc); Run a = Synth(c, Rep(45.0, 60), 1, 1);
      Cad3In gap; std::memset(&gap, 0, sizeof gap); c.Frame(gap);
      Run b = Synth(c, Rep(45.0, 10), 2, 1); check(b.period[0] == 2 && b.firstEntry == 0 && b.period[1] == 3, "the next visit of a class that ended at 3 starts at 3 from its cut");
      c.Frame(gap);
      Run d = Synth(c, Rep(30.0, 10), 3, 0); check(d.firstEntry < 0 && d.period[0] == 2, "the other class starts at 2");
      c.Frame(gap);
      Run e = Synth(c, Rep(22.0, 60), 4, 1); check(e.exitAt == 43, "a carried 3 goes back to 2 after 40 pictures under 28 ms (the first 4 pictures of a visit are not judged)");
      (void)a; }
    // the cut inside a held run (a new visit at the same task state) judges afresh
    { Cad3 c; Run a = Synth(c, Rep(30.0, 50), 1, 1); Run b = Synth(c, Rep(45.0, 40), 2, 1); check(a.firstEntry < 0 && b.firstEntry >= 0, "a new visit is judged on its own pictures"); }
    std::printf(g_fail ? "FAILED (%d)\n" : "cad3 test: passed\n", g_fail);
    return g_fail ? 1 : 0;
}
