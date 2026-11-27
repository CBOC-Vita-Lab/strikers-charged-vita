// np_vita_core3.h: the rules for moving work off the record player's core (1.3). Pure: the kernel calls come in as an object, so the game (vita_threads.cpp, vita_plan.cpp) and
// the unit test (native/platform/vita/tests/test_core3.cpp, a fake kernel) run the same code.
//
// Fourth core. MSC_VITA_CORE3=1 (default off) moves audio output, the mixer, the disc and save workers and the log writer to the system core. An application may use it only with
// the owner's kernel plug-in CapUnlocker (mask SCE_KERNEL_CPU_MASK_SYSTEM, 0x80000); the game does not depend on it, install it or look for it. Instead a small thread has to
// prove the core: it is created with the mask (or, if that is refused, moved to it), and it must then run, finish in time, and report CPU 3 and the mask. Anything else is
// "not available", and nothing moves. The Galaxy port's log on this console without the plug-in: change rc 0x80028025, mask read back 0x0.
//
// Collector and mixer. MSC_VITA_CORE_GC=<cores> places vitaGL's collector (vita_threads.cpp). With it the mixer goes along, unless NP_VITA_CORE_AUDIO is named, but only where
// that is off the player's core and off the game's: in plans C and Cn the player has core 2 and the GL back end core 1; in plans A and S the player and the back end share a
// thread on core 1, so "core 1" there would pile the audio onto the render work.
#ifndef NP_VITA_CORE3_H
#define NP_VITA_CORE3_H

#include <cstdlib>
#include <cstring>

