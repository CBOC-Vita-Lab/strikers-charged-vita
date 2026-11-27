// np_vita_clock.h: which ARM clock the game asks for at start-up (1.3). Pure: the power API and the calibrated loop come in as an object, so the game
// (np_vita_main.cpp SetUpCpu) and the unit test (native/platform/vita/tests/test_clock_policy.cpp, a fake power API) run the same code.
//
// The Galaxy port's measured case (r28-r31, same console): a PSVshellPlus profile held the core at a real 498 MHz, the system reported 333, and the game's own 500
// request came back 0x802B0000. So neither a setter's result nor a getter says what the core runs at; the loop does, and a kernel plugin can stop the core being at 500
// if the game asks for 444 over it. The rules:
//   MSC_VITA_CPU_MHZ=111|166|222|333|444  as in 1.2 and before: exactly that request, whatever is measured (std::atoi, so "333x" counts; other values count as unset).
//   MSC_VITA_CLOCK_POLICY=measure (the policy of an unset or unknown value in this header; the Vita build passes "max" when unset)  the core is measured first. kPluginFloorMHz or more: a plugin is giving about 500, no ARM request at all. Less: 444 as in 1.2.
//   MSC_VITA_CLOCK_POLICY=fixed    1.2 exactly: 444, nothing measured.
//   (The Vita build passes "max" when the variable is unset, as the Galaxy port does.)
//   MSC_VITA_CLOCK_POLICY=max      (the Vita build's default, as Galaxy's GXV_CLOCK_DEFER path) measured first. kPluginFloorMHz or more: a plugin holds about 500, no ARM request at
//                                  all (clock17: 1.3 asked for 500 over a profile too; Galaxy r99 found that a request overlapping the plugin can lock the core at 444). Less:
//                                  one ordinary 500 request; negative: one 444 request. Result 0: no fallback, whatever the getter says afterwards (an owner lock answers 0
//                                  without changing anything: such a request is "unverified", not refused).
//   MSC_VITA_STOCK_SIM=1 (clock17, Galaxy's GXV_STOCK_SIM, test only, default off)  exactly one 444 request whatever the core measures, and the fourth core off (MSC_VITA_CORE3=0,
//                                  _SEND=0): a stock console with no clock or core plug-in, for an A/B on a console that has them. Still deferred as the policy is.
//   Clock speed cannot change a result: the game steps at a fixed 50 Hz and nothing in a step reads the clock; only how many steps fit in a frame's time follows it.
//   MSC_VITA_GPU_MHZ=111|166|222, MSC_VITA_BUS_MHZ=111|166|222, MSC_VITA_XBAR_MHZ=111|166  (1.3, console A/B only) a LOWER request for that clock, never a higher one: how much of a
//                                  frame's GPU time follows each clock is what says whether the GPU's core or its memory paths are the limit. 222/222/166 are the top of every table:
//                                  VitaSDK's power headers (FW 3.60: GPU 1-222 quantised to 41/55/83/111/166/222, xbar 1-166, bus 1-222), and PSVshell's own lists stop there as well.
//   MSC_VITA_CLOCK_MWATCH=1 (default, 0 off; clockfix17) when nothing was asked (the core already measured 470 or more) and the getter reports under 400 (a plugin's profile), the core is measured every 30 s on
//                                  the lowest-priority thread; two readings under 430 in a row ask 500 (then 444), at most 3 times a run. Rules at MeasuredLook below.
//   MSC_VITA_CLOCK_KEEP=1 (default), 0 off  (1.3) a low-priority look every 2 s at what the power API reports against what it reported at the start; a clock that fell (the only cause in
//                                  sight is a suspend or a system overlay) is asked for once more and the event goes to the basic record. Rules at KeepLook below.
// Not done, on purpose: retries, a request on resume from a callback, plugin profile or lock conventions, SYS as a CPU route, private exports. The 222/222/166 requests are always made,
// each on its own: the result of one never decides another. The line says what was requested, returned, reported and measured, each kept apart.
#ifndef NP_VITA_CLOCK_H
#define NP_VITA_CLOCK_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace NpVitaClock {

