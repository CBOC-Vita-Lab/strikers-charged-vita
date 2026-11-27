// Which thread runs on which core, in one place, settable from env.txt (the recompiled build's
// vita_threads.cpp without the split worker, plus the job pool's role).
//
// A Vita gives an application three cores (a fourth only through a kernel plug-in, CapUnlocker,
// which is the owner's decision: see np/docs/vita.md, and Core3Active below). The threads that matter:
//   game     the main thread: the Wii game's task loop (simulation, animation, frame building) and
//            the SDK layer called from it
//   render   the renderer's player and GL back end (vgx on vitaGL), and with it sceGxm's
//            display-queue thread ("gxmqueue")
//   audio    the audio output thread (vita_audio.cpp): blocks in sceAudioOutOutput
//   helper   threads the SDK layer creates through std::thread: the AX mixer, texture workers
//   gc       vitaGL's collector thread
//   player   the renderer's optional third thread (MSC_VGX_BACKEND_THREAD=1)
//   worker   the job pool's workers (np_jobs.cpp): fork-join jobs lent to the game thread
//   framesend the frame-send worker (np_framesend.cpp): the End Frame task's send of the finished frame (default: core 2, with audio and the mixer)
//
// DEFAULT PLAN (np/docs/vita.md "Core plan" has the reasoning and the numbers):
//   core 0   game
//   core 1   render, gxm display queue, worker 1
//   core 2   audio output, helpers (mixer), gc, player, worker 0
// Workers sit on the two cores that have idle time and run at a priority ABOVE the render
// thread's (np_jobs.cpp), so a job is never queued behind a 15 ms render burst.
//
// env.txt: MSC_VITA_CORE_GAME / _RENDER / _GXMQUEUE / _AUDIO / _HELPER / _GC / _PLAYER / _WORKER
// / _FRAMESEND = 0, 1 or 2; several digits for a set ("12" = core 1 or 2); "any" for all three.
// MSC_VITA_CORE_GC also takes the mixer along where that makes sense (np_vita_core3.h, vita_plan.cpp). MSC_VITA_CORE3=1 (1.3, default off): when a probe thread proves the
// system core (np_vita_core3.h), audio output goes there by default, the mixer and the disc and save workers too (vita_plan.cpp), and the log writer. An explicit
// MSC_VITA_CORE_AUDIO wins, and 3 is not a value of these variables: nothing names that core without the proof.
// The same file is built for armhf Linux (target `check`): there the roles are only parsed and logged.
#include "np_env_cache.h"
#include "np_vita_internal.h"
#include "np_vita_core3.h"
#include "../../../include/np/hot_knob.h"
#include "../../../include/np/touch_diag.h"
#pragma weak NpDiagRegisterThread

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
// (W5 FPS) np_fps_probe.cpp: every pinned thread is listed for the per-core busy time that MSC_FPS=auto reads. Weak: a build without it does not list them.
extern "C" void NpFpsProbeRegisterThread(int tid, int kind) __attribute__((weak));
#endif

// pace18: weak, a build without the game's NpSimRun17 is level 0
extern "C" int NpSimtLevel(void) __attribute__((weak));

