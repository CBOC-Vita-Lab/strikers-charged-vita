// The fourth-core probe and the mixer's placement (src/np_vita_core3.h) on a fake kernel. Control flow only: this says nothing about what a console does.
//   clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -Inative/platform/vita/src native/platform/vita/tests/test_core3.cpp -o /tmp/test_core3 && /tmp/test_core3
#include "np_vita_core3.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

using namespace NpVitaCore3;

namespace {
int g_checks, g_failures;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}

// A console without the plug-in unless a case says otherwise: 0x80028025 is what the Galaxy port's log shows.
struct Fake {
    int createSystemRc = static_cast<int>(0x80028025), createPlainRc = 0x1234, changeRc = static_cast<int>(0x80028025), startRc = 0, waitRc = 0;
    bool reported = true;
    int cpu = 3, mask = kSystemMask;
    int creates = 0, changes = 0, starts = 0, deletes = 0, waits = 0, agains = 0, pauses = 0;
    int waitFailures = 0;      // (MSC_VITA_CORE3_RETRY) the first n waits time out, the later ones end in time
    unsigned lastTimeout = 0;
    int Create(int m) { ++creates; return m == kSystemMask ? createSystemRc : createPlainRc; }
    int Change(int, int) { ++changes; return changeRc; }
    int Start(int) { ++starts; return startRc; }
    int Wait(int, unsigned t) { lastTimeout = t; ++waits; return waits <= waitFailures ? static_cast<int>(0x80028005) : waitRc; }
    bool Reported() { return reported; }
    int Cpu() { return cpu; }
    int Mask() { return mask; }
    void Delete(int) { ++deletes; }
    void Again() { ++agains; }
    void Pause(unsigned) { ++pauses; }
};
constexpr int kRefused = static_cast<int>(0x80028025);
}