constexpr int kPluginFloorMHz = 470;   // between 444 and 500; Galaxy's figure (its loop read 497-498 under the profile, 440-444 without it)
constexpr int kDefaultMHz = 444, kMaxMHz = 500, kBusMHz = 222, kGpuMHz = 222, kXbarMHz = 166;

enum class Policy { kMeasure, kFixed, kMax };

inline Policy ParsePolicy(const char* text) {
    if (text && std::strcmp(text, "fixed") == 0) return Policy::kFixed;
    if (text && std::strcmp(text, "max") == 0) return Policy::kMax;
    return Policy::kMeasure;
}
// The value of MSC_VITA_GPU_MHZ / _BUS_MHZ / _XBAR_MHZ that counts, else the top. (std::atoi as MSC_VITA_CPU_MHZ. Only values under the top: nothing here can ask for more.)
inline int LowerMHz(const char* text, int top, int a, int b) {
    if (!text) return top;
    const int v = std::atoi(text);
    return v == a || v == b ? v : top;
}
inline const char* PolicyName(Policy p) { return p == Policy::kFixed ? "fixed" : p == Policy::kMax ? "max" : "measure"; }

// The value of MSC_VITA_CPU_MHZ that counts, 0 when it does not (unset, or a value 1.2 ignored as well).
inline int ExplicitMHz(const char* text) {
    if (!text) return 0;
    const int v = std::atoi(text);
    return v == 111 || v == 166 || v == 222 || v == 333 || v == 444 ? v : 0;
}

struct Result {
    Policy policy = Policy::kMeasure;
    bool explicitValue = false;   // MSC_VITA_CPU_MHZ decided
    bool stockSim = false;        // MSC_VITA_STOCK_SIM decided (exactly 444, whatever is measured)
    int armRequest = 0;           // 0: no ARM request was made
    int armRc = 0;
    bool fallbackTried = false;   // the one 444 after a refused 500
    int fallbackRc = 0;
    int busRc = 0, gpuRc = 0, xbarRc = 0;
    int busRequest = kBusMHz, gpuRequest = kGpuMHz, xbarRequest = kXbarMHz;
    int reportedArm = 0, reportedBus = 0, reportedGpu = 0, reportedXbar = 0;   // what the getters said (a negative value is a failed getter)
    int measuredBefore = 0, measuredAfter = 0;                                 // the calibrated loop, 0: not measured
};

// Api: int SetArm(int), SetBus(int), SetGpu(int), SetXbar(int), GetArm(), GetBus(), GetGpu(), GetXbar(); int MeasureMHz().
template <class Api>
Result Apply(Api& api, const char* cpuMhz, const char* policy, const char* gpuMhz = nullptr, const char* busMhz = nullptr, const char* xbarMhz = nullptr, bool stockSim = false) {
    Result r;
    r.gpuRequest = LowerMHz(gpuMhz, kGpuMHz, 111, 166);
    r.busRequest = LowerMHz(busMhz, kBusMHz, 111, 166);
    r.xbarRequest = LowerMHz(xbarMhz, kXbarMHz, 111, 111);
    r.policy = ParsePolicy(policy);
    const int chosen = ExplicitMHz(cpuMhz);
    r.explicitValue = chosen != 0;
    r.stockSim = stockSim && !r.explicitValue;   // (an explicit lower clock is the more specific A/B)
    const bool measure = !r.explicitValue && r.policy != Policy::kFixed;
    if (measure) r.measuredBefore = api.MeasureMHz();
    const bool pluginClock = measure && !r.stockSim && r.measuredBefore >= kPluginFloorMHz;
    if (r.explicitValue) {
        r.armRequest = chosen;
        r.armRc = api.SetArm(chosen);
    } else if (r.stockSim) {
        r.armRequest = kDefaultMHz;
        r.armRc = api.SetArm(kDefaultMHz);
    } else if (r.policy == Policy::kMax && !pluginClock) {
        r.armRequest = kMaxMHz;
        r.armRc = api.SetArm(kMaxMHz);
        if (r.armRc < 0) {
            r.fallbackTried = true;
            r.fallbackRc = api.SetArm(kDefaultMHz);
        }
    } else if (!pluginClock) {
        r.armRequest = kDefaultMHz;
        r.armRc = api.SetArm(kDefaultMHz);
    }
    r.busRc = api.SetBus(r.busRequest);
    r.gpuRc = api.SetGpu(r.gpuRequest);
    r.xbarRc = api.SetXbar(r.xbarRequest);
    r.reportedArm = api.GetArm();
    r.reportedBus = api.GetBus();
    r.reportedGpu = api.GetGpu();
    r.reportedXbar = api.GetXbar();
    if (r.policy != Policy::kFixed) r.measuredAfter = api.MeasureMHz();
    return r;
}