namespace VitaThreads {
namespace {

volatile int g_core3Held = 0;   // (T8) an online or LAN session or the online screens are up: nothing lives on the system core (NpVitaCore3::OnlineGuard, Core3OnlineStep)
volatile int g_core3Lost = 0;   // (pace18) the watch found the system core short of its share twice in a row: it is not used for the rest of the session (NpVitaCore3::Watch)
constexpr int kUser0 = 0x00010000;   // SCE_KERNEL_CPU_MASK_USER_0; cores 1 and 2 are the next two bits
constexpr int kUserAll = 0x00070000;
constexpr int kSystem = NpVitaCore3::kSystemMask;
#if defined(__vita__)
static_assert(kUser0 == SCE_KERNEL_CPU_MASK_USER_0 && kUserAll == SCE_KERNEL_CPU_MASK_USER_ALL && kSystem == SCE_KERNEL_CPU_MASK_SYSTEM, "core mask constants");
#endif

struct RoleInfo {
    const char* name;      // in log lines
    const char* variable;  // in env.txt
};
constexpr RoleInfo kRoles[kRoleCount] = {
    {"game", "MSC_VITA_CORE_GAME"},   {"render", "MSC_VITA_CORE_RENDER"},     {"gxm display queue", "MSC_VITA_CORE_GXMQUEUE"},
    {"audio output", "MSC_VITA_CORE_AUDIO"}, {"helper", "MSC_VITA_CORE_HELPER"}, {"vitaGL collector", "MSC_VITA_CORE_GC"},
    {"record player", "MSC_VITA_CORE_PLAYER"}, {"job worker", "MSC_VITA_CORE_WORKER"}, {"frame send", "MSC_VITA_CORE_FRAMESEND"},
    {"sim thread", "MSC_VITA_CORE_SIM"},
};

int SimtLevel() { return NpSimtLevel ? NpSimtLevel() : 0; }   // pace18: MSC_SIMT in force (0 off or refused, 1 the step on np_sim and the game thread waits, 2 np_sim beside the window); weak: no NpSimRun17, level 0

// "0" / "1" / "2", several digits for a set, "any" / "all". 0 = not understood.
int ParseMask(const char* text) {
    if (!text || !*text) return 0;
    if (!std::strcmp(text, "any") || !std::strcmp(text, "all")) return kUserAll;
    int mask = 0;
    for (const char* c = text; *c; ++c) {
        if (*c < '0' || *c > '2') return 0;
        mask |= kUser0 << (*c - '0');
    }
    return mask;
}

// The plan's mask, without the fourth core.
int DefaultMask(Role role) {
    switch (role) {
    case kSim:   // pace18: MSC_SIMT=2 core 2 (the window keeps core 0, render and the send worker core 1); MSC_SIMT=1 the game thread's own core (it waits, so the core is idle)
        return SimtLevel() == 2 ? (kUser0 << 2) : CoreMask(kGame);
    case kFrameSend:   // pace18: under MSC_SIMT=2 core 2 belongs to np_sim, so the send worker's role default is core 1 (the plan C setting; plan A and S, which leave it unset, would have put it on core 2)
        return SimtLevel() == 2 ? (kUser0 << 1) : (kUser0 << 2);
    case kGame: return kUser0;
    case kRender: return kUser0 << 1;
    case kDisplayQueue: return CoreMask(kRender);
    case kWorker: return (kUser0 << 1) | (kUser0 << 2);
    default: return kUser0 << 2;   // audio, helper, gc, player
    }
}

void MaskText(int mask, char* out, size_t size) {
    if (mask == kUserAll) { std::snprintf(out, size, "any"); return; }
    size_t n = 0;
    for (int core = 0; core < 3 && n + 1 < size; ++core)
        if (mask & (kUser0 << core)) out[n++] = static_cast<char>('0' + core);
    if ((mask & kSystem) && n + 1 < size) out[n++] = '3';
    out[n] = '\0';
}

#if defined(__vita__)
// The probe thread's report (static: a thread that outlives the wait must not write to a stack that is gone). 1.3 (MSC_VITA_CORE3_RETRY): one slot per try, by the
// try's number, which the thread gets as its argument: a thread of an earlier try that runs late writes only its own slot.
constexpr int kProbeSlots = 4;   // (one try and at most three retries)
volatile int g_probeCpu[kProbeSlots] = {-1, -1, -1, -1}, g_probeMask[kProbeSlots] = {}, g_probeDone[kProbeSlots] = {};
int g_probeTry = 0;
int Core3ProbeMain(SceSize args, void* argp) {
    int slot = 0;
    if (argp && args >= sizeof(int)) std::memcpy(&slot, argp, sizeof slot);
    slot = (slot >= 0 && slot < kProbeSlots) ? slot : 0;
    g_probeCpu[slot] = sceKernelGetCpuId();
    g_probeMask[slot] = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    g_probeDone[slot] = 1;
    return 0;
}
struct KernelApi {
    int Create(int mask) { return sceKernelCreateThread("np_core3_probe", Core3ProbeMain, 0x10000100, 16 * 1024, 0, mask, nullptr); }
    int Change(int id, int mask) { return sceKernelChangeThreadCpuAffinityMask(id, mask); }
    int Start(int id) {
        int slot = g_probeTry;
        return sceKernelStartThread(id, sizeof slot, &slot);
    }
    int Wait(int id, unsigned timeoutUs) {
        SceUInt timeout = timeoutUs;
        const int rc = sceKernelWaitThreadEnd(id, nullptr, &timeout);
        return rc < 0 ? rc : 0;
    }
    bool Reported() { return g_probeDone[g_probeTry] != 0; }
    int Cpu() { return g_probeCpu[g_probeTry]; }
    int Mask() { return g_probeMask[g_probeTry]; }
    void Delete(int id) { sceKernelDeleteThread(id); }
    void Again() { if (g_probeTry + 1 < kProbeSlots) ++g_probeTry; }
    void Pause(unsigned us) { sceKernelDelayThread(us); }
};

// (pace18; the VitAdhoc finding: a thread on the system core can lose its share of it during play) The watch: every NpVitaCore3::kWatchPeriodMs it spins 3 ms on core 3 at a priority above
// the threads that live there (audio 128, send 144) and reads how much of that spell it ran (run clocks / system time). The decision (two checks under 50 % in a row, a good one clears the
// count) is NpVitaCore3::Watch, unit-tested in tests/test_core3.cpp. On "move": every thread whose mask is the system core goes to cores 1-2, the core is marked lost (Core3Proven / Core3Active
// answer false, a thread pinned later takes its plan's core), one log line, the watch ends. MSC_VITA_CORE3_WATCH=0 turns it off; it exists only when the probe proved the core.
// (c3g18, Galaxy's recommendation 8) The launch marker: the only fallback for a whole-console freeze, which no process can report. Written when the probe proves the core; removed by
// the watch after kMarkerClearS seconds of the session (the freezes Galaxy saw came at loading, in the first minutes). A launch that finds it starts with the fourth core off and removes it,
// so the launch after that tries again. MSC_VITA_CORE3_MARKER=0 turns it off. The literal path follows the title (stage-hardware-test.py rewrites it for the test app).
constexpr const char* kCore3Marker = "ux0:data/mscharged/core3-launch.txt";
constexpr unsigned kMarkerClearS = 120;
bool MarkerWanted() { const char* e = std::getenv("MSC_VITA_CORE3_MARKER"); return !(e && *e == '0'); }
bool MarkerPresent() { SceIoStat st; return sceIoGetstat(kCore3Marker, &st) >= 0; }
void MarkerWrite() {
    const SceUID fd = sceIoOpen(kCore3Marker, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    static const char kText[] = "the fourth core was in use; this file is removed after two minutes of play. Found at a launch: that launch runs without the fourth core.\n";
    sceIoWrite(fd, kText, sizeof(kText) - 1);
    sceIoClose(fd);
}
volatile int g_markerSet = 0;

uint64_t SelfRunClocks() {
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    return sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) == 0 ? static_cast<uint64_t>(info.runClocks) : 0;
}
extern "C" int NpJobsCore3Busy(void) __attribute__((weak));   // np_jobs.cpp (lane24 FIX-09)
int Core3WatchMain(SceSize, void*) {
    NpVitaCore3::Watch watch;
    const char* deferEnv = std::getenv("MSC_VITA_CORE3_WATCH_DEFER");
    const bool deferWanted = deferEnv && *deferEnv && *deferEnv != '0';
    unsigned deferred = 0, zeroStreak = 0;
    const uint64_t started = sceKernelGetSystemTimeWide();
    for (;;) {
        sceKernelDelayThread(NpVitaCore3::kWatchPeriodMs * 1000u);
        if (__atomic_load_n(&g_markerSet, __ATOMIC_ACQUIRE) && sceKernelGetSystemTimeWide() - started >= kMarkerClearS * 1000000ull) {   // (c3g18) the launch got through its first minutes
            if (__atomic_exchange_n(&g_markerSet, 0, __ATOMIC_ACQ_REL)) {
                sceIoRemove(kCore3Marker);
                std::fprintf(stderr, "[vita] fourth core: launch marker removed after %u s\n", kMarkerClearS);
            }
        }
        if (g_core3Held) continue;   // (T8) the 3 ms spell is a spin on core 3: not while an online session is up
        if (deferWanted && NpJobsCore3Busy) {   // (lane24 FIX-09, MSC_VITA_CORE3_WATCH_DEFER=1, default off) the spell runs above the job lane's priority: not while that lane is inside a job (it would delay the job by the spell's 3 ms);
            int tries = 0;                      // look again in 5 ms, up to 20 times (100 ms), then spin anyway: the check must still happen, and a lane that is always busy is the load the check is about
            while (NpJobsCore3Busy() && tries < 20) { sceKernelDelayThread(5000); ++tries; }
            if (tries) { ++deferred; if (deferred <= 3 || deferred % 100 == 0) std::fprintf(stderr, "[vita] fourth core watch: spell %u waited %d x 5 ms for the job lane on core 3 to leave its job%s\n", deferred, tries, tries >= 20 ? " (gave up waiting, spinning anyway)" : ""); }
        }
        // int26b (real-Vita 6402/6404 logs): sceKernelGetThreadInfo().runClocks is only flushed for the current
        // thread on a context switch; an uninterrupted 3 ms spin read r1 - r0 == 0 ("0 %") and falsely evicted core 3.
        sceKernelDelayThread(1);
        const uint64_t r0 = SelfRunClocks(), w0 = sceKernelGetSystemTimeWide();
        while (sceKernelGetSystemTimeWide() - w0 < 3000) {}
        const uint64_t w1 = sceKernelGetSystemTimeWide();
        sceKernelDelayThread(1);
        const uint64_t r1 = SelfRunClocks();
        const uint64_t wallUs = w1 > w0 ? (w1 - w0) : 0;
        if (r1 < r0 || (r1 == r0 && wallUs >= 2000 && ++zeroStreak <= 2)) {
            static bool zeroWarned = false;
            if (!zeroWarned) {
                zeroWarned = true;
                std::fprintf(stderr, "[vita] fourth core watch: runClocks did not advance across a %llu us spin (treated as indeterminate, not 0 %%)\n",
                             static_cast<unsigned long long>(wallUs));
            }
            continue;
        }
        if (r1 > r0) zeroStreak = 0;
        const double run = static_cast<double>(r1 > r0 ? (r1 - r0) : 0), wall = static_cast<double>(wallUs);
        const NpVitaCore3::Watch::Result res = watch.Check(run, wall);
        if (res == NpVitaCore3::Watch::kLow)
            std::fprintf(stderr, "[vita] fourth core watch: a spell on core 3 ran %.0f %% of its time (limit %.0f %%), once\n", 100.0 * run / wall, 100.0 * NpVitaCore3::kLowShare);
        if (res != NpVitaCore3::Watch::kMove) continue;
        g_core3Lost = 1;
        if (__atomic_exchange_n(&g_markerSet, 0, __ATOMIC_ACQ_REL)) {
            sceIoRemove(kCore3Marker);   // (int26b: active eviction is not a boot freeze; leaving the marker poisoned the next launch onto 3 cores)
        }
        int moved = 0, missesInRow = 0;
        const SceUID self = sceKernelGetThreadId();
        SceKernelThreadInfo info;
        for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
            if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
            std::memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
            missesInRow = 0;
            if (uid == self || !(static_cast<int>(info.currentCpuAffinityMask) & kSystem)) continue;
            if (sceKernelChangeThreadCpuAffinityMask(uid, kUser0 << 1 | kUser0 << 2) >= 0) ++moved;
        }
        std::fprintf(stderr, "[vita] fourth core lost its share: two checks in a row under %.0f %% (last %.0f %%): %d thread(s) moved back to cores 1-2, core 3 is not used for the rest of the session (MSC_VITA_CORE3_WATCH=0 turns the watch off)\n",
                     100.0 * NpVitaCore3::kLowShare, 100.0 * run / wall, moved);
        return 0;
    }
}
void StartCore3Watch() {
    const char* e = std::getenv("MSC_VITA_CORE3_WATCH");
    if (e && *e == '0') return;
    // Relative 0x10000100 = 160; legal relative range is [-32, +31]. Use -28 (0x100000E4 = abs 132): below audio (128), above decode worker (136) and np_jobs (144) so fepar cannot falsely evict core 3 (Carbon #6).
    const SceUID id = sceKernelCreateThread("np_core3watch", Core3WatchMain, 0x10000100 - 28, 16 * 1024, 0, kUser0 << 2, nullptr);
    if (id < 0) return;
    if (sceKernelChangeThreadCpuAffinityMask(id, kSystem) < 0) { sceKernelDeleteThread(id); return; }   // (cannot be placed on the core after all: no watch, nothing moved by it)
    // Carbon #4: publish marker before starting the watch thread so Core 3 cannot race MarkerWrite or miss g_markerSet.
    const bool wroteMarker = MarkerWanted();
    if (wroteMarker) {
        MarkerWrite();
        __atomic_store_n(&g_markerSet, 1, __ATOMIC_RELEASE);   // (c3g18) cleared by the watch after kMarkerClearS
    }
    if (sceKernelStartThread(id, 0, nullptr) < 0) {
        sceKernelDeleteThread(id);
        if (wroteMarker && __atomic_exchange_n(&g_markerSet, 0, __ATOMIC_ACQ_REL)) sceIoRemove(kCore3Marker);
        return;
    }
    std::fprintf(stderr, "[vita] fourth core watch: every %u ms a 3 ms spell on core 3; two checks under %.0f %% of its time move the system core's threads back (MSC_VITA_CORE3_WATCH=0 off)\n",
                 NpVitaCore3::kWatchPeriodMs, 100.0 * NpVitaCore3::kLowShare);
}
#endif

} // namespace