namespace NpVitaCore3 {

constexpr int kSystemMask = 0x00080000;    // SCE_KERNEL_CPU_MASK_SYSTEM
constexpr int kProbeStartMask = 0x00040000;   // the route that does not name core 3 at creation: a thread on core 2, then moved
constexpr unsigned kProbeTimeoutUs = 200 * 1000;   // the probe does nothing but report: a thread that is not done in 200 ms has no usable core

inline bool Wanted(const char* text) { return text && std::strcmp(text, "1") == 0; }

struct Result {
    bool usable = false;
    bool transient = false;   // (1.3) an answer that a moment's load on the system core can give: MSC_VITA_CORE3_RETRY asks again
    const char* why = "not tried";
    int createRc = 0, changeRc = 0, startRc = 0, waitRc = 0, cpu = -1, mask = 0;
};

// Api: int Create(int mask) (a thread id, negative when refused; the thread does not run yet), int Change(int id, int mask), int Start(int id), int Wait(int id, unsigned timeoutUs)
// (0 when the thread ended), bool Reported(), int Cpu(), int Mask() (what the thread itself saw), void Delete(int id).
// Fails closed: every way out but the last says not usable.
template <class Api>
Result Probe(Api& api, unsigned timeoutUs = kProbeTimeoutUs) {
    Result r;
    int id = r.createRc = api.Create(kSystemMask);
    if (id < 0) {
        id = api.Create(kProbeStartMask);
        if (id < 0) { r.why = "no thread could be made"; return r; }
        r.changeRc = api.Change(id, kSystemMask);
        if (r.changeRc < 0) {
            api.Delete(id);
            r.why = "the system refused the mask";
            return r;
        }
    }
    r.startRc = api.Start(id);
    if (r.startRc < 0) {
        api.Delete(id);
        r.why = "the thread did not start";
        return r;
    }
    r.waitRc = api.Wait(id, timeoutUs);
    if (r.waitRc < 0) {
        api.Delete(id);   // (may be refused if it is running after all; it ends by itself, and touches only static data)
        r.why = "the thread did not finish in time";
        r.transient = true;
        return r;
    }
    if (!api.Reported()) {
        api.Delete(id);
        r.why = "the thread did not report";
        r.transient = true;
        return r;
    }
    r.cpu = api.Cpu();
    r.mask = api.Mask();
    api.Delete(id);
    if (r.cpu != 3) { r.why = "the thread ran on another core"; r.transient = (r.mask & kSystemMask) != 0; return r; }
    if (!(r.mask & kSystemMask)) { r.why = "the thread's mask does not have the system core"; return r; }
    r.usable = true;
    r.why = "ok";
    return r;
}

// (1.3) MSC_VITA_CORE3_RETRY=<n> (default 2; 0 = one try as before): the Galaxy port's r83 lesson (its probe once measured a 3 % share and the run went on three cores).
// Here the probe is a yes/no, and one answer decides the whole run: a probe thread that is not scheduled on the system core within 200 ms (the system's own work
// there at boot) said "not available" although the plug-in is installed. Only the answers that can be a moment's load are asked again: did not finish in
// time, did not report, ran on another core. A refused mask, no thread, a start refused or a mask read back without the system core are final (the console
// without the plug-in answers the first try with a refusal: no retry, no delay). Each retry waits `pauseUs`, then gives the thread twice the time (at most 400 ms):
// the default two retries cost at most 1.1 s at boot, and only on a console where the first answer may have been the load.
// Api as Probe, plus void Again() (a fresh report: the earlier thread may still write the old one) and void Pause(unsigned us).
inline bool Retryable(const Result& r) {
#ifdef NP_CORE3_RETRY_BREAK
    return false;   // (the deliberate break of the unit test)
#endif
    return !r.usable && r.transient;
}
template <class Api>
Result ProbeRetry(Api& api, int retries, int* tries = nullptr, unsigned pauseUs = 50 * 1000) {
    unsigned timeout = kProbeTimeoutUs;
    Result r = Probe(api, timeout);
    int n = 1;
    while (n <= retries && Retryable(r)) {
        api.Pause(pauseUs);
        api.Again();
        timeout = timeout * 2 > 400u * 1000u ? 400u * 1000u : timeout * 2;
        r = Probe(api, timeout);
        ++n;
    }
    if (tries) *tries = n;
    return r;
}

// The value for NP_VITA_CORE_AUDIO that follows MSC_VITA_CORE_GC, or nullptr when the mixer stays. planLevel: VitaPlan's, 0 C, 1 Cn, 2 A, 3 S, -1 threads set by hand.
inline const char* MixerFollows(const char* gc, int planLevel) {
    if (!gc || !*gc || planLevel > 1) return nullptr;
    for (const char* c = gc; *c; ++c)
        if (*c != '1' && *c != '2') return nullptr;   // "any", or anything naming core 0, is not a place for the mixer
    return std::strcmp(gc, "2") == 0 ? nullptr : gc;  // ("2" is where it is)
}

// (1.3) MSC_VITA_CORE3_SEND=1: the frame-send worker (the End Frame task's send of the finished frame, plan C and A) goes to the system core, the place the strikersVita port
// gives its send thread, so core 1 keeps the GL back end and the display queue. Only a proven core counts; with it unproven the plan's mask stands.
inline int SendMask(bool wanted, bool proven, int planMask) { return wanted && proven ? kSystemMask : planMask; }
// MSC_VITA_CORE3_SEND: "1" the frame-send worker; "2" that and vitaGL's collector (1.3, from the deep review: Galaxy measured a gain from taking the collector off
// its front end's core, and on Strikers' plan C the collector shares core 2 with the record player). Anything else: 0.
inline int SendLevel(const char* text) {
    if (!text) return 0;
    if (!std::strcmp(text, "1")) return 1;
    if (!std::strcmp(text, "2")) return 2;
    return 0;
}
inline int CollectorMask(int sendLevel, bool proven, int planMask) { return sendLevel >= 2 && proven ? kSystemMask : planMask; }
// A system call refused a mask with the system core in it (the plug-in went away, or a thread the system pins): the plan's mask, never "any".
inline int AfterRefusal(int mask, int planMask) { return (mask & kSystemMask) ? planMask : mask; }


// (1.3, hw2's finding) The probe runs once per process, and the first caller may be any thread: the shader compile worker asks at boot while the game thread is
// still probing. With a plain `static int state` the second caller saw the interim 0 and answered "not available" (and cached a plain thread mask for its role)
// while the probe went on to prove the core. Here the first caller claims the state (-2), the others wait until it is final (0 or 1); `run()` is the probe,
// `pause(us)` sleeps (sceKernelDelayThread; a yield on the host). A caller that finds a final state costs one atomic load. state: -1 not started, -2 running.
template <class Run, class Pause>
inline bool ProbeOnceShared(int& state, Run run, Pause pause) {
    int s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (s >= 0) return s != 0;
    int expect = -1;
    if (__atomic_compare_exchange_n(&state, &expect, -2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        const int result = run() ? 1 : 0;
        __atomic_store_n(&state, result, __ATOMIC_RELEASE);
        return result != 0;
    }
    while ((s = __atomic_load_n(&state, __ATOMIC_ACQUIRE)) < 0) pause(1000);
    return s != 0;
}

// (pace18, after the VitAdhoc finding) A thread on the system core can lose its share of that core during play. After the probe accepted the core, a watch thread asks every
// kWatchPeriodMs how much of a short busy spell it got on the core (run time / wall time of a spin on core 3); under kLowShare twice in a row (kLowChecks) the threads on the system core are
// moved back to the normal cores, the core is marked not usable for the rest of the session, one log line. One low check alone does nothing; a good check clears the count.
// Pure: the thread, the clocks and the kernel calls are vita_threads.cpp's. Watches only when the core is in use (MSC_VITA_CORE3 / _SEND), so nothing changes without it.
#ifndef NP_CORE3WATCH_BREAK
#define NP_CORE3WATCH_BREAK 0   // host test mutants (test_core3.cpp): 1 one low check moves, 2 the count is not cleared by a good check, 3 a share of exactly 50 % counts as low, 4 a check without wall time counts as low
#endif
constexpr unsigned kWatchPeriodMs = 2000;
constexpr double kLowShare = 0.5;
constexpr int kLowChecks = 2;
class Watch {
public:
    enum Result { kOk = 0, kLow = 1, kMove = 2 };
    // runUs / wallUs of one spin on the system core. Returns kMove once (latched), kLow for a low check that does not move yet, kOk otherwise.
    Result Check(double runUs, double wallUs) {
        if (lost_) return kOk;
        if (!(wallUs > 0)) {
#if NP_CORE3WATCH_BREAK == 4
            return Low();
#else
            return kOk;   // no measurement: neither good nor bad
#endif
        }
        const double share = runUs / wallUs;
#if NP_CORE3WATCH_BREAK == 3
        const bool low = share <= kLowShare;
#else
        const bool low = share < kLowShare;
#endif
        if (low) return Low();
#if NP_CORE3WATCH_BREAK != 2
        lows_ = 0;
#endif
        return kOk;
    }
    bool Lost() const { return lost_; }
    int Lows() const { return lows_; }
private:
    Result Low() {
        ++lows_;
#if NP_CORE3WATCH_BREAK == 1
        lost_ = true;
        return kMove;
#else
        if (lows_ >= kLowChecks) { lost_ = true; return kMove; }
        return kLow;
#endif
    }
    int lows_ = 0;
    bool lost_ = false;
};


// ---- (T8, 22 Nov) The rules for what may live on the system core. From the Galaxy port's CORE3.md (stage-load freezes; the disc read-ahead worker on core 3 is the leading suspect) and the MH3 report
// (the system's network stack lives on core 3: a busy thread there stalled online login, pings 4.5-8.4 s). They are the behaviour of MSC_VITA_CORE3=1; MSC_VITA_CORE3_RULES=0 gives back the
// 1.3 placement (everything the key moves, the disc and save workers included) for an A/B. Default OFF as a whole: MSC_VITA_CORE3 itself is off.
//   * Short or blocking threads only: audio output and a job lane that parks (c3g18: the mixer only with MSC_VITA_CORE3_MIXER=1, the log writer never: file I/O). The disc and save workers read in long
//     kernel calls (Galaxy: 735 reads, 31 MiB at a stage start): they stay on their plan core unless MSC_VITA_CORE3_DISC=1 asks for them. The frame-send worker, the vitaGL collector and any other
//     long-running or library thread are refused the core (Kind::kOther).
//   * No spinning: a job lane on core 3 parks at once (spin 0) instead of polling 200 us for the next job.
//   * Audio output is not at the top user priority there: its priority is no more urgent than MSC_VITA_CORE3_AUDIO_PRIO (default 128, Strikers' own output priority; Galaxy's 64 was the top).
//   * Everything off the core while an online or LAN session or the online screens exist, and back after kReturnAfterMs without one (OnlineGuard; the threads that were moved are remembered).
//   * Threads are pinned at creation (the mask goes into sceKernelCreateThread: the audio output thread already, the SDK layer's workers since this change); a thread that needs the move later (the log
//     writer, which starts before the settings are read) is moved once and logged. Strays (a library thread whose mask has core 3 without being registered) are moved off in the thread sweep.
enum class Kind { kAudioOut, kMixer, kLog, kDisc, kSave, kJobLane, kOther };

// "MSC_VITA_CORE3_DISC=1" (default 0): the disc and save workers may use the core.
inline bool DiscWanted(const char* text) { return text && std::strcmp(text, "1") == 0; }
// (c3g18, Galaxy's CORE3 breakdown of 23 Nov) The mixer shares a lock with the game thread: preempted on the system core while it holds it, the game waits. It stays on its plan core unless
// MSC_VITA_CORE3_MIXER=1 asks. The log writer writes files (sceIo inside the file-system driver, next to the system's own file-system threads: Galaxy's console-freeze pattern): never on core 3.
inline bool MixerWanted(const char* text) { return text && std::strcmp(text, "1") == 0; }
// The rules apply with MSC_VITA_CORE3=1, unless MSC_VITA_CORE3_RULES=0.
inline bool RulesOn(const char* core3, const char* rules) { return Wanted(core3) && !(rules && std::strcmp(rules, "0") == 0); }

struct Rules {
    bool on = false;      // the rules apply (RulesOn)
    bool disc = false;    // DiscWanted
    bool mixer = false;   // (c3g18) MixerWanted: MSC_VITA_CORE3_MIXER=1
    bool held = false;    // an online or LAN session is up: nothing on the core
};

// Which Kind a thread of the SDK layer is. name is np_thread_create's name; the SDK roles are 2 audio (mixer), 3 io (disc/nand workers).
inline Kind KindOfSdkThread(const char* name, int role) {
    if (name && !std::strcmp(name, "nand-io")) return Kind::kSave;
    if (name && !std::strncmp(name, "dvd-", 4)) return Kind::kDisc;
    if (role == 3) return Kind::kDisc;   // (an io-role thread of a name this table does not know: treated as the disc's)
    if (role == 2) return Kind::kMixer;
    return Kind::kOther;
}

// The mask a thread of this kind gets: `requested` is what the plan or a named line asked for, `plan` the plain mask (no fourth core). Without the rules the request stands (1.3 behaviour).
// With them the system core is taken out of the request when it is held, or the kind is not allowed there. Cores 1-2 in the request stay.
inline int FilterMask(const Rules& r, Kind kind, int requested, int plan) {
    if (!r.on || !(requested & kSystemMask)) return requested;
    bool allowed = true;
    switch (kind) {
    case Kind::kAudioOut: case Kind::kJobLane: break;
    case Kind::kMixer: allowed = r.mixer; break;
    case Kind::kLog: allowed = false; break;
    case Kind::kDisc: case Kind::kSave: allowed = r.disc; break;
    case Kind::kOther: allowed = false; break;
    }
#ifdef NP_CORE3_RULES_BREAK
    allowed = true;   // (the deliberate break of the unit test: no rule is enforced)
#endif
    if (r.held || !allowed) {
        const int rest = requested & ~kSystemMask;
        return rest ? rest : plan;
    }
    return requested;
}

// Priorities in the kernel's own number (64 the most urgent user priority, 160 the default, 191 the least), from either spelling: 64..191, or 0x10000100 +- an offset (0x10000100 = 160).
// 0 when it is neither.
inline int AbsPriority(long long v) {
    if (v >= 0x10000000LL && v <= 0x1000FFFFLL) v = 160 + (v - 0x10000100LL);
    return (v >= 64 && v <= 191) ? static_cast<int>(v) : 0;
}
constexpr int kCore3AudioPrioDefault = 128;
// The most urgent priority a thread may have on the system core. text = MSC_VITA_CORE3_AUDIO_PRIO (either spelling); unset or unusable: 128. 64 gives Galaxy's top priority back.
inline int Core3PrioFloor(const char* text) {
    if (!text || !*text) return kCore3AudioPrioDefault;
    const int v = AbsPriority(std::strtoll(text, nullptr, 0));
    return v ? v : kCore3AudioPrioDefault;
}
// A request in either spelling, as sceKernelCreateThread takes it: unchanged unless the rules apply and it is more urgent than the floor, then the floor in the form 0x10000100 + (floor - 160).
// `onCore3`: the thread's mask has the system core.
inline int PriorityOnCore3(const Rules& r, bool onCore3, int request, int floorAbs) {
    if (!r.on || !onCore3) return request;
    const int a = AbsPriority(request);
    if (!a || a >= floorAbs) return request;   // (an unknown spelling stays as it is)
#ifdef NP_CORE3_RULES_BREAK
    return request;
#endif
    return 0x10000100 + (floorAbs - 160);
}

// A job lane that sits on the system core never polls for its next job: it waits on its condition variable at once. Other lanes keep the configured spin.
inline unsigned JobSpinUs(int core, unsigned configuredUs) {
#ifdef NP_CORE3_RULES_BREAK
    return configuredUs;
#endif
    return core == 3 ? 0u : configuredUs;
}

// The online guard: `online` = a LAN or online session exists, or the online screens are open, or the multiplayer entry runs (GuardOnline below, read every few frames). The system's network stack runs on
// core 3, so the first online answer moves everything off (kRetreat); the core is given back after kReturnAfterMs of no online (kReturn), so a flicker does not move threads about.
constexpr unsigned kReturnAfterMs = 3000;
// (mp18) What the guard counts as online: a LAN or online session (NpLanActive), Online Central's screens (NpOnlineFlowActive) and the multiplayer entry before a session exists
// (NpLanEntryActive: HOST A ROOM / JOIN WITH A CODE while the room is made, STUN, TURN, the punch and up to 10 minutes of waiting for a partner, or the local radio's bring-up). Before mp18
// the entry was not counted: the threads stayed on the system core beside the network stack for the whole wait. A missing input (weak symbol absent) counts as not online.
inline bool GuardOnline(bool session, bool onlineScreens, bool entry) {
#ifdef NP_CORE3_ENTRY_BREAK
    (void)entry;
    return session || onlineScreens;
#else
    return session || onlineScreens || entry;
#endif
}
class OnlineGuard {
public:
    enum Action { kNone = 0, kRetreat = 1, kReturn = 2 };
    Action Step(bool online, unsigned nowMs) {
        if (online) {
            waiting_ = false;
            if (!held_) { held_ = true; ++retreats_; return kRetreat; }
            return kNone;
        }
        if (!held_) return kNone;
        if (!waiting_) { waiting_ = true; offSince_ = nowMs; return kNone; }
#ifdef NP_CORE3_GUARD_BREAK
        if (true) {
#else
        if (nowMs - offSince_ >= kReturnAfterMs) {
#endif
            held_ = false;
            waiting_ = false;
            ++returns_;
            return kReturn;
        }
        return kNone;
    }
    bool Held() const { return held_; }
    int Retreats() const { return retreats_; }
    int Returns() const { return returns_; }
private:
    bool held_ = false;
    bool waiting_ = false;
    unsigned offSince_ = 0;
    int retreats_ = 0, returns_ = 0;
};

// ---- (core3x15, 22 Nov) MSC_VITA_CORE3_WORK=<bits> (default 0 = off): real work on the system core, only with a proven core (fail closed: unproven, lost or held means every bit does nothing and
// the plan stands as it is today). The rules above hold for it (parks at once, no spinning, off while online, back later; the watch moves the threads away if the core loses its share).
//   bit 1 (kWorkJobs):  the job pool (np_jobs: pose jobs MSC_POSE_JOBS, MSC_PHYS_POSE, MSC_SNAP_PAR, MSC_CROWD_POS_PAR) gets one more lane, on core 3 (the largest jobs only use it: a call uses the
//                       last lane only when it has work for every lane). Not when MSC_JOBS_CORES names the cores itself or MSC_JOBS says one lane.
//   bit 2 (kWorkFepar): the front end's decode worker (MSC_VGX_FEPAR, renderer/src/vgx_fepar.inc) is placed on core 3 instead of core 1, where it takes time from the record player (the core-1 bound
//                       of replays); on core 3 it parks at once.
// A lane that is late is given up: LaneGuard, below.
constexpr int kWorkJobs = 1, kWorkFepar = 2, kWorkAll = 3;
inline int WorkBits(const char* text) {
    if (!text || !*text) return 0;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 0);
    return (end && !*end && v > 0) ? static_cast<int>(v & kWorkAll) : 0;
}
// The lane cores of the job pool. `lanes` is the count the pool would have (caller included), cores[] the worker cores (index = worker, lane - 1). Returns the lane count to use: one more, the new
// worker on core 3, when bit 1 is asked and core 3 is proven and the owner left the placement to the plan (`named`: MSC_JOBS_CORES) and the pool is parallel at all (lanes >= 2) and has room (maxLanes).
inline unsigned JobLanesWith(int work, bool proven, bool named, unsigned lanes, unsigned maxLanes, int cores[3]) {
#ifdef NP_CORE3WORK_BREAK
    if (!(work & kWorkJobs)) return lanes;   // (mutant: the proof is skipped)
#else
    if (!(work & kWorkJobs) || !proven || named || lanes < 2 || lanes >= maxLanes || lanes > 3) return lanes;
#endif
    cores[lanes - 1] = 3;
    return lanes + 1;
}

// A job lane or a decode worker on the system core that keeps the caller waiting is given up for the rest of the session: the system can take the core for a while (the 2 s watch only sees it
// later), and a fork-join waits for the slowest lane. Each call that the core-3 lane took part in reports how long the caller waited for it after its own share (lateUs, 0 when it was not
// last). A call later than kSlowUs is slow; kSlowLimit slow calls within kWindow reported calls retire the lane (the window then starts again, so a rare late call over a long session
// does nothing). Pure; the clocks are np_jobs.cpp's and vgx_fepar.inc's.
#ifndef NP_CORE3LANE_BREAK
#define NP_CORE3LANE_BREAK 0   // host test mutants: 1 one slow call retires, 2 the window is never started again, 3 a call of exactly kSlowUs is slow, 4 a call that was not late counts as slow
#endif
constexpr unsigned kSlowUsDefault = 1500;
constexpr int kSlowLimit = 3, kWindow = 48;
class LaneGuard {
public:
    enum Result { kOk = 0, kSlow = 1, kRetire = 2 };
    explicit LaneGuard(unsigned slowUs = kSlowUsDefault) : slowUs_(slowUs ? slowUs : kSlowUsDefault) {}
    Result Report(unsigned lateUs) {
        if (retired_) return kOk;
#if NP_CORE3LANE_BREAK == 3
        bool slow = lateUs >= slowUs_;
#elif NP_CORE3LANE_BREAK == 4
        bool slow = lateUs >= slowUs_ || lateUs == 0;
#else
        bool slow = lateUs > slowUs_;
#endif
        ++calls_;
        Result r = kOk;
        if (slow) {
            ++slows_;
            ++totalSlow_;
            r = kSlow;
#if NP_CORE3LANE_BREAK == 1
            retired_ = true;
            return kRetire;
#else
            if (slows_ >= kSlowLimit) { retired_ = true; return kRetire; }
#endif
        }
#if NP_CORE3LANE_BREAK != 2
        if (calls_ >= kWindow) { calls_ = 0; slows_ = 0; }
#endif
        return r;
    }
    bool Retired() const { return retired_; }
    unsigned SlowUs() const { return slowUs_; }
    int TotalSlow() const { return totalSlow_; }
private:
    unsigned slowUs_;
    int calls_ = 0, slows_ = 0, totalSlow_ = 0;
    bool retired_ = false;
};
// The decode worker's core: core 3 when bit 2 is asked and the core is open (proven, not lost, no online session) and the guard has not retired it, else the render core (`plan`).
inline int FeparCore(int work, bool open, bool retired, int plan) { return (work & kWorkFepar) && open && !retired ? 3 : plan; }
inline unsigned FeparSpinUs(int core, unsigned configuredUs) {
#ifdef NP_CORE3WORK_BREAK
    return configuredUs;
#endif
    return core == 3 ? 0u : configuredUs;
}

} // namespace NpVitaCore3
#endif