// After a start that left the clock to a plugin (no ARM request): the plugin can stop holding it (profile switched off, a resume that does not reapply it), and the
// core then falls to the system's default, below what 1.2 asked for. The power API is read, not the loop: cheap, and off the game's core. But the getter only says
// something when it moved: under a PSVshellPlus profile on this console it read 333 with the loop at 497-498 (the Galaxy port's logs, and the top of this file), so a low
// reading alone is a plugin at work, and a 444 request then would be the very thing the policy exists to avoid. So: only a fall from a report of 400 or more at the start.
// A plugin that was reported low from the start and lets go is not seen; the clock then stays where the system puts it, as it would in the Galaxy port.
constexpr int kReportedFloorMHz = 400;
inline bool WatchUseful(const Result& r) {
    const bool leftToPlugin = r.armRequest == 0;
    return !r.explicitValue && !r.stockSim && leftToPlugin && r.policy != Policy::kFixed && r.reportedArm >= kReportedFloorMHz;
}
// (clockfix17, R1) The same case when the getter is already low at the start (the PSVshellPlus profile on the owner's console: reported 333, core at 498): the report says nothing, so
// the loop is read instead. Every kMeasureEveryS s (the caller's thread sleeps; one MeasureMHz is the existing ~12 ms spin), two readings under kMeasureLowMHz in a row, then one
// RestoreArm (500 then 444), at most kMeasureRestores times a run. A reading cut short by a busier core only reads low and costs one needless request: a plugin's lock answers 0 and
// changes nothing. The caller's thread runs at the lowest user priority, below every game, render and audio thread (np_vita_main.cpp).
constexpr int kMeasureLowMHz = 430, kMeasureRestores = 3, kMeasureEveryS = 30;
inline bool MeasuredWatchUseful(const Result& r) {
    return !r.explicitValue && !r.stockSim && r.armRequest == 0 && r.policy != Policy::kFixed && r.reportedArm < kReportedFloorMHz;
}
struct MeasuredWatch {
    int lowRun = 0;     // low readings in a row
    int restores = 0;   // requests made so far
};
// One reading (mhz <= 0: nothing measured). True: ask now.
inline bool MeasuredLook(MeasuredWatch& w, int mhz) {
    if (mhz <= 0) return false;
    if (mhz >= kMeasureLowMHz) { w.lowRun = 0; return false; }
    if (++w.lowRun < 2) return false;
    w.lowRun = 0;
    if (w.restores >= kMeasureRestores) return false;
    ++w.restores;
    return true;
}
inline bool MeasuredWatchOn(const char* text) { return !text || std::strcmp(text, "0") != 0; }
inline bool NeedsRestore(const Result& r, int reportedArm) {
    return WatchUseful(r) && reportedArm > 0 && reportedArm < kReportedFloorMHz;
}
// What the watch asks for when the plugin let go (clock17): under the max policy 500 first (the owner's clock, as at start), then 444 if that is refused; otherwise 444 as before.
// Api: int SetArm(int). Returns the first request; *second is 0 when no second request was made.
struct Restore { int first = 0, firstRc = 0, second = 0, secondRc = 0; };
template <class Api>
Restore RestoreArm(Api& api, const Result& r) {
    Restore o;
    if (r.policy == Policy::kMax) {
        o.first = kMaxMHz;
        o.firstRc = api.SetArm(kMaxMHz);
        if (o.firstRc >= 0) return o;
        o.second = kDefaultMHz;
        o.secondRc = api.SetArm(kDefaultMHz);
        return o;
    }
    o.first = kDefaultMHz;
    o.firstRc = api.SetArm(kDefaultMHz);
    return o;
}
// MSC_VITA_STOCK_SIM=1: a stock console. (Everything but "1" is off.)
inline bool StockSimOn(const char* text) { return text && std::strcmp(text, "1") == 0; }
// The three keys a stock console does without (clockfix17): set(name, value) for each, in the order the plan reads them. Under the stock sim these must be forced BEFORE the plan
// places the mixer and the file workers (vita_plan.cpp PlaceAudioAndFiles also returns early), or they stay on the fourth core. True when the sim is on.
template <class SetEnv>
bool ForceFourthCoreOff(const char* stockText, SetEnv&& set) {
    if (!StockSimOn(stockText)) return false;
    set("MSC_VITA_CORE3", "0");
    set("MSC_VITA_CORE3_SEND", "0");
    set("MSC_VITA_CORE3_SHADERS", "0");
    return true;
}