namespace {
bool EnvOne(const char* name) { return NpVitaCore3::Wanted(std::getenv(name)); }
// (T8) The rules of np_vita_core3.h as the environment and the online guard have them now. on: MSC_VITA_CORE3=1 and not MSC_VITA_CORE3_RULES=0 (read once).
// (c3crash18) No guarded statics on this path: on the console two threads met in the first use of CurrentRules' `static const` (the SDK layer's thread creation and the game thread's
// probe, at boot with MSC_VITA_CORE3=1), and libstdc++'s wait for the other initialiser failed in this runtime's recursive mutex: "terminate ... __concurrence_unlock_error", abort at
// frame 0 in every c3w run of 23 Nov (dump: __cxa_guard_acquire under CurrentRules under Core3ProbeRun). A read of the environment is pure, so concurrent first readers may both read:
// plain atomics with a "not read yet" value, constant-initialised (no guard).
// (c3def18) Read fresh at each call, not kept: the plan's defaults (vita_plan.cpp Apply) may set MSC_VITA_CORE3 / _WORK after an early reader (a thread pinned at boot) would have
// frozen "off"; the callers are thread creation, the probe and the online step every 8 frames, so a getenv each is nothing.
// (nxboot25 R2-12, MSC_ENV_CACHE=0 gives this back) The plan's defaults are all set once ApplyPlan has run: from then to the next LoadEnvFile a value is kept (np_env_cache.h; each reader has its slot),
// before that every call reads as above. The [env] line's takes per picture must fall from 5.7-14.3 to about 1 (6032.log, 6027.log).
NpEnvCache::Slot g_slotRulesOn, g_slotDisc, g_slotMixer, g_slotWork, g_slotKeys, g_slotWanted;
int CachedFlag(NpEnvCache::Slot& slot, int (*read)()) { return NpEnvCache::Get(slot, read); }
NpVitaCore3::Rules CurrentRules() {
    NpVitaCore3::Rules r;
    r.on = CachedFlag(g_slotRulesOn, [] { return NpVitaCore3::RulesOn(std::getenv("MSC_VITA_CORE3"), std::getenv("MSC_VITA_CORE3_RULES")) ? 1 : 0; }) != 0;
    r.disc = CachedFlag(g_slotDisc, [] { return NpVitaCore3::DiscWanted(std::getenv("MSC_VITA_CORE3_DISC")) ? 1 : 0; }) != 0;
    r.mixer = CachedFlag(g_slotMixer, [] { return NpVitaCore3::MixerWanted(std::getenv("MSC_VITA_CORE3_MIXER")) ? 1 : 0; }) != 0;
    r.held = g_core3Held != 0;
    return r;
}
int SendLevel() { return NpVitaCore3::SendLevel(std::getenv("MSC_VITA_CORE3_SEND")); }
// (T8) MSC_JOBS_CORES names core 3 for a job lane (np_jobs.cpp): the lane needs the proven core like every other user, so the probe runs for it too.
bool JobsWantCore3() { const char* e = std::getenv("MSC_JOBS_CORES"); return e && std::strchr(e, '3'); }
// (core3x15) MSC_VITA_CORE3_WORK (np_vita_core3.h): bit 1 a job lane on core 3, bit 2 the front end's decode worker; the probe runs for it, nothing moves unless the core is proven.
int WorkBitsNow() {
    return CachedFlag(g_slotWork, [] {
        const char* k = std::getenv("MSC_VITA_CORE3"); if (k && std::strcmp(k, "0") == 0) return 0;   // (c3kill18)
        return NpVitaCore3::WorkBits(std::getenv("MSC_VITA_CORE3_WORK")); });   // (c3crash18: no guard; nxboot25: the kill check is part of the kept value)
}

// The probe, once per process: a reread of the settings is the same start, and the threads have their cores. Runs when either key asks for the core.
// (c3def18) Any key asks for the core. Checked before the once-only probe too, so an early question (before the plan's defaults are set) does not cache "not available" for the process.
// (c3kill18) MSC_VITA_CORE3=0 written out is the kill switch for every use of the core (the plan's WORK default, a send or job-lane key included): no probe, nothing moves.
bool Core3Killed() { const char* e = std::getenv("MSC_VITA_CORE3"); return e && std::strcmp(e, "0") == 0; }
bool Core3KeysWantRead() { return !Core3Killed() && (EnvOne("MSC_VITA_CORE3") || SendLevel() || EnvOneShaders() || JobsWantCore3() || WorkBitsNow()); }
bool Core3KeysWant() { return CachedFlag(g_slotKeys, [] { return Core3KeysWantRead() ? 1 : 0; }) != 0; }   // (nxboot25: the per-job and per-frame callers)
bool Core3ProbeRun() {
    if (!Core3KeysWant()) return false;   // (1.3: MSC_VITA_CORE3_SHADERS, the shader compile worker; T8: MSC_JOBS_CORES with a 3)
    bool usable = false;
#if defined(__vita__)
    if (MarkerWanted() && MarkerPresent()) {
        sceIoRemove(kCore3Marker);
        std::fprintf(stderr, "[vita] fourth core: not available, nothing moves (the last launch that used it ended in its first %u s: the launch marker; this launch runs on three cores, the next one tries again; MSC_VITA_CORE3_MARKER=0 off)\n", kMarkerClearS);
        return false;
    }
    KernelApi api;
    // 1.3 MSC_VITA_CORE3_RETRY=<0..3> (default 2): a probe that may have met a moment's load on the system core is asked again (np_vita_core3.h ProbeRetry)
    const int retries = NP_HOT_KNOB(int, [] { const char* e = std::getenv("MSC_VITA_CORE3_RETRY"); return (e && *e) ? std::atoi(e) : 2; }());
    int tries = 1;
    const NpVitaCore3::Result r = NpVitaCore3::ProbeRetry(api, retries < 0 ? 0 : (retries > 3 ? 3 : retries), &tries);
    usable = r.usable;
    std::fprintf(stderr, "[vita] fourth core: %s (%s; create 0x%X, move 0x%X, start 0x%X, wait 0x%X, cpu %d, mask 0x%X; try %d, MSC_VITA_CORE3_RETRY)\n",
                 r.usable ? "usable" : "not available, nothing moves", r.why, static_cast<unsigned>(r.createRc),
                 static_cast<unsigned>(r.changeRc), static_cast<unsigned>(r.startRc), static_cast<unsigned>(r.waitRc), r.cpu, static_cast<unsigned>(r.mask), tries);
    if (r.usable) StartCore3Watch();
    if (r.usable && CurrentRules().on)
        std::fprintf(stderr, "[vita] fourth core rules (T8, c3g18): audio output (priority no more urgent than %d)%s%s; never the log writer (file I/O); no frame send, collector or library thread; no spinning; everything off while an online or LAN session is up (MSC_VITA_CORE3_RULES=0 for the old placement, MSC_VITA_CORE3_DISC=1 for the disc and save workers)\n",
                     NpVitaCore3::Core3PrioFloor(std::getenv("MSC_VITA_CORE3_AUDIO_PRIO")), CurrentRules().mixer ? ", the mixer (MSC_VITA_CORE3_MIXER=1)" : "", CurrentRules().disc ? ", the disc and save workers (MSC_VITA_CORE3_DISC=1)" : "");
    if (r.usable)
        std::fprintf(stderr, "[vita] fourth core use: %s%s%s\n", EnvOne("MSC_VITA_CORE3") ? (CurrentRules().on ? (CurrentRules().disc ? "audio output, disc and save work" : "audio output") : "audio, disc and save work and the log writer") : "",
                     SendLevel() ? (EnvOne("MSC_VITA_CORE3") ? ", frame send" : "frame send") : "", SendLevel() >= 2 ? ", vitaGL collector" : "");
#endif
    return usable;
}
// Once per process, safe for concurrent first callers (the shader worker at boot): the others wait for the final answer (NpVitaCore3::ProbeOnceShared).
bool Core3ProbeOnce() {
    static int state = -1;
    return NpVitaCore3::ProbeOnceShared(state, Core3ProbeRun, [](unsigned us) {
#if defined(__vita__)
        sceKernelDelayThread(us);
#else
        (void)us;
#endif
    });
}
} // namespace

