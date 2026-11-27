// The start-up clock rules (src/np_vita_clock.h) on a fake power API. Control flow only: this says nothing about what a console really runs at.
//   clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -Inative/platform/vita/src native/platform/vita/tests/test_clock_policy.cpp -o /tmp/test_clock_policy && /tmp/test_clock_policy
#include "np_vita_clock.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace NpVitaClock;

namespace {
int g_checks, g_failures;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}

struct Fake {
    int measured[2] = {333, 444};   // before, after
    int armRc[2] = {0, 0};          // first and second SetArm
    int busRc = 0, gpuRc = 0, xbarRc = 0;
    int getArm = 444, getBus = 222, getGpu = 222, getXbar = 166;
    std::vector<std::string> calls;
    int measures = 0, armCalls = 0;
    int SetArm(int v) { calls.push_back("arm" + std::to_string(v)); const int rc = armRc[armCalls < 2 ? armCalls : 1]; ++armCalls; return rc; }
    int SetBus(int v) { calls.push_back("bus" + std::to_string(v)); return busRc; }
    int SetGpu(int v) { calls.push_back("gpu" + std::to_string(v)); return gpuRc; }
    int SetXbar(int v) { calls.push_back("xbar" + std::to_string(v)); return xbarRc; }
    int GetArm() { return getArm; }
    int GetBus() { return getBus; }
    int GetGpu() { return getGpu; }
    int GetXbar() { return getXbar; }
    int MeasureMHz() { const int v = measured[measures < 2 ? measures : 1]; ++measures; return v; }
    int ArmRequests(int mhz) const { int n = 0; for (const std::string& c : calls) if (c == "arm" + std::to_string(mhz)) ++n; return n; }
    int AllArmRequests() const { int n = 0; for (const std::string& c : calls) if (c.rfind("arm", 0) == 0) ++n; return n; }
    bool Rest() const {   // the other three, once each, in the order of 1.2
        const size_t n = calls.size();
        return n >= 3 && calls[n - 3] == "bus222" && calls[n - 2] == "gpu222" && calls[n - 1] == "xbar166";
    }
};
constexpr int kRefused = static_cast<int>(0x802B0000);
}  // namespace