// MSC_VITA_CLOCK_KEEP. Whether a stock system gives a process its requests back after a suspend is not established: ScePower keeps them per process (PSVshell's oc.c reads them
// from its process storage) and the kernel header speaks of "active processes", but no source says so outright, and PSVshellPlus 1.1 fixed a clock reset of its own after suspend.
// So this looks. Each clock is compared with the report taken right after the start-up requests; that is the baseline whatever set it, so a plugin holding a lower clock is no
// fall and is left alone. A fall brings one request for what was asked at the start. A plugin's lock answers 0 and changes nothing, so a clock that stays down gets at most
// kKeepTries requests, then is left alone until it recovers by itself; a recovered clock arms the rule again. The ARM clock is watched only when the game asked for it and it
// reported at least kReportedFloorMHz at the start: under a plugin that stops the report saying anything (top of this file; the plugin's own watch is in np_vita_main.cpp).
constexpr int kKeepTries = 3;
struct KeepClock {
    int baseline = 0;   // 0: not watched
    int request = 0;
    int tries = 0;
};
// One look: reported is the getter's answer now. True: request `request` again. fellNote / backNote say what to write down (once per fall, once per return).
inline bool KeepLook(KeepClock& c, int reported, bool* fellNote, bool* backNote) {
    *fellNote = *backNote = false;
    if (c.baseline <= 0 || reported <= 0) return false;   // (a failed getter says nothing)
    if (reported >= c.baseline) {
        if (c.tries) *backNote = true;
        c.tries = 0;
        return false;
    }
    if (c.tries == 0) *fellNote = true;
    if (c.tries >= kKeepTries) return false;
    ++c.tries;
    return true;
}
inline bool KeepOn(const char* text) { return !text || std::strcmp(text, "0") != 0; }
inline KeepClock KeepArm(const Result& r) {
    KeepClock c;
    if (r.fallbackTried && r.fallbackRc >= 0 && r.reportedArm >= kDefaultMHz) { c.baseline = kDefaultMHz; c.request = kDefaultMHz; return c; }   // (500 refused, 444 taken: the usual start of the Vita's default policy)
    // (clockfix17) a 500 the game asked for and got is kept too (RestoreArm asks 500 and falls to 444); the getter may read 494, so the baseline is what it said
    if (r.policy == Policy::kMax && r.armRequest == kMaxMHz && r.armRc >= 0 && !r.fallbackTried && r.reportedArm >= kDefaultMHz) { c.baseline = r.reportedArm < kMaxMHz ? r.reportedArm : kMaxMHz; c.request = kMaxMHz; return c; }
    if (r.armRequest > 0 && r.armRequest <= kDefaultMHz && r.armRc >= 0 && !r.fallbackTried && r.reportedArm >= kReportedFloorMHz && r.reportedArm >= r.armRequest) { c.baseline = r.armRequest; c.request = r.armRequest; }
    return c;
}
inline KeepClock KeepOther(int request, int reported) {
    KeepClock c;
    if (reported > 0) { c.baseline = reported < request ? reported : request; c.request = request; }
    return c;
}