// MSC_VITA_CORE3=1: audio, mixer, disc and save workers, log writer (proven core only).
bool Core3Active() { return EnvOne("MSC_VITA_CORE3") && !g_core3Lost && Core3ProbeOnce(); }
// (T8) Active and not held by an online session: where a thread that is made now may go.
bool Core3Open() { return Core3Active() && !g_core3Held; }
// Either key, and the core proven.
bool Core3Proven() { return !g_core3Lost && Core3KeysWant() && Core3ProbeOnce(); }   // (c3def18: no probe, and nothing cached, while no key asks)
// MSC_VITA_CORE3_SHADERS=1 (1.3): the shader compile worker asks for the fourth core.
bool EnvOneShaders() {   // r16 (Galaxy core_layout.h, hitch33 11: its console powered off seconds after the compile worker ran libshacccg on the system core): refused
    static bool told = false;
    if (EnvOne("MSC_VITA_CORE3_SHADERS") && !told) { told = true; std::fprintf(stderr, "[vita] MSC_VITA_CORE3_SHADERS=1 refused: the shader compile worker never runs on the system core (Galaxy found its console powered off after that)\n"); }
    return false;
}

// ---- (T8) The threads on the system core, and the online guard ---------------------------------------------------------------------------------------------------
// Every thread that was placed on the system core is listed (id and name, so that a recycled id is never moved by mistake). The list is what the online guard moves off the core and back, and what
// the stray sweep compares with: a thread with core 3 in its mask that is not on the list asked for nothing and is moved to cores 1-2 (a library thread; Galaxy wraps pte_osThreadCreate for this).
#if defined(__vita__)
namespace {
struct Core3Entry { SceUID uid; char name[32]; int retreatPrio; int savedPrio; };
Core3Entry g_c3[16];
int g_c3n = 0;
volatile int g_c3lock = 0;
void C3Lock() { while (__sync_lock_test_and_set(&g_c3lock, 1)) sceKernelDelayThread(50); }
void C3Unlock() { __sync_lock_release(&g_c3lock); }
bool NameOf(SceUID uid, char* out, size_t size, int* mask, int* prio = nullptr) {
    SceKernelThreadInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(uid, &info) != 0) return false;
    std::snprintf(out, size, "%s", info.name);
    if (mask) *mask = static_cast<int>(info.currentCpuAffinityMask);
    if (prio) *prio = info.currentPriority;
    return true;
}
NpVitaCore3::OnlineGuard g_guard;
}   // namespace
#endif