int main() {
    Check(!Wanted(nullptr) && !Wanted("") && !Wanted("0") && !Wanted("off") && !Wanted("11") && Wanted("1"), "only MSC_VITA_CORE3=1 asks for the core");
    {   // no plug-in: creation and the move are both refused, nothing ever runs
        Fake f;
        const Result r = Probe(f);
        Check(!r.usable && f.starts == 0 && f.deletes == 1 && f.creates == 2, "no plug-in: closed, and the plain thread is cleaned up");
    }
    {   // no thread at all
        Fake f; f.createPlainRc = -1;
        Check(!Probe(f).usable && f.deletes == 0, "no thread could be made");
    }
    {   // the plug-in: creation with the mask
        Fake f; f.createSystemRc = 0x55;
        const Result r = Probe(f);
        Check(r.usable && r.cpu == 3 && f.creates == 1 && f.changes == 0 && f.deletes == 1, "created with the mask");
    }
    {   // the plug-in lifts the move only
        Fake f; f.changeRc = 0;
        Check(Probe(f).usable && f.creates == 2 && f.changes == 1 && f.deletes == 1, "moved to the mask");
    }
    {   // creation "works", but the mask was ignored: this is the case a success code alone would have got wrong
        Fake f; f.createSystemRc = 0x55; f.cpu = 2;
        Check(!Probe(f).usable, "a thread that ran on core 2 proves nothing");
        Fake g; g.createSystemRc = 0x55; g.mask = 0x40000;
        Check(!Probe(g).usable, "a thread without the system mask proves nothing");
        Fake h; h.createSystemRc = 0x55; h.mask = 0;
        Check(!Probe(h).usable, "mask read back 0 (the Galaxy log)");
    }
    {   // refused at the start, or never scheduled
        Fake f; f.createSystemRc = 0x55; f.startRc = kRefused;
        Check(!Probe(f).usable && f.deletes == 1 && f.waits == 0, "start refused");
        Fake g; g.createSystemRc = 0x55; g.waitRc = static_cast<int>(0x80028005);
        Check(!Probe(g).usable && g.deletes == 1, "never finished: timeout");
        Fake h; h.createSystemRc = 0x55; h.reported = false;
        Check(!Probe(h).usable && h.deletes == 1, "ended without a report");
    }
    // 1.3 MSC_VITA_CORE3_RETRY: a bounded retry of the answers a moment's load can give, never of the final ones
    {   // no plug-in: refused at once, no retry and no pause
        Fake f;
        int tries = 0;
        const Result r = ProbeRetry(f, 2, &tries);
        Check(!r.usable && !r.transient && tries == 1 && f.pauses == 0 && f.creates == 2, "retry: no plug-in is final (one try, no delay)");
    }
    {   // the plug-in, the first probe not scheduled in time: the second one proves the core
        Fake f; f.createSystemRc = 0x55; f.waitFailures = 1;
        int tries = 0;
        const Result r = ProbeRetry(f, 2, &tries);
        Check(r.usable && tries == 2 && f.pauses == 1 && f.agains == 1 && f.lastTimeout == 2 * kProbeTimeoutUs, "retry: one timeout, then usable (twice the time)");
    }
    {   // timeouts every time: bounded, the time at most 400 ms
        Fake f; f.createSystemRc = 0x55; f.waitFailures = 100;
        int tries = 0;
        const Result r = ProbeRetry(f, 3, &tries);
        Check(!r.usable && tries == 4 && f.waits == 4 && f.lastTimeout == 400u * 1000u, "retry: bounded at 1 + 3 tries, 400 ms at most");
        Fake g; g.createSystemRc = 0x55; g.waitFailures = 100;
        Check(!ProbeRetry(g, 0, &tries).usable && tries == 1, "retry: MSC_VITA_CORE3_RETRY=0 is one try as before");
    }
    {   // final answers are not asked again
        Fake f; f.createSystemRc = 0x55; f.mask = 0x40000; f.cpu = 2;
        int tries = 0;
        Check(!ProbeRetry(f, 2, &tries).usable && tries == 1, "retry: a mask without the system core is final");
        Fake g; g.createSystemRc = 0x55; g.startRc = kRefused;
        Check(!ProbeRetry(g, 2, &tries).usable && tries == 1, "retry: a refused start is final");
        Fake h; h.createPlainRc = -1;
        Check(!ProbeRetry(h, 2, &tries).usable && tries == 1, "retry: no thread is final");
        Fake k; k.createSystemRc = 0x55; k.cpu = 2;   // the mask has the system core, the thread was seen elsewhere: may be a moment's scheduling
        Check(ProbeRetry(k, 2, &tries).usable == false && tries == 3, "retry: ran on another core with the system mask is asked again");
    }
    // ProbeOnceShared (hw2): concurrent first callers all get the final answer, the probe runs once
    for (int want = 0; want <= 1; ++want) {
        int state = -1;
        std::atomic<int> runs{0}, wrong{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < 6; ++i)
            ts.emplace_back([&] {
                const bool r = ProbeOnceShared(state, [&] { ++runs; std::this_thread::sleep_for(std::chrono::milliseconds(60)); return want == 1; },
                                               [](unsigned) { std::this_thread::sleep_for(std::chrono::microseconds(200)); });
                if (r != (want == 1)) ++wrong;
            });
        for (auto& th : ts) th.join();
        Check(runs == 1 && wrong == 0, "probe once: one run, every concurrent caller sees the final answer");
    }
    Check(!MixerFollows(nullptr, 0) && !MixerFollows("", 0), "no key, the mixer stays");
    Check(MixerFollows("1", 0) && !std::strcmp(MixerFollows("1", 0), "1") && MixerFollows("1", 1) && MixerFollows("1", -1), "core 1 in plans C, Cn and by hand");
    Check(!MixerFollows("1", 2) && !MixerFollows("1", 3), "plans A and S: the render work is on core 1");
    Check(!MixerFollows("2", 0), "core 2 is where it is");
    Check(MixerFollows("12", 0) != nullptr, "a set without core 0");
    Check(!MixerFollows("0", 0) && !MixerFollows("01", 0) && !MixerFollows("any", 0) && !MixerFollows("all", 0), "never the game's core");
    // MSC_VITA_CORE3_SEND: only a proven core moves the frame-send worker, and a refusal goes back to the plan's mask
    Check(SendMask(true, true, 0x20000) == kSystemMask, "send wanted and core proven: the system core");
    Check(SendMask(true, false, 0x20000) == 0x20000 && SendMask(false, true, 0x20000) == 0x20000 && SendMask(false, false, 0x20000) == 0x20000, "otherwise the plan's mask");
    Check(AfterRefusal(kSystemMask, 0x20000) == 0x20000 && AfterRefusal(0x20000, 0x40000) == 0x20000 && AfterRefusal(0x30000 | kSystemMask, 0x20000) == 0x20000, "a refused system mask falls back to the plan; others are kept");
    // MSC_VITA_CORE3_SEND=2 (deep, folded into io's key): the collector too, only on a proven core
    Check(SendLevel("1") == 1 && SendLevel("2") == 2 && SendLevel("0") == 0 && SendLevel("3") == 0 && SendLevel(nullptr) == 0, "send levels");
    Check(CollectorMask(2, true, 0x40000) == kSystemMask, "level 2 on a proven core: the collector goes to the system core");
    Check(CollectorMask(1, true, 0x40000) == 0x40000 && CollectorMask(2, false, 0x40000) == 0x40000 && CollectorMask(0, true, 0x40000) == 0x40000, "otherwise its plan mask");
    // pace18: the watch on the system core: two low checks in a row move, one does not, a good check in between clears the count, no measurement counts for nothing
    {
        Watch w;
        Check(w.Check(1000, 2000) == Watch::kOk && w.Check(900, 2000) == Watch::kLow && !w.Lost(), "watch: one low check (45 %) does not move");
        Check(w.Check(1900, 2000) == Watch::kOk && w.Lows() == 0, "watch: a good check clears the count");
        Check(w.Check(800, 2000) == Watch::kLow && w.Check(700, 2000) == Watch::kMove && w.Lost(), "watch: two low checks in a row move");
        Check(w.Check(100, 2000) == Watch::kOk, "watch: moves once (latched)");
        Watch v;
        Check(v.Check(1000, 2000) == Watch::kOk && v.Check(1, 2000) == Watch::kLow && v.Check(1000, 2000) == Watch::kOk && v.Check(1, 2000) == Watch::kLow && !v.Lost(), "watch: low, good, low never moves");
        Watch x;
        Check(x.Check(1, 2000) == Watch::kLow && x.Check(0, 0) == Watch::kOk && x.Lows() == 1 && x.Check(1, 2000) == Watch::kMove, "watch: a check without wall time is neither good nor low");
        Watch y;
        Check(y.Check(1000, 2000) == Watch::kOk && y.Check(1000, 2000) == Watch::kOk && y.Lows() == 0, "watch: exactly 50 % is not low");
    }

    // ---- T8: the placement rules (np_vita_core3.h), on a fake thread table. Build with -DNP_CORE3_RULES_BREAK or -DNP_CORE3_GUARD_BREAK: those mutants MUST fail some check.
    {
        constexpr int kPlain = 0x40000;   // core 2
        Check(!RulesOn(nullptr, nullptr) && !RulesOn("0", nullptr) && RulesOn("1", nullptr) && RulesOn("1", "1") && !RulesOn("1", "0"), "rules: on with MSC_VITA_CORE3=1, off with MSC_VITA_CORE3_RULES=0");
        Check(!DiscWanted(nullptr) && !DiscWanted("0") && DiscWanted("1") && !DiscWanted("2"), "rules: MSC_VITA_CORE3_DISC is 1 or off (default off)");
        Rules off;   // MSC_VITA_CORE3 not set: the request stands whatever it is (nothing else is changed by this work)
        Check(FilterMask(off, Kind::kDisc, kSystemMask, kPlain) == kSystemMask && FilterMask(off, Kind::kOther, kSystemMask, kPlain) == kSystemMask, "rules off: the 1.3 placement stands");
        Rules on; on.on = true;
        // the table of the SDK layer's threads under MSC_VITA_CORE3=1 (what vita_plan.cpp asks for: the mixer on 3; the io workers on 3 only if the plan asked, which it does not without DISC=1)
        struct Th { const char* name; int role; };
        const Th table[] = {{"audio", 2}, {"dvd-io", 3}, {"dvd-open", 3}, {"dvd-keep", 3}, {"nand-io", 3}, {"thp-decode", 5}, {"vi-retrace", 4}};
        int onCore3 = 0;
        for (const Th& t : table) {
            const int m = FilterMask(on, KindOfSdkThread(t.name, t.role), kSystemMask, kPlain);
            if (m & kSystemMask) ++onCore3;
            if (t.role == 3) Check(!(m & kSystemMask) && m == kPlain, "rules: the disc and save workers are never on core 3 without MSC_VITA_CORE3_DISC");
        }
        Check(onCore3 == 0, "rules (c3g18): no SDK thread is on core 3 by default (the mixer needs MSC_VITA_CORE3_MIXER=1)");
        Rules mix = on; mix.mixer = true;
        Check(FilterMask(mix, Kind::kMixer, kSystemMask, kPlain) == kSystemMask && FilterMask(on, Kind::kMixer, kSystemMask, kPlain) == kPlain, "rules (c3g18): the mixer only with MSC_VITA_CORE3_MIXER=1");
        Check(!MixerWanted(nullptr) && !MixerWanted("0") && MixerWanted("1"), "rules (c3g18): MSC_VITA_CORE3_MIXER is 1 or off");
        Check(FilterMask(mix, Kind::kLog, kSystemMask, kPlain) == kPlain, "rules (c3g18): the log writer (file I/O) never");
        Check(KindOfSdkThread("nand-io", 3) == Kind::kSave && KindOfSdkThread("dvd-keep", 3) == Kind::kDisc && KindOfSdkThread("audio", 2) == Kind::kMixer && KindOfSdkThread("x", 5) == Kind::kOther, "rules: the kinds of the SDK threads");
        Rules disc = on; disc.disc = true;
        Check(FilterMask(disc, Kind::kDisc, kSystemMask, kPlain) == kSystemMask && FilterMask(disc, Kind::kSave, kSystemMask, kPlain) == kSystemMask, "rules: MSC_VITA_CORE3_DISC=1 gives the disc and save workers the core");
        // the frame send worker and the collector (kOther) and any library thread: never
        Check(FilterMask(disc, Kind::kOther, kSystemMask, kPlain) == kPlain, "rules: long-running and library threads never");
        Check(FilterMask(on, Kind::kAudioOut, kSystemMask, kPlain) == kSystemMask && FilterMask(on, Kind::kJobLane, kSystemMask, kPlain) == kSystemMask, "rules: audio output and a job lane may");
        // a request that names cores 1-2 as well as 3 keeps them
        Check(FilterMask(on, Kind::kOther, kSystemMask | 0x20000 | 0x40000, kPlain) == 0x60000, "rules: a set with cores 1-2 keeps them when 3 is taken out");
        Check(FilterMask(on, Kind::kDisc, 0x40000, kPlain) == 0x40000, "rules: a request without core 3 is never changed");
        // held by an online session: nothing
        Rules held = disc; held.held = true;
        for (Kind k : {Kind::kAudioOut, Kind::kMixer, Kind::kLog, Kind::kDisc, Kind::kSave, Kind::kJobLane, Kind::kOther})
            Check(!(FilterMask(held, k, kSystemMask, kPlain) & kSystemMask), "rules: while an online session is up no kind is on core 3");
    }
    {   // priorities: audio output not at the top user priority on core 3
        Rules on; on.on = true;
        Check(AbsPriority(64) == 64 && AbsPriority(191) == 191 && AbsPriority(63) == 0 && AbsPriority(192) == 0 && AbsPriority(0x10000100) == 160 && AbsPriority(0x10000100 - 32) == 128 && AbsPriority(0x10000100 - 96) == 64, "priority spellings");
        Check(Core3PrioFloor(nullptr) == 128 && Core3PrioFloor("") == 128 && Core3PrioFloor("112") == 112 && Core3PrioFloor("64") == 64 && Core3PrioFloor("0x100000E0") == 128 && Core3PrioFloor("7") == 128, "priority floor: default 128, either spelling, junk is the default");
        Check(PriorityOnCore3(on, true, 0x10000100 - 96, 128) == 0x10000100 - 32, "the top priority (64) on core 3 is held back to the floor");
        Check(PriorityOnCore3(on, true, 0x10000100 - 48, 128) == 0x10000100 - 32, "112 is more urgent than 128: held back");
        Check(PriorityOnCore3(on, true, 0x10000100 - 32, 128) == 0x10000100 - 32 && PriorityOnCore3(on, true, 0x10000100, 128) == 0x10000100, "128 and 160 stay");
        Check(PriorityOnCore3(on, false, 0x10000100 - 96, 128) == 0x10000100 - 96, "a thread that is not on core 3 keeps its priority");
        Rules off;
        Check(PriorityOnCore3(off, true, 0x10000100 - 96, 128) == 0x10000100 - 96, "rules off: unchanged");
        Check(PriorityOnCore3(on, true, 64, 64) == 64, "floor 64 (the owner's opt-in) lets the top priority through");
        Check(PriorityOnCore3(on, true, 90, 128) == 0x10000100 - 32, "absolute spelling is capped too");
        Check(PriorityOnCore3(on, true, 0, 128) == 0, "0 (leave the priority) stays 0");
    }
    {   // no spinning on core 3
        Check(JobSpinUs(3, 200) == 0 && JobSpinUs(2, 200) == 200 && JobSpinUs(1, 200) == 200 && JobSpinUs(0, 0) == 0, "job lane: core 3 parks at once, the others keep their spin");
    }
    {   // (mp18) what counts as online: a session, Online Central's screens, and the multiplayer entry before a session (HOST A ROOM / JOIN: room, STUN, TURN, the wait for a partner)
        Check(!GuardOnline(false, false, false), "guard input: nothing up is not online");
        Check(GuardOnline(true, false, false) && GuardOnline(false, true, false), "guard input: a session or the online screens are online");
        Check(GuardOnline(false, false, true), "guard input: the multiplayer entry (room made, waiting for a partner) is online");
        OnlineGuard g;   // a host waiting 10 minutes in its room: the threads are off for the whole wait, not only once the lobby opens
        Check(g.Step(GuardOnline(false, false, true), 1000) == OnlineGuard::kRetreat, "guard: HOST A ROOM retreats before any session exists");
        Check(g.Step(GuardOnline(false, false, true), 600000) == OnlineGuard::kNone && g.Held(), "guard: still held after 10 minutes of waiting");
        Check(g.Step(GuardOnline(true, false, false), 600100) == OnlineGuard::kNone && g.Held() && g.Retreats() == 1, "guard: the session that follows the entry keeps it held (no second move)");
    }
    {   // the online guard: retreat at once, return only after kReturnAfterMs of no online, no flapping
        OnlineGuard g;
        Check(g.Step(false, 1000) == OnlineGuard::kNone && !g.Held(), "guard: no online, nothing");
        Check(g.Step(true, 2000) == OnlineGuard::kRetreat && g.Held(), "guard: the first online answer retreats");
        Check(g.Step(true, 2100) == OnlineGuard::kNone && g.Retreats() == 1, "guard: retreats once");
        Check(g.Step(false, 3000) == OnlineGuard::kNone && g.Held(), "guard: the end of online starts the wait");
        Check(g.Step(false, 3000 + kReturnAfterMs - 1) == OnlineGuard::kNone && g.Held(), "guard: not back before the wait is over");
        Check(g.Step(true, 3000 + kReturnAfterMs - 1) == OnlineGuard::kNone && g.Held() && g.Returns() == 0, "guard: online again resets the wait (a flicker moves nothing)");
        Check(g.Step(false, 7000) == OnlineGuard::kNone, "guard: the wait starts again");
        Check(g.Step(false, 7000 + kReturnAfterMs) == OnlineGuard::kReturn && !g.Held() && g.Returns() == 1, "guard: back after kReturnAfterMs without online");
        Check(g.Step(true, 20000) == OnlineGuard::kRetreat && g.Retreats() == 2, "guard: and away again for the next session");
        OnlineGuard z;   // clock at 0 is a valid stamp
        Check(z.Step(true, 0) == OnlineGuard::kRetreat && z.Step(false, 0) == OnlineGuard::kNone && z.Step(false, kReturnAfterMs) == OnlineGuard::kReturn, "guard: a clock of 0 works");
    }
    {   // a whole session on a fake thread table: threads made before, during and after an online match
        struct Fake3 { int mask[8]; Kind kind[8]; int n = 0; } t;
        Rules rules; rules.on = true;
        OnlineGuard g;
        auto make = [&](Kind k) { t.kind[t.n] = k; t.mask[t.n++] = FilterMask(rules, k, kSystemMask, 0x40000); };
        make(Kind::kAudioOut); make(Kind::kMixer); make(Kind::kDisc); make(Kind::kLog); make(Kind::kJobLane);
        auto count3 = [&] { int c = 0; for (int i = 0; i < t.n; ++i) c += (t.mask[i] & kSystemMask) != 0; return c; };
        Check(count3() == 2, "session (c3g18): audio output and the job lane are on core 3; the mixer, the log writer and the disc worker are not");
        const OnlineGuard::Action a = g.Step(true, 100);   // the online screens open
        rules.held = g.Held();
        if (a == OnlineGuard::kRetreat) for (int i = 0; i < t.n; ++i) t.mask[i] &= ~kSystemMask;
        make(Kind::kMixer);   // a thread made during the match (a new SDK worker)
        Check(count3() == 0, "session: nothing on core 3 while online (the threads that were there and the one made during the match)");
        g.Step(false, 5000); const OnlineGuard::Action b = g.Step(false, 5000 + kReturnAfterMs);
        rules.held = g.Held();
        if (b == OnlineGuard::kReturn) for (int i = 0; i < 5; ++i) if (t.kind[i] != Kind::kDisc) t.mask[i] |= kSystemMask;
        Check(count3() == 4, "session: the threads that were on core 3 are back after the session");
    }
    {   // (core3x15) MSC_VITA_CORE3_WORK: the bits, the job lane's place, the lateness rule, the decode worker's core
        Check(WorkBits(nullptr) == 0 && WorkBits("") == 0 && WorkBits("0") == 0 && WorkBits("x") == 0 && WorkBits("1x") == 0 && WorkBits("-1") == 0, "work: unset, empty, 0 and junk ask for nothing");
        Check(WorkBits("1") == kWorkJobs && WorkBits("2") == kWorkFepar && WorkBits("3") == kWorkAll && WorkBits("7") == kWorkAll, "work: bits 1 and 2, others ignored");
        int c[3] = {2, 1, 0};
        Check(JobLanesWith(1, true, false, 3, 4, c) == 4 && c[2] == 3 && c[0] == 2 && c[1] == 1, "work: the pool's third worker is on core 3, the others stay (workers 2, 1, 3)");
        int c2[3] = {2, 1, 0};
        Check(JobLanesWith(1, true, false, 2, 4, c2) == 3 && c2[1] == 3 && c2[0] == 2, "work: with one worker, the new one is on core 3");
        int d[3] = {2, 1, 0};
        Check(JobLanesWith(1, false, false, 3, 4, d) == 3 && d[2] == 0, "work: core 3 not proven: the pool is as it was");
        Check(JobLanesWith(0, true, false, 3, 4, d) == 3 && d[2] == 0, "work: bit 1 not asked: as it was");
        Check(JobLanesWith(2, true, false, 3, 4, d) == 3 && d[2] == 0, "work: only bit 2: the pool is as it was");
        Check(JobLanesWith(1, true, true, 3, 4, d) == 3 && d[2] == 0, "work: MSC_JOBS_CORES names the cores: they stand");
        Check(JobLanesWith(1, true, false, 1, 4, d) == 1, "work: one lane (serial) stays serial");
        Check(JobLanesWith(1, true, false, 4, 4, d) == 4 && d[2] == 0, "work: no room for another lane");
        Check(FeparCore(2, true, false, 1) == 3 && FeparCore(2, false, false, 1) == 1 && FeparCore(1, true, false, 1) == 1 && FeparCore(2, true, true, 1) == 1 && FeparCore(0, true, false, 1) == 1,
              "work: the decode worker goes to core 3 only when asked, open and not retired");
        Check(FeparSpinUs(3, 30) == 0 && FeparSpinUs(1, 30) == 30, "work: the decode worker parks on core 3");
        // the lateness rule
        LaneGuard g;
        Check(g.Report(0) == LaneGuard::kOk && g.Report(kSlowUsDefault) == LaneGuard::kOk && !g.Retired(), "lane guard: calls that are not late do nothing (exactly the limit is not late)");
        Check(g.Report(kSlowUsDefault + 1) == LaneGuard::kSlow && g.Report(kSlowUsDefault + 1) == LaneGuard::kSlow && !g.Retired(), "lane guard: one or two late calls only count");
        Check(g.Report(kSlowUsDefault + 1) == LaneGuard::kRetire && g.Retired() && g.Report(99999) == LaneGuard::kOk, "lane guard: the third late call in the window retires the lane, for good");
        LaneGuard w;   // slow calls spread over windows never add up
        bool retired = false;
        for (int i = 0; i < 20 * kWindow; ++i) if (w.Report(i % kWindow == 7 ? 5000u : 100u) == LaneGuard::kRetire) retired = true;
        Check(!retired && !w.Retired() && w.TotalSlow() == 20, "lane guard: a late call in every window, 20 windows: never retired");
        LaneGuard v;   // three late calls inside one window, with good calls between
        int rc = 0;
        for (int i = 0; i < kWindow; ++i) rc += v.Report(i == 3 || i == 20 || i == 40 ? 4000u : 50u) == LaneGuard::kRetire;
        Check(rc == 1 && v.Retired(), "lane guard: three late calls spread over one window retire it");
        LaneGuard v2;
        for (int i = 0; i < kWindow - 1; ++i) v2.Report(i == 3 || i == 20 ? 4000u : 50u);
        v2.Report(10u);   // the window ends here: the two late calls are forgotten
        Check(v2.Report(4000u) == LaneGuard::kSlow && !v2.Retired(), "lane guard: a new window starts after kWindow calls");
        LaneGuard tuned(300);
        Check(tuned.Report(301) == LaneGuard::kSlow && tuned.Report(300) == LaneGuard::kOk && tuned.SlowUs() == 300, "lane guard: the limit is tunable (MSC_VITA_CORE3_SLOW_US)");
    }
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ok", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