// MSC_VITA_CLOCK_DEFER (1.3, Galaxy r99 GXV_CLOCK_DEFER): the start-up clock requests (Apply above, with its measurement) are made by a thread of their own once the
// process has run for this long, not at the very start. The Galaxy port's console (same Vita, same PSVshellPlus profile) measured the core before the plugin's profile
// had taken hold of the new process: the measurement read the system's clock, so the policy asked for 500 (refused) and then 444, over the profile's 500 still to come.
// Unset or 1: 1000 ms (Galaxy's "1 s into the process"); 0: at the start, as before; 2..5000: that many ms. The "fixed" policy is never deferred. An explicit MSC_VITA_CPU_MHZ
// measures nothing and is not deferred by default, but an explicit MSC_VITA_CLOCK_DEFER with it is honoured (clockfix17: the 444 A/B must land after a PSVshellPlus profile, which
// is applied 0.25-0.45 s into the process and answers 0 to a request before it).
constexpr long kDeferDefaultMs = 1000, kDeferMaxMs = 5000;
inline long DeferMs(const char* text, const char* cpuMhz, const char* policy) {
    if (ParsePolicy(policy) == Policy::kFixed) return 0;
    if (!text || !*text) return ExplicitMHz(cpuMhz) != 0 ? 0 : kDeferDefaultMs;
    const long v = std::atol(text);
    if (v <= 0) return 0;
#ifdef NP_CLOCK_DEFER_BREAK
    if (v == 1) return 1;   // (the deliberate break of test_clock_policy.cpp: "1" taken as 1 ms)
#endif
    if (v == 1) return kDeferDefaultMs;
    return v > kDeferMaxMs ? kDeferMaxMs : v;
}
// How long the deferring thread still waits, given the process time now (us). 0: at once.
inline unsigned long long DeferWaitUs(unsigned long long processUs, long deferMs) {
    const unsigned long long due = static_cast<unsigned long long>(deferMs > 0 ? deferMs : 0) * 1000ull;
    return processUs >= due ? 0ull : due - processUs;
}

// The "clocks:" line of the basic record.
inline int Format(const Result& r, char* out, size_t size) {
    char arm[96];
    if (!r.armRequest) std::snprintf(arm, sizeof(arm), "none (the core already measures %d MHz)", r.measuredBefore);
    else if (r.fallbackTried) std::snprintf(arm, sizeof(arm), "%d rc 0x%X, then 444 rc 0x%X", r.armRequest, static_cast<unsigned>(r.armRc), static_cast<unsigned>(r.fallbackRc));
    else std::snprintf(arm, sizeof(arm), "%d rc 0x%X", r.armRequest, static_cast<unsigned>(r.armRc));
    return std::snprintf(out, size, "clocks: policy %s%s; requested ARM %s, bus %d rc 0x%X, GPU %d rc 0x%X, xbar %d rc 0x%X; reported ARM %d bus %d GPU %d xbar %d; measured %d before, %d after (MHz)",
                         PolicyName(r.policy), r.explicitValue ? " (MSC_VITA_CPU_MHZ)" : r.stockSim ? " (MSC_VITA_STOCK_SIM: stock console, 4th core off)" : "", arm, r.busRequest, static_cast<unsigned>(r.busRc), r.gpuRequest, static_cast<unsigned>(r.gpuRc), r.xbarRequest,
                         static_cast<unsigned>(r.xbarRc), r.reportedArm, r.reportedBus, r.reportedGpu, r.reportedXbar, r.measuredBefore, r.measuredAfter);
}

} // namespace NpVitaClock

#endif // NP_VITA_CLOCK_H