void Core3Register(int uid, int retreatPrio) {
#if defined(__vita__)
    char name[32];
    if (!NameOf(uid, name, sizeof(name), nullptr)) return;
    C3Lock();
    bool have = false;
    for (int i = 0; i < g_c3n; ++i) if (g_c3[i].uid == uid) { have = true; if (retreatPrio > 0) g_c3[i].retreatPrio = retreatPrio; }
    if (!have && g_c3n < 16) { g_c3[g_c3n].uid = uid; std::snprintf(g_c3[g_c3n].name, sizeof(g_c3[g_c3n].name), "%s", name); g_c3[g_c3n].retreatPrio = retreatPrio; g_c3[g_c3n].savedPrio = 0; ++g_c3n; }
    C3Unlock();
#else
    (void)uid; (void)retreatPrio;
#endif
}

void Core3Unregister(int uid) {
#if defined(__vita__)
    C3Lock();
    for (int j = 0; j < g_c3n; ++j) if (g_c3[j].uid == uid) { g_c3[j] = g_c3[--g_c3n]; break; }
    C3Unlock();
#else
    (void)uid;
#endif
}

// Moves the listed threads to `mask` (cores 1-2 to leave, the system core to come back); a thread that is gone, or whose id is another thread's now, is dropped from the list.
static int Core3MoveListed(int mask) {
    int moved = 0;
#if defined(__vita__)
    const bool retreating = (mask != kSystem);
    Core3Entry copy[16];
    C3Lock();
    const int n = g_c3n;
    std::memcpy(copy, g_c3, sizeof(copy));
    C3Unlock();
    for (int i = 0; i < n; ++i) {
        char name[32];
        int curPrio = 0;
        if (!NameOf(copy[i].uid, name, sizeof(name), nullptr, &curPrio) || std::strcmp(name, copy[i].name) != 0) {
            C3Lock();
            for (int j = 0; j < g_c3n; ++j) if (g_c3[j].uid == copy[i].uid) { g_c3[j] = g_c3[--g_c3n]; break; }
            C3Unlock();
            continue;
        }
        if (sceKernelChangeThreadCpuAffinityMask(copy[i].uid, mask) >= 0) {
            ++moved;
            // int28 (Carbon #3): only threads that explicitly opted in with retreatPrio (e.g. np_lane at abs 140) change priority on online retreat;
            // Audio Output (128), np_jobs (144), and fepar (136) keep their native priorities.
            if (retreating && copy[i].retreatPrio > 0 && curPrio > 0) {
                C3Lock();
                for (int j = 0; j < g_c3n; ++j) if (g_c3[j].uid == copy[i].uid && g_c3[j].savedPrio == 0) { g_c3[j].savedPrio = curPrio; break; }
                C3Unlock();
                sceKernelChangeThreadPriority(copy[i].uid, copy[i].retreatPrio);
            } else if (!retreating && copy[i].savedPrio > 0) {
                sceKernelChangeThreadPriority(copy[i].uid, copy[i].savedPrio);
                C3Lock();
                for (int j = 0; j < g_c3n; ++j) if (g_c3[j].uid == copy[i].uid) { g_c3[j].savedPrio = 0; break; }
                C3Unlock();
            }
        }
    }
#else
    (void)mask;
#endif
    return moved;
}

// The online guard's step, from the game thread every few frames. `online`: NpLanActive() || NpOnlineFlowActive(). Retreat: held is set first (a thread made from now on takes its plan's core), then the
// listed threads leave. Return: held clears, the listed threads go back (not after the watch gave the core up).
void Core3OnlineStep(bool online) {
#if defined(__vita__)
    const bool wanted = CachedFlag(g_slotWanted, [] { return (CurrentRules().on || JobsWantCore3() || WorkBitsNow()) ? 1 : 0; }) != 0;   // (c3crash18: no guard)
    if (!wanted || g_core3Lost) return;
    if (!Core3Proven()) return;   // (not proven: nothing is on the core)
    const NpVitaCore3::OnlineGuard::Action a = g_guard.Step(online, static_cast<unsigned>(sceKernelGetSystemTimeWide() / 1000u));
    if (a == NpVitaCore3::OnlineGuard::kRetreat) {
        g_core3Held = 1;
        const int moved = Core3MoveListed(kUser0 << 1 | kUser0 << 2);
        std::fprintf(stderr, "[vita] core 3 rules: an online or LAN session is up: %d thread(s) moved off the system core (they come back %u ms after the session ends)\n", moved, NpVitaCore3::kReturnAfterMs);
    } else if (a == NpVitaCore3::OnlineGuard::kReturn) {
        g_core3Held = 0;
        const int moved = Core3MoveListed(kSystem);
        std::fprintf(stderr, "[vita] core 3 rules: no online session any more: %d thread(s) back on the system core\n", moved);
    }
#else
    (void)online;
#endif
}

// The stray sweep: threads with the system core in their mask that nothing asked for (the thread sweep's call, MoveLogWriterToCore3 below).
static void Core3SweepStrays() {
#if defined(__vita__)
    if (!CurrentRules().on || !Core3Active()) return;
    static int told = 0;
    SceKernelThreadInfo info;
    int missesInRow = 0, moved = 0;
    C3Lock();
    Core3Entry copy[16];
    const int n = g_c3n;
    std::memcpy(copy, g_c3, sizeof(copy));
    C3Unlock();
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        const int have = static_cast<int>(info.currentCpuAffinityMask);
        if (!(have & kSystem)) continue;
        bool listed = !std::strcmp(info.name, "np_core3watch") || !std::strcmp(info.name, "np_core3_probe");
        for (int i = 0; i < n && !listed; ++i) if (copy[i].uid == uid) listed = true;
        if (listed) continue;
        const int rest = have & kUserAll;
        const int to = rest ? rest : (kUser0 << 1 | kUser0 << 2);
        const int res = sceKernelChangeThreadCpuAffinityMask(uid, to);
        ++moved;
        if (told < 8) { ++told; std::fprintf(stderr, "[vita] core 3 rules: thread \"%s\" (priority %d) had the system core in its mask (0x%X) without asking: moved to 0x%X (%s)\n", info.name,
                                             static_cast<int>(info.currentPriority), static_cast<unsigned>(have), static_cast<unsigned>(to), res >= 0 ? "ok" : "refused"); }
    }
    (void)moved;
#endif
}

bool g_core3LostNow() { return g_core3Lost != 0; }
bool g_core3HeldNow() { return g_core3Held != 0; }
NpVitaCore3::Rules CurrentRulesNow() { return CurrentRules(); }

int PlainMask(Role role) { return DefaultMask(role); }

namespace {
int g_planMask[kRoleCount];   // CoreMask's answer before the fourth core was considered (0 = not decided)
}