int main() {
    // MSC_VITA_CPU_MHZ exactly as in 1.2.
    for (const char* v : {"111", "166", "222", "333", "444", "333x", " 222"}) {
        Fake f;
        f.measured[0] = f.measured[1] = 498;   // not even a plugin's 500 changes an explicit request
        const Result r = Apply(f, v, nullptr);
        Check(r.explicitValue && r.armRequest == std::atoi(v) && f.AllArmRequests() == 1 && f.ArmRequests(500) == 0, "an explicit value is that one request, never 500");
        Check(f.Rest(), "explicit: the other three as always");
        Check(r.measuredBefore == 0, "explicit: nothing is measured to decide it");
    }
    {
        Fake f;
        const Result r = Apply(f, "333", "max");
        Check(r.armRequest == 333 && f.ArmRequests(500) == 0, "explicit lower wins over the max policy");
    }
    for (const char* v : {"500", "0", "", "fast", "445", "1000"}) {
        Check(ExplicitMHz(v) == 0, "values 1.2 ignored count as unset");
        Fake f;
        const Result r = Apply(f, v, nullptr);
        Check(!r.explicitValue && r.armRequest == 444 && f.AllArmRequests() == 1, "an invalid value behaves as unset (444 on a core that measures below the floor)");
    }
    Check(ExplicitMHz(nullptr) == 0, "unset");
    // Default policy.
    {
        Fake f;
        const Result r = Apply(f, nullptr, nullptr);
        Check(r.policy == Policy::kMeasure && r.armRequest == 444 && r.armRc == 0 && f.AllArmRequests() == 1 && f.ArmRequests(500) == 0, "no plugin: as today, one 444, no 500");
        Check(f.calls[0] == "arm444" && f.Rest() && f.calls.size() == 4, "the order of 1.2");
        Check(r.measuredBefore == 333 && r.measuredAfter == 444, "both measurements kept");
    }
    {
        Fake f;
        f.measured[0] = f.measured[1] = 498;
        f.getArm = 333;
        const Result r = Apply(f, nullptr, nullptr);
        Check(r.armRequest == 0 && f.AllArmRequests() == 0, "a plugin's 498 gets no ARM request at all");
        Check(f.Rest() && f.calls.size() == 3, "the other three are still made");
        Check(r.reportedArm == 333, "the getter's 333 is kept apart from the measurement");
    }
    {
        Fake f;
        f.measured[0] = kPluginFloorMHz - 1;
        const Result r = Apply(f, nullptr, nullptr);
        Check(r.armRequest == 444, "just under the floor: 444");
        Fake g;
        g.measured[0] = kPluginFloorMHz;
        Check(Apply(g, nullptr, nullptr).armRequest == 0, "at the floor: left alone");
    }
    {
        Fake f;
        f.measured[0] = 0;   // the loop failed
        Check(Apply(f, nullptr, nullptr).armRequest == 444, "no measurement: as today");
    }
    // Fixed: 1.2, nothing measured.
    {
        Fake f;
        f.measured[0] = f.measured[1] = 498;
        const Result r = Apply(f, nullptr, "fixed");
        Check(r.armRequest == 444 && f.ArmRequests(444) == 1 && f.measures == 0, "fixed is 1.2: 444 over anything, no loop");
        {
            Fake f; f.measured[0] = 440; f.armRc[0] = (int)0x802B0000; f.getArm = 444;
            const Result r = Apply(f, nullptr, "max");
            const KeepClock k = KeepArm(r);
            Check(r.fallbackTried && k.request == 444 && k.baseline == 444, "500 refused, 444 taken: 444 is what is kept");
        }
        {
            // Galaxy's console log (r59): profile at 498, 500 refused 0x802B0000, the system reports 333. The 444 fallback must never be asked for.
            Fake f; f.measured[0] = 498; f.measured[1] = 498; f.armRc[0] = (int)0x802B0000; f.getArm = 333;
            const Result r = Apply(f, nullptr, "max");
            Check(f.AllArmRequests() == 0 && r.armRequest == 0 && !r.fallbackTried, "a profile at 498 (clock17): no ARM request at all, 500 or 444");
            Check(KeepArm(r).baseline == 0 && !NeedsRestore(r, 333), "a profile at 498 reporting 333: neither thread touches it");
        }
        Check(ParsePolicy("nonsense") == Policy::kMeasure && ParsePolicy("") == Policy::kMeasure, "an unknown policy is the default");
    }
    // Max: one ordinary 500.
    {
        Fake f;
        f.armRc[0] = kRefused;
        const Result r = Apply(f, nullptr, "max");
        Check(r.armRequest == 500 && r.armRc == kRefused && r.fallbackTried && r.fallbackRc == 0, "negative 500 then 444");
        Check(f.ArmRequests(500) == 1 && f.ArmRequests(444) == 1 && f.calls[0] == "arm500" && f.calls[1] == "arm444", "exactly one of each, in that order");
    }
    {
        Fake f;
        f.armRc[0] = kRefused;
        f.armRc[1] = kRefused + 1;
        const Result r = Apply(f, nullptr, "max");
        Check(r.fallbackTried && r.fallbackRc == kRefused + 1 && f.AllArmRequests() == 2, "a failed fallback is kept, not retried");
        Check(r.armRc == kRefused && f.Rest(), "both results observable, the other three made");
    }
    {
        Fake f;
        f.armRc[0] = kRefused;
        f.measured[0] = 498;
        const Result r = Apply(f, nullptr, "max");
        Check(!r.fallbackTried && f.AllArmRequests() == 0, "a plugin's 498: nothing asked, so nothing refused and never 444");
    }
    {
        Fake f;   // success, the getter says 444 (a hook that took the normal path)
        const Result r = Apply(f, nullptr, "max");
        Check(r.armRc == 0 && !r.fallbackTried && f.ArmRequests(444) == 0 && r.reportedArm == 444, "successful 500 with a 444 readback: no fallback");
    }
    {
        Fake f;   // success-but-ignored: an owner lock answers 0, nothing changes
        f.getArm = 333;
        f.measured[1] = 333;
        const Result r = Apply(f, nullptr, "max");
        Check(r.armRc == 0 && !r.fallbackTried && f.AllArmRequests() == 1 && r.measuredAfter == 333, "an ignored request: no fallback, no retry, and the line shows it");
    }
    {
        Fake f;
        f.getArm = -5;
        const Result r = Apply(f, nullptr, "max");
        Check(r.reportedArm == -5 && !r.fallbackTried, "a failed getter decides nothing and is kept");
    }
    // The four setters are independent.
    {
        Fake f;
        f.armRc[0] = -1; f.busRc = -2; f.gpuRc = -3; f.xbarRc = -4;
        const Result r = Apply(f, nullptr, nullptr);
        Check(r.armRc == -1 && r.busRc == -2 && r.gpuRc == -3 && r.xbarRc == -4, "each result is its own");
        Check(f.AllArmRequests() == 1 && f.calls.size() == 4 && f.Rest(), "a failure stops none of the others; default policy never retries");
    }
    // The line.
    {
        Fake f;
        f.measured[0] = f.measured[1] = 498;
        f.getArm = 333;
        const Result r = Apply(f, nullptr, nullptr);
        char line[400];
        const int n = Format(r, line, sizeof(line));
        Check(n > 0 && n < 400, "the line fits");
        const std::string s = line;
        Check(s.rfind("clocks: policy measure", 0) == 0 && s.find("requested ARM none") != std::string::npos && s.find("reported ARM 333") != std::string::npos &&
                  s.find("measured 498 before, 498 after") != std::string::npos, line);
        Fake g;
        g.armRc[0] = kRefused;
        const Result q = Apply(g, nullptr, "max");
        Format(q, line, sizeof(line));
        Check(std::string(line).find("500 rc 0x802B0000, then 444 rc 0x0") != std::string::npos, line);
    }
    // No state between calls: the header holds nothing that a resume or a retry could use.
    {   // a plugin's clock that goes away is put back; every other start is left alone
        NpVitaClock::Result r; r.policy = NpVitaClock::Policy::kMeasure; r.armRequest = 0; r.reportedArm = 500;
        Check(NpVitaClock::WatchUseful(r), "restore: a report of 500 at the start can be watched");
        // The Galaxy port's console: PSVshellPlus at a real 498 MHz, the getter at 333 from the start. A 444 request over that is what the policy exists to avoid.
        NpVitaClock::Result under = r; under.reportedArm = 333;
        Check(!NpVitaClock::WatchUseful(under), "restore: a plugin the getter reads as 333 from the start is not watched");
        Check(!NpVitaClock::NeedsRestore(under, 333), "restore: !NpVitaClock::NeedsRestore(under, 333)");
        NpVitaClock::Result failed = r; failed.reportedArm = -1;
        Check(!NpVitaClock::NeedsRestore(failed, 333), "restore: a getter that failed at the start is no base to compare with");
        Check(NpVitaClock::NeedsRestore(r, 333), "restore: NpVitaClock::NeedsRestore(r, 333)");
        Check(!NpVitaClock::NeedsRestore(r, 444), "restore: !NpVitaClock::NeedsRestore(r, 444)");
        Check(!NpVitaClock::NeedsRestore(r, 500), "restore: !NpVitaClock::NeedsRestore(r, 500)");
        Check(!NpVitaClock::NeedsRestore(r, -1), "restore: !NpVitaClock::NeedsRestore(r, -1)");   // a failed read is no reason to act
        NpVitaClock::Result asked = r; asked.armRequest = 444;
        Check(!NpVitaClock::NeedsRestore(asked, 333), "restore: !NpVitaClock::NeedsRestore(asked, 333)");
        NpVitaClock::Result chosen = r; chosen.explicitValue = true;
        Check(!NpVitaClock::NeedsRestore(chosen, 222), "restore: !NpVitaClock::NeedsRestore(chosen, 222)");
        NpVitaClock::Result fixed = r; fixed.policy = NpVitaClock::Policy::kFixed;
        Check(!NpVitaClock::NeedsRestore(fixed, 333), "restore: !NpVitaClock::NeedsRestore(fixed, 333)");
    }
    // Lower requests of the other three (MSC_VITA_GPU_MHZ, _BUS_MHZ, _XBAR_MHZ): lower only, each its own.
    {
        Fake f;
        const Result r = Apply(f, nullptr, nullptr, "166", "111", "111");
        Check(r.gpuRequest == 166 && r.busRequest == 111 && r.xbarRequest == 111, "lower requests are taken");
        Check(f.calls[f.calls.size() - 3] == "bus111" && f.calls[f.calls.size() - 2] == "gpu166" && f.calls.back() == "xbar111", "and are what is asked for");
    }
    for (const char* v : {"222", "250", "333", "0", "", "fast"}) {
        Fake f;
        const Result r = Apply(f, nullptr, nullptr, v, v, v);
        Check(r.gpuRequest == 222 && r.busRequest == 222 && r.xbarRequest == 166 && f.Rest(), "anything else is the top, never above it");
    }
    {
        Fake f;
        const Result r = Apply(f, nullptr, nullptr, nullptr, nullptr, "166");
        Check(r.xbarRequest == 166, "xbar 166 is the top");
    }
    {
        Fake f;
        char line[400];
        Format(Apply(f, nullptr, nullptr, "111"), line, sizeof line);
        Check(std::string(line).find("GPU 111 rc") != std::string::npos, "the line says what was asked");
    }
    // The keep rules.
    {
        KeepClock c = KeepOther(222, 222);
        bool fell, back;
        Check(!KeepLook(c, 222, &fell, &back) && !fell && !back, "steady: nothing");
        Check(KeepLook(c, 111, &fell, &back) && fell && !back, "a fall: one request and a note");
        Check(KeepLook(c, 111, &fell, &back) && !fell, "still down: again, no second note");
        Check(!KeepLook(c, 222, &fell, &back) && back && !fell, "back: a note, no request");
        Check(KeepLook(c, 111, &fell, &back) && fell, "a later fall counts anew");
        Check(KeepLook(c, 111, &fell, &back) && KeepLook(c, 111, &fell, &back), "second and third try");
        Check(!KeepLook(c, 111, &fell, &back) && !KeepLook(c, 111, &fell, &back), "then a lock is left alone");
        Check(!KeepLook(c, -1, &fell, &back) && !KeepLook(c, 0, &fell, &back), "a failed getter says nothing");
    }
    {
        KeepClock c = KeepOther(222, 166);   // a plugin holds 166 from the start
        bool fell, back;
        Check(!KeepLook(c, 166, &fell, &back) && !fell, "a plugin's lower clock is the baseline, not a fall");
        Check(KeepLook(c, 111, &fell, &back) && c.request == 222, "under it, the game's own request");
    }
    {
        KeepClock none;
        bool fell, back;
        Check(!KeepLook(none, 41, &fell, &back), "an unwatched clock is not touched");
        Check(KeepOther(222, -5).baseline == 0, "a failed first report: unwatched");
    }
    {   // ARM: only where the game asked and the report meant something.
        Fake f;
        f.getArm = 444;
        Result r = Apply(f, nullptr, nullptr);
        Check(KeepArm(r).baseline == 444 && KeepArm(r).request == 444, "plain 444 is kept");
        Fake p;
        p.measured[0] = p.measured[1] = 498; p.getArm = 333;
        r = Apply(p, nullptr, nullptr);
        Check(KeepArm(r).baseline == 0, "a plugin's 500 (no request, report 333) is never asked over");
        Fake m;
        m.getArm = 333;
        r = Apply(m, nullptr, nullptr);
        Check(KeepArm(r).baseline == 0, "a report that did not move to 444: the report says nothing");
        Fake e;
        e.getArm = 333;
        r = Apply(e, "333", nullptr);
        Check(KeepArm(r).baseline == 0, "an explicit low clock is under the report floor: not watched");
        Fake x;
        x.armRc[0] = kRefused; x.getArm = 444;
        r = Apply(x, nullptr, "max");
        Check(KeepArm(r).request == 444, "after a refused 500 it is the 444 that is kept, never the 500");
    }
    Check(KeepOn(nullptr) && KeepOn("1") && !KeepOn("0"), "KEEP is on unless 0");
    // MSC_VITA_CLOCK_DEFER (Galaxy r99 GXV_CLOCK_DEFER)
    Check(DeferMs(nullptr, nullptr, "max") == 1000 && DeferMs("1", nullptr, "max") == 1000 && DeferMs("", nullptr, "measure") == 1000, "DEFER: 1 s by default and for 1");
    Check(DeferMs("0", nullptr, "max") == 0 && DeferMs("-5", nullptr, "max") == 0, "DEFER: 0 (or less) is the old start");
    Check(DeferMs("250", nullptr, "max") == 250 && DeferMs("99999", nullptr, "max") == kDeferMaxMs, "DEFER: a count of ms, capped");
    Check(DeferMs(nullptr, "444", "max") == 0 && DeferMs("", "333", "measure") == 0, "DEFER: an explicit MSC_VITA_CPU_MHZ measures nothing and is not deferred unless MSC_VITA_CLOCK_DEFER is set");
    Check(DeferMs(nullptr, nullptr, "fixed") == 0, "DEFER: the fixed policy is not deferred");
    Check(DeferMs(nullptr, "500", "max") == 1000, "DEFER: a value MSC_VITA_CPU_MHZ ignores does not stop it");
    Check(DeferWaitUs(0, 1000) == 1000000ull && DeferWaitUs(400000, 1000) == 600000ull && DeferWaitUs(1000000, 1000) == 0 && DeferWaitUs(5000000, 1000) == 0, "DEFER: the wait is what is left of the delay");
    Check(DeferWaitUs(123, 0) == 0, "DEFER: no delay, no wait");
    {
        // a deferred start is the same requests: Apply does not know when it runs (a plugin's 500 at 1 s: no ARM request at all)
        Fake early, late;
        early.measured[0] = early.measured[1] = 333; early.armRc[0] = kRefused; early.getArm = 444;
        late.measured[0] = late.measured[1] = 498; late.armRc[0] = kRefused; late.getArm = 333;
        const Result re = Apply(early, nullptr, "max"), rl = Apply(late, nullptr, "max");
        Check(re.fallbackTried && early.ArmRequests(444) == 1, "DEFER: measured before the profile holds, a refused 500 brings 444 (what deferring avoids)");
        Check(!rl.fallbackTried && late.ArmRequests(444) == 0, "DEFER: measured once the profile holds, no 444 over its 500");
    }
    // clock17: the decision under the Vita default (max, deferred). Measured below 470 -> 500; refused -> 444; a profile at 500 -> nothing; stock sim -> 444.
    {
        Fake f;   // no profile: measures 444, 500 taken
        const Result r = Apply(f, nullptr, "max");
        Check(r.armRequest == 500 && f.calls[0] == "arm500" && f.AllArmRequests() == 1 && !r.fallbackTried && f.Rest(), "C17: measured below 470: 500 asked, taken, no 444");
        Fake g; g.armRc[0] = kRefused;
        const Result q = Apply(g, nullptr, "max");
        Check(q.fallbackTried && g.calls[0] == "arm500" && g.calls[1] == "arm444" && g.AllArmRequests() == 2, "C17: 500 refused: then 444");
        Fake h; h.measured[0] = h.measured[1] = 498;
        const Result p = Apply(h, nullptr, "max");
        Check(p.armRequest == 0 && h.AllArmRequests() == 0 && h.Rest(), "C17: a profile at ~500: no ARM request, bus/GPU/xbar still asked");
        Fake e; e.measured[0] = kPluginFloorMHz - 1;
        Check(Apply(e, nullptr, "max").armRequest == 500, "C17: 469 asks for 500");
        Fake d; d.measured[0] = kPluginFloorMHz;
        Check(Apply(d, nullptr, "max").armRequest == 0, "C17: 470 is a profile");
        Check(f.Rest() && r.gpuRequest == 222 && r.busRequest == 222 && r.xbarRequest == 166, "C17: GPU 222, bus 222, xbar 166 (the maxima)");
    }
    {   // stock sim
        Fake f; f.measured[0] = f.measured[1] = 498;   // a profile at 500 is asked over, exactly 444
        const Result r = Apply(f, nullptr, "max", nullptr, nullptr, nullptr, true);
        Check(r.stockSim && r.armRequest == 444 && f.AllArmRequests() == 1 && f.ArmRequests(444) == 1 && f.Rest(), "C17: stock sim: exactly one 444 over a profile");
        Check(r.measuredBefore == 498, "C17: stock sim still measures for the line");
        Check(!WatchUseful(r) && !NeedsRestore(r, 100), "C17: stock sim is not watched");
        char line[400];
        Format(r, line, sizeof line);
        Check(std::string(line).find("MSC_VITA_STOCK_SIM") != std::string::npos && std::strlen(line) < 399, "C17: the line says stock sim");
        Fake g; g.armRc[0] = kRefused;
        const Result q = Apply(g, nullptr, "max", nullptr, nullptr, nullptr, true);
        Check(!q.fallbackTried && g.AllArmRequests() == 1 && g.ArmRequests(500) == 0, "C17: stock sim never asks for 500");
        Fake h;
        const Result e = Apply(h, "333", "max", nullptr, nullptr, nullptr, true);
        Check(!e.stockSim && e.armRequest == 333, "C17: MSC_VITA_CPU_MHZ wins over stock sim");
        Check(StockSimOn("1") && !StockSimOn("0") && !StockSimOn("") && !StockSimOn(nullptr) && !StockSimOn("2"), "C17: only 1 turns stock sim on");
        Check(DeferMs(nullptr, nullptr, "max") == 1000, "C17: stock sim keeps the 1 s deferral (DeferMs does not look at it)");
    }
    {   // the watch: 500 first, then 444
        Result held; held.policy = Policy::kMax; held.armRequest = 0; held.reportedArm = 500;
        Check(WatchUseful(held) && NeedsRestore(held, 333), "C17: a plugin at 500 that lets go is seen");
        Fake f;
        Restore a = RestoreArm(f, held);
        Check(a.first == 500 && a.second == 0 && f.calls.size() == 1 && f.calls[0] == "arm500", "C17: restore asks 500 first, once if taken");
        Fake g; g.armRc[0] = kRefused;
        Restore b = RestoreArm(g, held);
        Check(b.first == 500 && b.second == 444 && g.calls.size() == 2 && g.calls[1] == "arm444", "C17: restore: 500 refused, then 444");
        Result m = held; m.policy = Policy::kMeasure;
        Fake h;
        Restore c = RestoreArm(h, m);
        Check(c.first == 444 && c.second == 0 && h.calls.size() == 1, "C17: the measure policy still restores 444 only");
    }
    {   // clockfix17 F2: an explicit MSC_VITA_CLOCK_DEFER is honoured with MSC_VITA_CPU_MHZ (the 444 A/B lands after a profile); fixed is never deferred
        Check(DeferMs("1", "444", "max") == 1000, "F2: CPU_MHZ=444 with DEFER=1 is asked 1 s into the process");
        Check(DeferMs("250", "444", "max") == 250 && DeferMs("99999", "444", "max") == kDeferMaxMs, "F2: an explicit count of ms with CPU_MHZ is kept, capped");
        Check(DeferMs("0", "444", "max") == 0 && DeferMs("", "444", "max") == 0 && DeferMs(nullptr, "444", "max") == 0, "F2: CPU_MHZ alone, or DEFER=0, asks at the start");
        Check(DeferMs("1", "444", "fixed") == 0 && DeferMs("1", nullptr, "fixed") == 0, "F2: the fixed policy is never deferred");
        Fake f;   // a deferred explicit 444 is still exactly that one request, measuring nothing before
        const Result r = Apply(f, "444", "max");
        Check(r.armRequest == 444 && f.AllArmRequests() == 1 && r.measuredBefore == 0, "F2: the deferred 444 is exactly one request");
    }
    {   // clockfix17 F3: a 500 the game asked for and got is kept
        Fake f; f.getArm = 500;
        Result r = Apply(f, nullptr, "max");
        KeepClock k = KeepArm(r);
        Check(r.armRequest == 500 && k.baseline == 500 && k.request == 500, "F3: a taken 500 reporting 500 is kept");
        Fake g; g.getArm = 494;
        k = KeepArm(Apply(g, nullptr, "max"));
        Check(k.baseline == 494 && k.request == 500, "F3: a taken 500 reporting 494 is kept with baseline 494");
        Fake h; h.getArm = 333;
        Check(KeepArm(Apply(h, nullptr, "max")).baseline == 0, "F3: 500 answered 0 with a report of 333 (a lock) is not kept");
        Fake i; i.getArm = 443;
        Check(KeepArm(Apply(i, nullptr, "max")).baseline == 0, "F3: a report under 444 is not kept");
        Fake j; j.getArm = 500; j.armRc[0] = kRefused; j.armRc[1] = 0;
        k = KeepArm(Apply(j, nullptr, "max"));
        Check(k.baseline == 444 && k.request == 444, "F3: a refused 500 keeps the 444, as before");
        Fake m; m.getArm = 500;
        Check(KeepArm(Apply(m, nullptr, "measure")).request == 444, "F3: the measure policy never asked 500: what is kept is its 444");
        Fake n; n.getArm = 500;
        bool fell, back;
        KeepClock c = KeepArm(Apply(n, nullptr, "max"));
        Check(KeepLook(c, 444, &fell, &back) && fell && c.tries == 1, "F3: a fall from the kept 500 asks again");
        Result held = Apply(n, nullptr, "max");
        Fake q; q.armRc[0] = kRefused;
        const Restore rr = RestoreArm(q, held);
        Check(rr.first == 500 && rr.second == 444, "F3: the keep path asks 500 and falls to 444 (RestoreArm)");
    }
    {   // clockfix17 F4: the stock sim forces the three core keys, in order
        std::vector<std::string> set;
        auto record = [&set](const char* k, const char* v) { set.push_back(std::string(k) + "=" + v); };
        Check(!ForceFourthCoreOff("0", record) && !ForceFourthCoreOff(nullptr, record) && set.empty(), "F4: no sim, no key forced");
        Check(ForceFourthCoreOff("1", record) && set.size() == 3 && set[0] == "MSC_VITA_CORE3=0" && set[1] == "MSC_VITA_CORE3_SEND=0" && set[2] == "MSC_VITA_CORE3_SHADERS=0", "F4: the sim forces the three keys to 0");
    }
    {   // clockfix17 R1: the measured watch
        Fake p; p.measured[0] = p.measured[1] = 498; p.getArm = 333;
        const Result r = Apply(p, nullptr, "max");
        Check(r.armRequest == 0 && MeasuredWatchUseful(r), "R1: nothing asked and the getter under 400: the measured watch runs");
        Fake q; q.measured[0] = q.measured[1] = 498; q.getArm = 500;
        Check(!MeasuredWatchUseful(Apply(q, nullptr, "max")) && WatchUseful(Apply(q, nullptr, "max")), "R1: a getter at 500 is the old watch's, not this one");
        Fake s; s.getArm = 333;
        Check(!MeasuredWatchUseful(Apply(s, nullptr, "max")), "R1: a request was made: not watched");
        Fake t; t.measured[0] = t.measured[1] = 498; t.getArm = 333;
        Check(!MeasuredWatchUseful(Apply(t, "333", "max")) && !MeasuredWatchUseful(Apply(t, nullptr, "max", nullptr, nullptr, nullptr, true)) && !MeasuredWatchUseful(Apply(t, nullptr, "fixed")), "R1: explicit, stock sim, fixed: not watched");
        MeasuredWatch w;
        Check(!MeasuredLook(w, 498) && !MeasuredLook(w, 420) && !MeasuredLook(w, 498) && !MeasuredLook(w, 420), "R1: one low reading, or low readings with a good one between, ask nothing");
        w = MeasuredWatch();
        Check(MeasuredLook(w, 400) == false && MeasuredLook(w, 400) == true && w.restores == 1, "R1: two low readings in a row ask once");
        Check(!MeasuredLook(w, 0) && !MeasuredLook(w, -1) && w.lowRun == 0, "R1: a reading that did not measure counts for nothing");
        Check(!MeasuredLook(w, 429) && MeasuredLook(w, 429) && !MeasuredLook(w, 430), "R1: 429 is low, 430 is not");
        MeasuredWatch cap;
        int asks = 0;
        for (int i = 0; i < 40; ++i) asks += MeasuredLook(cap, 333) ? 1 : 0;
        Check(asks == kMeasureRestores && asks == 3, "R1: at most 3 requests a run");
        Check(MeasuredWatchOn(nullptr) && MeasuredWatchOn("1") && !MeasuredWatchOn("0"), "R1: MWATCH is on unless 0");
        Check(kMeasureEveryS == 30 && kMeasureLowMHz == 430, "R1: every 30 s, under 430");
    }
    std::printf("%s: %d checks, %d failed\n", g_failures ? "FAILED" : "clock policy test passed", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