int CoreMask(Role role) {
    static int cached[kRoleCount];   // 0 = not decided yet
    if (role < 0 || role >= kRoleCount) return kUserAll;
    if (cached[role]) return cached[role];
    int mask = 0;
    const char* text = std::getenv(kRoles[role].variable);
    if (role == kSim && !(text && *text)) text = std::getenv("MSC_SIMT_CORE");   // (the alias: MSC_VITA_CORE_SIM wins)
    if (text && *text) {
        mask = ParseMask(text);
        if (!mask && role == kSim && std::strchr(text, '3')) std::fprintf(stderr, "[vita] %s=%s refused: np_sim never runs on the fourth core (digits 0-2 only): using the default\n", kRoles[role].variable, text);
        else if (!mask) std::fprintf(stderr, "[vita] %s=%s not understood (0, 1, 2, a set like 12, or any): using the default\n", kRoles[role].variable, text);
    }
    if (!mask) mask = role == kAudioOut && Core3Active() ? kSystem : DefaultMask(role);
    g_planMask[role] = mask;   // without the fourth core
    if (role == kFrameSend) mask = NpVitaCore3::SendMask(SendLevel() >= 1, Core3Proven(), mask);
    if (role == kCollector) mask = NpVitaCore3::CollectorMask(SendLevel(), Core3Proven(), mask);
    // (T8) The rules: audio output may be on the system core; the frame-send worker and the collector (long-running, one of them busy most of a frame) may not, MSC_VITA_CORE3=1 or not
    // MSC_VITA_CORE3_RULES=0. Held by an online session at the time of the call: the plan's core.
    if (mask & kSystem) {
        const NpVitaCore3::Kind kind = role == kAudioOut ? NpVitaCore3::Kind::kAudioOut : NpVitaCore3::Kind::kOther;
        const int filtered = NpVitaCore3::FilterMask(CurrentRules(), kind, mask, g_planMask[role]);
        if (filtered != mask) {
            std::fprintf(stderr, "[vita] core 3 rules: the %s does not run on the system core (%s): core plan's cores instead\n", RoleName(role), g_core3Held ? "an online session is up" : "long-running thread");
            mask = filtered;
        }
    }
    cached[role] = mask;
    return mask;
}

int SimPriority(int level) {   // -1: leave the priority as the thread has it
    const char* text = std::getenv("MSC_VITA_PRIO_SIM");
    if (!(text && *text)) text = std::getenv("MSC_SIMT_PRIO");
    if (text && *text) return static_cast<int>(std::strtol(text, nullptr, 0));
    return level == 2 ? 0x10000100 - 17 : -1;   // np_jobs.cpp gives the workers 0x10000100 - 16: np_sim is one more urgent, below the audio output (and the mixer / timers at 128)
}

const char* RoleName(Role role) { return role >= 0 && role < kRoleCount ? kRoles[role].name : "?"; }

int PlanMask(Role role) {
    if (role < 0 || role >= kRoleCount) return kUserAll;
    CoreMask(role);
    return g_planMask[role] ? g_planMask[role] : DefaultMask(role);
}

namespace {
int g_gameUid = 0;   // the game thread's kernel id (the thread that pinned itself as kGame), for KeepCoreZeroForGame
}

int PinCurrentThread(Role role, bool log) {
    int mask = CoreMask(role);
    int result = 0;
#if defined(__vita__)
    if (role == kGame) g_gameUid = sceKernelGetThreadId();
    if ((g_core3Lost || g_core3Held) && (mask & kSystem)) mask = g_planMask[role] ? g_planMask[role] : DefaultMask(role);   // (pace18) the watch gave the system core up, or (T8) an online session is up: a thread started later goes to the plan's core
    result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    if (result < 0 && (mask & kSystem)) {   // (1.3) the fourth core was refused after all: the plan's core, never a thread left on "any"
        const int plan = NpVitaCore3::AfterRefusal(mask, g_planMask[role] ? g_planMask[role] : DefaultMask(role));
        std::fprintf(stderr, "[vita] thread \"%s\": the fourth core was refused (0x%X), back to the plan's core\n", RoleName(role), static_cast<unsigned>(result));
        result = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), plan);
        mask = plan;
    }
    if (result >= 0 && (mask & kSystem)) Core3Register(sceKernelGetThreadId());
    if(NpDiagRegisterThread)NpDiagRegisterThread((uint32_t)role,(uint32_t)sceKernelGetThreadId());
    // kind: 2 the game, 1 work that follows time (audio output, mixer, helpers), 0 per frame
    if (NpFpsProbeRegisterThread) NpFpsProbeRegisterThread(sceKernelGetThreadId(), role == kGame ? 2 : (role == kAudioOut || role == kHelper) ? 1 : 0);
#endif
    if (VitaMem::RegisterThread) VitaMem::RegisterThread(RoleName(role));
    if (log) {
        char cores[8];
        MaskText(mask, cores, sizeof(cores));
        std::fprintf(stderr, "[vita] thread \"%s\" on core %s (mask 0x%X): %s (0x%X)\n", RoleName(role), cores, static_cast<unsigned>(mask),
                     result >= 0 ? "ok" : "REFUSED", static_cast<unsigned>(result));
    }
    return result;
}

void LogPlan() {
    char line[420];
    size_t n = 0;
    for (int role = 0; role < kRoleCount; ++role) {
        char cores[8];
        MaskText(CoreMask(static_cast<Role>(role)), cores, sizeof(cores));
        const int wrote = std::snprintf(line + n, sizeof(line) - n, "%s%s %s", n ? ", " : "", kRoles[role].name, cores);
        if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(line) - n) break;
        n += static_cast<size_t>(wrote);
    }
    std::fprintf(stderr, "[vita] core plan: %s (MSC_VITA_CORE_* in env.txt changes it)\n", line);
}

// ---- MSC_VITA_THREAD_CPU (1.3, after Galaxy r99 GXV_THREAD_CPU): where the CPU time went, by core mask, since the last line. For the long console sessions:
// what else than the game thread may take core 0, and how much of each core the workers use. One sceKernelGetThreadInfo per live thread a call (the table walk of
// LogThreadTable), every 600 frames. Reads only; default off.
bool ThreadCpuOn() {
    const bool on = NP_HOT_KNOB(bool, [] { const char* v = std::getenv("MSC_VITA_THREAD_CPU"); return v && std::strcmp(v, "0") != 0; }());   // (c3crash18: no guard)
    return on;
}
void LogThreadCpu(const char* when) {
#if defined(__vita__)
    struct Seen { SceUID uid; uint64_t run; };
    static Seen seen[256];
    static int nSeen = 0;
    static uint64_t lastUs = 0;
    static SceUID game = 0;
    if (game <= 0) game = g_gameUid;
    const uint64_t now = sceKernelGetSystemTimeWide();
    // mask classes: 0 core 0 only, 1 core 1, 2 core 2, 3 cores 1-2, 4 core 3, 5 other (several cores with core 0, or the system's default)
    unsigned long long cls[6] = {0, 0, 0, 0, 0, 0}, gameMs = 0;
    struct Zero { char name[32]; unsigned mask; double ms; };
    Zero zero[8];
    int nZero = 0;
    Seen next[256];
    int nNext = 0, missesInRow = 0;
    SceKernelThreadInfo info;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        uint64_t prev = 0;
        bool known = false;
        for (int i = 0; i < nSeen; ++i)
            if (seen[i].uid == uid) { prev = seen[i].run; known = true; break; }
        if (nNext < 256) next[nNext++] = Seen{uid, info.runClocks};
        if (!known && lastUs) prev = 0;            // (started since the last line: all of its time is in this interval)
        else if (!known) prev = info.runClocks;    // (the first line counts from now)
        const double ms = info.runClocks > prev ? static_cast<double>(info.runClocks - prev) / 1000.0 : 0.0;
        unsigned m = static_cast<unsigned>(info.currentCpuAffinityMask) & 0xF0000u;
        int c = 5;
        if (m == 0x10000u) c = 0; else if (m == 0x20000u) c = 1; else if (m == 0x40000u) c = 2; else if (m == 0x60000u) c = 3; else if (m == 0x80000u) c = 4;
        cls[c] += static_cast<unsigned long long>(ms + 0.5);
        if (uid == game) gameMs += static_cast<unsigned long long>(ms + 0.5);
        else if ((m == 0 || (m & 0x10000u)) && ms >= 0.05 && nZero < 8) {
            std::snprintf(zero[nZero].name, sizeof(zero[nZero].name), "%s", info.name);
            zero[nZero].mask = static_cast<unsigned>(info.currentCpuAffinityMask);
            zero[nZero].ms = ms;
            ++nZero;
        }
    }
    std::memcpy(seen, next, sizeof(Seen) * static_cast<size_t>(nNext));
    nSeen = nNext;
    if (lastUs) {
        std::fprintf(stderr, "[threads] %s: %.1f s, CPU ms by core mask: core 0 only %llu (of it the game thread %llu), 1 %llu, 2 %llu, 1-2 %llu, 3 %llu, other %llu (MSC_VITA_THREAD_CPU)\n",
                     when ? when : "", static_cast<double>(now - lastUs) / 1e6, cls[0], gameMs, cls[1], cls[2], cls[3], cls[4], cls[5]);
        for (int i = 0; i < nZero; ++i)
            std::fprintf(stderr, "[threads]   may run on core 0: \"%s\" mask 0x%X: %.1f ms\n", zero[i].name, zero[i].mask, zero[i].ms);
    }
    lastUs = now;
#else
    (void)when;
#endif
}

// ---- the live thread table (1.2) -------------------------------------------------------------------------------------------------
// One log line per thread of the process: name, priority (sceKernel's own number: lower is more urgent, 160 is the default of a user thread),
// core mask, stack size, CPU time so far. Logging builds print it a few times in a session (the memory report's frames) so that a console log
// shows what the code says: audio output 128, mixer and timers 128, job/frame-send workers 144, render and game 160, and where the library
// threads (gxm display queue, vitaGL collector) really are.
void LogThreadTable(const char* when) {
#if defined(__vita__)
    SceKernelThreadInfo info;
    int missesInRow = 0, seen = 0;
    std::fprintf(stderr, "[threads] %s:\n", when ? when : "");
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        ++seen;
        std::fprintf(stderr, "[threads]   %-26s priority %3d (initial %3d)  cores 0x%X (initial 0x%X, last ran on %d)  stack %6u KiB  cpu %8llu ms\n", info.name,
                     static_cast<int>(info.currentPriority), static_cast<int>(info.initPriority), static_cast<unsigned>(info.currentCpuAffinityMask),
                     static_cast<unsigned>(info.initCpuAffinityMask), static_cast<int>(info.lastExecutedCpuId), static_cast<unsigned>(info.stackSize / 1024u),
                     static_cast<unsigned long long>(info.runClocks / 1000u));
    }
    std::fprintf(stderr, "[threads] %d threads\n", seen);
#else
    (void)when;
#endif
}

// (1.3) The log writer starts before the settings are read, so it is moved when it is found: at the first presents, with the sweep below. A refusal leaves it where it is.
#if defined(__vita__)
static void MoveLogWriterToCore3() {
    static int tries = 0;
    if (tries >= 4 || !Core3Open()) return;   // (T8: not while an online session holds the core)
    // (c3g18) the rules keep the log writer (file I/O) off the system core; MSC_VITA_CORE3_RULES=0 gives the old move back for an A/B
    if (!(NpVitaCore3::FilterMask(CurrentRules(), NpVitaCore3::Kind::kLog, kSystem, 0) & kSystem)) { tries = 4; return; }
    ++tries;
    SceKernelThreadInfo info;
    int missesInRow = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        if (std::strcmp(info.name, "np_log_writer") != 0) continue;
        const int res = sceKernelChangeThreadCpuAffinityMask(uid, kSystem);
        if (res >= 0) Core3Register(uid);
        std::fprintf(stderr, "[vita] thread \"%s\" moved to the fourth core: %s (0x%X)\n", info.name, res >= 0 ? "ok" : "refused", static_cast<unsigned>(res));
        tries = 4;
        return;
    }
}
#endif

// ---- core 0 for the game thread only (1.2; the Galaxy port's CORE0_ALONE) ---------------------------------------------------------
// MSC_VITA_CORE0_ALONE=1 moves every other thread of the process that may run on core 0 AND on another core (affinity "any", or 0: what a thread made without a
// mask gets) to cores 1-2; =log only lists them. Threads pinned to core 0 alone (the game's OS threads, which run one at a time with the game) and the game thread
// itself are left, and so is a thread that would be left with no core. The plan already pins what this build makes (vita_threads roles, the pthread_create wrap
// for helpers, the sceGxm display queue and vitaGL's collector); this catches the rest (a library or log thread with the default mask) at the same presents as
// PinLibraryThreads. On in the player build (vita_plan.cpp; =0 or =off turns it off); the log mode tells which threads there are.
// 1.3 MSC_VITA_SWEEP_BG=1 (default 0): each sweep is at least 500 system calls (one per id, until 500 ids in a row are no thread). Every 600 frames it was made on the game thread,
// and PinLibraryThreads on the render thread, a burst in the middle of a frame every 20 s at 30 fps in every build. With the switch the two are made on np_stallwatch, a thread that
// only wakes every 5 s, every 30 s. A thread that appears later is moved up to 30 s later, as against up to 20 s; no result of the game depends on either.
bool SweepInBackground() {
    const bool on = NP_HOT_KNOB(bool, [] { const char* e = std::getenv("MSC_VITA_SWEEP_BG"); return e && *e == '1'; }());   // (c3crash18: no guard)
    return on;
}

int KeepCoreZeroForGame(bool log) {
#if defined(__vita__)
    MoveLogWriterToCore3();
#endif
    Core3SweepStrays();   // (T8)
    static int mode = -1;   // 0 off, 1 log only, 2 move
    if (mode < 0) {
        const char* e = std::getenv("MSC_VITA_CORE0_ALONE");
        mode = (!e || !*e || !std::strcmp(e, "0") || !std::strcmp(e, "off")) ? 0 : (!std::strcmp(e, "log") ? 1 : 2);
        if (mode) std::fprintf(stderr, "[vita] MSC_VITA_CORE0_ALONE=%s: %s\n", e, mode == 1 ? "threads that share core 0 with the game are listed" : "threads that could run on core 0 and elsewhere are moved to cores 1-2");
    }
    if (!mode) return 0;
    int moved = 0;
#if defined(__vita__)
    SceKernelThreadInfo info;
    int missesInRow = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        if (uid == g_gameUid || (g_gameUid == 0 && uid == sceKernelGetThreadId())) continue;
        const int have = static_cast<int>(info.currentCpuAffinityMask) & kUserAll;
        if (!have && (info.currentCpuAffinityMask & static_cast<unsigned>(kSystem))) continue;   // on the fourth core alone: no game core at all (0 below means "any")
        const int effective = have ? have : kUserAll;
        const int others = effective & ~kUser0;
        if (!(effective & kUser0) || !others) continue;   // not on core 0, or on core 0 alone (a thread of the game's own)
        if (mode == 1) {
            if (log) std::fprintf(stderr, "[vita] thread \"%s\" (priority %d) may run on core 0 with the game (cores 0x%X)\n", info.name, static_cast<int>(info.currentPriority),
                                  static_cast<unsigned>(have));
            continue;
        }
        const int res = sceKernelChangeThreadCpuAffinityMask(uid, others);
        ++moved;
        std::fprintf(stderr, "[vita] thread \"%s\" (priority %d) moved off core 0 (cores 0x%X -> 0x%X): %s\n", info.name, static_cast<int>(info.currentPriority),
                     static_cast<unsigned>(have), static_cast<unsigned>(others), res >= 0 ? "ok" : "refused");
    }
#else
    (void)log;
#endif
    return moved;
}

// ---- helper threads: the SDK layer's std::thread objects (np_vita_process.cpp wraps pthread_create)
namespace {
struct HelperStart {
    void* (*entry)(void*);
    void* arg;
};
void* HelperTrampoline(void* raw) {
    const HelperStart start = *static_cast<HelperStart*>(raw);
    std::free(raw);
    PinCurrentThread(kHelper, false);   // (also lists the thread's stack for the memory report)
    return start.entry(start.arg);
}
} // namespace

bool WrapHelperThread(void* (**entry)(void*), void** arg) {
    HelperStart* start = static_cast<HelperStart*>(std::malloc(sizeof(HelperStart)));
    if (!start) return false;
    start->entry = *entry;
    start->arg = *arg;
    *entry = HelperTrampoline;
    *arg = start;
    return true;
}

} // namespace VitaThreads

// 1.3 MSC_VITA_CORE3_SHADERS (the renderer's shader compile worker, vgx_host_vita.cpp VgxHostAsyncWorkerPlace): the system core's mask once the
// probe proved it (the probe runs for this key too), else 0. Only with the key set: without it the worker stays on cores 1-2.
extern "C" int NpVitaCore3Mask(void) {
    if (!VitaThreads::EnvOneShaders()) return 0;
    return VitaThreads::Core3Proven() ? NpVitaCore3::kSystemMask : 0;
}

// pace18: np_sim's start-up: the role kSim's core (and registration with the per-core probe, kind 0), then its priority.
extern "C" int NpVitaPinSimThread(int level, unsigned* coreMask) {
    const int rc = VitaThreads::PinCurrentThread(VitaThreads::kSim, true);
    const int mask = VitaThreads::CoreMask(VitaThreads::kSim);
    if (coreMask) *coreMask = static_cast<unsigned>(mask >> 16) & 7u;
    const int prio = VitaThreads::SimPriority(level);
#if defined(__vita__)
    if (prio >= 0) {
        const int r2 = sceKernelChangeThreadPriority(sceKernelGetThreadId(), prio);
        std::fprintf(stderr, "[simt] np_sim: priority 0x%X (%s; MSC_SIMT=%d)\n", static_cast<unsigned>(prio), r2 >= 0 ? "ok" : "REFUSED", level);
    } else std::fprintf(stderr, "[simt] np_sim: priority kept as the thread has it (MSC_SIMT=%d)\n", level);
#else
    (void)prio;
#endif
    return rc;
}

// (T8) The SDK layer's thread plan (sdk/vita/np_plat_vita.cpp apply_role, np_thread_create) asks here before it puts a thread on the system core. Weak there: without this file nothing changes.
// NpVitaCore3Filter: the mask the thread may have (the request, unless the rules take the system core out of it); NpVitaCore3Priority: the priority, kept no more urgent than the floor on that core;
// NpVitaCore3Placed: the calling thread is on the system core now (listed for the online guard and the stray sweep).
extern "C" int NpVitaCore3Filter(const char* name, int role, int mask) {
    if (!(mask & NpVitaCore3::kSystemMask)) return mask;
    constexpr int kPlain = 0x00040000;   // the SDK layer's helper cores: core 2
    if (VitaThreads::g_core3LostNow()) return (mask & ~NpVitaCore3::kSystemMask) ? (mask & ~NpVitaCore3::kSystemMask) : kPlain;
    return NpVitaCore3::FilterMask(VitaThreads::CurrentRulesNow(), NpVitaCore3::KindOfSdkThread(name, role), mask, kPlain);
}
extern "C" int NpVitaCore3Priority(int mask, int prio) {
    if (!(mask & NpVitaCore3::kSystemMask) || !prio) return prio;
    return NpVitaCore3::PriorityOnCore3(VitaThreads::CurrentRulesNow(), true, prio, NpVitaCore3::Core3PrioFloor(std::getenv("MSC_VITA_CORE3_AUDIO_PRIO")));
}
extern "C" void NpVitaCore3Placed(void) {
#if defined(__vita__)
    VitaThreads::Core3Register(sceKernelGetThreadId());
#endif
}
// np_jobs.cpp: may a job lane use the system core now (proven, not lost, no online session)?
extern "C" int NpVitaCore3LaneOk(void) { return VitaThreads::Core3Proven() && !VitaThreads::g_core3HeldNow() ? 1 : 0; }

// (core3x15) MSC_VITA_CORE3_WORK, asked by np_jobs.cpp (bit 1) and the renderer's decode worker (bit 2, vgx_host_vita.cpp PinFeparThread). The bits only say what was asked: each user still needs the proven core
// (NpVitaCore3LaneOk / NpVitaCore3FeparOpen) and takes the plan's core otherwise.
extern "C" int NpVitaCore3Work(void) { return VitaThreads::WorkBitsNow(); }
extern "C" void NpVitaCore3Unplaced(void) {   // the calling thread left the system core by itself (a retired placement): it is no longer the online guard's to move
#if defined(__vita__)
    VitaThreads::Core3Unregister(sceKernelGetThreadId());
#endif
}
extern "C" int NpVitaCore3FeparOpen(void) { return (VitaThreads::WorkBitsNow() & NpVitaCore3::kWorkFepar) && VitaThreads::Core3Proven() && !VitaThreads::g_core3HeldNow() ? 1 : 0; }
