// vita_probe.cpp (Vita only): the real CPU clock, and the probe of the fourth core (PIPE, wave 5).
//
// 1. MeasureMHz(): a calibrated loop of dependent additions (1 cycle each on the Cortex-A9: the ALU result is forwarded), so iterations x 64 / time = MHz.
//    scePowerGetArmClockFrequency() reports what the application asked for and what the system shows; a PSVshell profile (500 MHz, kernel side)
//    can make the real clock differ from it, and the game's frame budget depends on the real one. Logged at every start ("[clock]"), and every
//    MSC_VITA_CLOCK_PERIOD seconds when that is set (a 20 ms burst on the audio core, background priority).
// 2. RunCore4Probe(): MSC_VITA_PROBE_CORE4=1 (env.txt / np-player.txt) before the game starts, about 12 s; =only ends the process afterwards.
//    A thread with the affinity mask SCE_KERNEL_CPU_MASK_SYSTEM (0x80000, the system core). Spin-and-count threads measure
//      * whether the thread can be created / moved to the mask, and which core sceKernelGetCpuId() reports,
//      * how much of a core the thread gets: bursts per second against a user core, per 10 ms window (min, p5, median), and the longest gap
//        between two bursts (the system took the core away),
//      * what it costs the three game cores: the same spin on cores 0, 1, 2 alone, together, and together with core 3 (scheduling and memory
//        bandwidth: a second pass streams a 4 MiB buffer on each core).
//    The verdict line says whether a job lane on core 3 is plausible: the job pool takes MSC_JOBS=4 MSC_JOBS_CORES=213 (np_jobs.h) after that.
#include "np_vita_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__vita__)
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#endif

namespace VitaProbe {

#if defined(__vita__)
namespace {

constexpr uint32_t kChain = 64;   // dependent additions per loop iteration

// n iterations of 64 dependent adds (+ a loop counter that issues beside them): 64 cycles an iteration.
__attribute__((noinline)) uint32_t Chain(uint32_t acc, uint32_t n) {
    __asm__ volatile(
        "1:\n\t"
        ".rept 64\n\t"
        "add %0, %0, #1\n\t"
        ".endr\n\t"
        "subs %1, %1, #1\n\t"
        "bne 1b\n\t"
        : "+r"(acc), "+r"(n)
        :
        : "cc");
    return acc;
}

uint64_t NowUs() { return sceKernelGetSystemTimeWide(); }

// The VitAdhoc session's measurement (read 496 MHz under the 500 MHz profile): 2,000,000 iterations of subs / bne, one cycle each on the A9, best of three.
__attribute__((noinline)) void SubsLoop(uint32_t n) {
    __asm__ volatile(
        "1:\n\t"
        "subs %0, %0, #1\n\t"
        "bne 1b\n\t"
        : "+r"(n)
        :
        : "cc");
}
double MeasureLoop2M() {
    uint64_t best = ~0ull;
    for (int i = 0; i < 3; ++i) {
        const uint64_t t0 = NowUs();
        SubsLoop(2000000u);
        const uint64_t dt = NowUs() - t0;
        if (dt && dt < best) best = dt;
    }
    return best == ~0ull ? 0.0 : 2000000.0 / double(best);   // iterations per microsecond = MHz
}

uint32_t g_sink;

} // namespace

double MeasureMHz(uint32_t tries, uint32_t windowMs) {
    double best = 0.0;
    uint32_t acc = 0;
    for (uint32_t t = 0; t < tries; ++t) {
        uint64_t iters = 0;
        const uint64_t t0 = NowUs();
        uint64_t t1 = t0;
        while (t1 - t0 < uint64_t(windowMs) * 1000) {
            acc = Chain(acc, 1000);
            iters += 1000;
            t1 = NowUs();
        }
        const double mhz = double(iters) * kChain / double(t1 - t0);   // cycles per microsecond
        if (mhz > best) best = mhz;
    }
    g_sink += acc;
    return best;
}

void LogClock(const char* when) {
    const double mhz = MeasureMHz(5, 20);
    const double mhz2 = MeasureLoop2M();
    const int api = scePowerGetArmClockFrequency();
    // (W5 BOOT: formatted first and written in one piece: the boot measurement now runs on a thread of its own, and stderr is unbuffered, so a
    // printf in parts was cut by the boot thread's lines)
    char line[512];
    std::snprintf(line, sizeof line, "[clock] %s: dependent-add loop %.1f MHz (best of 5 x 20 ms), subs/bne loop of 2M iterations %.1f MHz (best of 3, as VitAdhoc's mhz_measured), scePowerGetArmClockFrequency %d MHz, ratio %.3f; bus %d, GPU %d, xbar %d MHz\n", when,
                 mhz, mhz2, api, api > 0 ? mhz / api : 0.0, scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    std::fputs(line, stderr);
}

namespace {

int ClockPeriodMain(SceSize, void*) {
    const char* e = std::getenv("MSC_VITA_CLOCK_PERIOD");
    const int period = e ? std::atoi(e) : 0;
    for (int n = 1; period > 0; ++n) {
        sceKernelDelayThread(period * 1000 * 1000);
        char when[32];
        std::snprintf(when, sizeof(when), "after %d s", period * n);
        LogClock(when);
    }
    return 0;
}

} // namespace

void StartClockMonitor() {
    const char* e = std::getenv("MSC_VITA_CLOCK_PERIOD");
    if (!e || std::atoi(e) <= 0) return;
    // background priority on the audio/helper core: a 20 ms burst every period seconds
    const SceUID tid = sceKernelCreateThread("clock monitor", ClockPeriodMain, 0x10000100 + 120, 16 * 1024, 0, VitaThreads::CoreMask(VitaThreads::kHelper), nullptr);
    if (tid >= 0) sceKernelStartThread(tid, 0, nullptr);
    std::fprintf(stderr, "[clock] monitor thread %s (every %d s)\n", tid >= 0 ? "started" : "NOT created", std::atoi(e));
}

// ---- the fourth core ---------------------------------------------------------------------------------------------------------------

namespace {

constexpr int kWindows = 128;           // 10 ms windows kept per spinner
constexpr uint32_t kBurstIters = 500;   // about 72 us at 444 MHz
constexpr size_t kStreamBytes = 4u << 20;

struct Spinner {
    int core = 0;
    int mask = 0;
    bool stream = false;
    // results of the thread-creation steps
    SceUID tid = -1;
    int createRc = 0, startRc = 0, affRc = 0;
    // control
    volatile int go = 0, stop = 0, done = 0;
    // measurements
    volatile uint32_t cpuMaskSeen = 0;    // bit n: sceKernelGetCpuId() returned n at some check
    uint64_t bursts = 0, bytes = 0;
    uint64_t startUs = 0, endUs = 0;
    uint32_t maxGapUs = 0, over1ms = 0, over5ms = 0, over20ms = 0;
    uint32_t window[kWindows] = {};
    uint8_t* buffer = nullptr;
};

int SpinMain(SceSize, void* argp) {
    Spinner& s = **static_cast<Spinner**>(argp);
    uint32_t acc = 0;
    while (!s.go) sceKernelDelayThread(200);
    s.cpuMaskSeen |= 1u << (sceKernelGetCpuId() & 7);
    s.startUs = NowUs();
    uint64_t last = s.startUs;
    uint32_t check = 0;
    while (!s.stop) {
        if (s.stream) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(s.buffer);
            const uint32_t* end = p + kStreamBytes / 4;
            uint32_t sum = 0;
            for (; p < end; p += 8) sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
            acc += sum;
            s.bytes += kStreamBytes;
        } else {
            acc = Chain(acc, kBurstIters);
        }
        const uint64_t now = NowUs();
        const uint32_t gap = uint32_t(now - last);
        last = now;
        if (!s.stream) {
            if (gap > s.maxGapUs) s.maxGapUs = gap;
            if (gap > 1000) ++s.over1ms;
            if (gap > 5000) ++s.over5ms;
            if (gap > 20000) ++s.over20ms;
        }
        ++s.bursts;
        const uint64_t w = (now - s.startUs) / 10000;
        if (w < kWindows) s.window[w] += s.stream ? 1 : 1;
        if ((++check & 255) == 0) s.cpuMaskSeen |= 1u << (sceKernelGetCpuId() & 7);
    }
    s.endUs = NowUs();
    s.cpuMaskSeen |= 1u << (sceKernelGetCpuId() & 7);
    g_sink += acc;
    s.done = 1;
    return 0;
}

// Creates the thread for `core` (3 = the system core). Core 3 is tried two ways: the mask in sceKernelCreateThread, then a default thread moved
// with sceKernelChangeThreadCpuAffinityMask; both results are logged.
bool Create(Spinner& s) {
    s.mask = s.core == 3 ? SCE_KERNEL_CPU_MASK_SYSTEM : (0x10000 << s.core);
    char name[24];
    std::snprintf(name, sizeof(name), "probe%d%s", s.core, s.stream ? "m" : "");
    s.tid = sceKernelCreateThread(name, SpinMain, 0x10000100, 32 * 1024, 0, s.mask, nullptr);
    s.createRc = s.tid;
    if (s.tid < 0 && s.core == 3) {
        const SceUID t2 = sceKernelCreateThread(name, SpinMain, 0x10000100, 32 * 1024, 0, 0x70000, nullptr);
        if (t2 >= 0) {
            s.affRc = sceKernelChangeThreadCpuAffinityMask(t2, s.mask);
            if (s.affRc >= 0) {
                s.tid = t2;
            } else {
                sceKernelDeleteThread(t2);
            }
        }
    }
    if (s.tid < 0) return false;
    Spinner* self = &s;
    s.startRc = sceKernelStartThread(s.tid, sizeof(self), &self);
    return s.startRc >= 0;
}

struct Result {
    bool created = false;
    double rate = 0;          // bursts per second (or MiB per second for a stream)
    double min10 = 0, p5 = 0, median = 0;   // per 10 ms window, in the same unit per second
    uint32_t maxGapUs = 0, over1ms = 0, over5ms = 0, over20ms = 0;
    uint32_t cpuMaskSeen = 0;
};

// One phase: the listed spinners run for `ms` together; returns each one's result.
void Phase(Spinner** list, int n, int ms, Result* out) {
    for (int i = 0; i < n; ++i) {
        Spinner& s = *list[i];
        s.stop = s.done = 0;
        s.bursts = s.bytes = 0;
        s.maxGapUs = s.over1ms = s.over5ms = s.over20ms = 0;
        std::memset(s.window, 0, sizeof(s.window));
        s.go = 0;
    }
    bool ok[4] = {};
    for (int i = 0; i < n; ++i) ok[i] = Create(*list[i]);
    for (int i = 0; i < n; ++i) if (ok[i]) list[i]->go = 1;
    sceKernelDelayThread(ms * 1000);
    for (int i = 0; i < n; ++i) if (ok[i]) list[i]->stop = 1;
    for (int i = 0; i < n; ++i) {
        Spinner& s = *list[i];
        Result& r = out[i];
        r = Result();
        r.created = ok[i];
        if (!ok[i]) continue;
        int status = 0;
        SceUInt timeout = 3 * 1000 * 1000;
        sceKernelWaitThreadEnd(s.tid, &status, &timeout);
        sceKernelDeleteThread(s.tid);
        s.tid = -1;
        const double secs = double(s.endUs - s.startUs) / 1e6;
        const double units = s.stream ? double(s.bytes) / (1 << 20) : double(s.bursts);
        r.rate = secs > 0 ? units / secs : 0;
        // windows: all full windows (the last one is partial)
        const int full = int(s.endUs - s.startUs) / 10000;
        uint32_t w[kWindows];
        int m = 0;
        for (int k = 1; k < full - 1 && k < kWindows; ++k) w[m++] = s.window[k];   // skip the first (thread start) and the last (partial) windows
        for (int a = 1; a < m; ++a) { uint32_t v = w[a]; int b = a - 1; while (b >= 0 && w[b] > v) { w[b + 1] = w[b]; --b; } w[b + 1] = v; }
        if (m > 0) {
            const double per = s.stream ? double(kStreamBytes) / (1 << 20) : 1.0;
            r.min10 = w[0] * per * 100.0;               // per second
            r.p5 = w[m * 5 / 100] * per * 100.0;
            r.median = w[m / 2] * per * 100.0;
        }
        r.maxGapUs = s.maxGapUs; r.over1ms = s.over1ms; r.over5ms = s.over5ms; r.over20ms = s.over20ms;
        r.cpuMaskSeen = s.cpuMaskSeen;
    }
}

} // namespace

void RunCore4Probe() {
    const char* e = std::getenv("MSC_VITA_PROBE_CORE4");
    if (!e || !*e || *e == '0') return;
    const bool only = !std::strcmp(e, "only");
    const int ms = [] { const char* t = std::getenv("MSC_VITA_PROBE_MS"); const int v = t ? std::atoi(t) : 0; return v >= 200 ? v : 1000; }();
    std::fprintf(stderr, "[core4] ===== fourth-core probe (%d ms per phase; SCE_KERNEL_CPU_MASK_SYSTEM = 0x%X) =====\n", ms, static_cast<unsigned>(SCE_KERNEL_CPU_MASK_SYSTEM));
    LogClock("probe start");
    static Spinner sp[4], sm[4];
    for (int c = 0; c < 4; ++c) { sp[c].core = c; sm[c].core = c; sm[c].stream = true; }
    Result r[4];
    double alone[4] = {};
    // A: each core alone
    for (int c = 0; c < 4; ++c) {
        Spinner* l[1] = { &sp[c] };
        Phase(l, 1, ms, r);
        alone[c] = r[0].rate;
        if (!r[0].created) {
            std::fprintf(stderr, "[core4] core %d: thread NOT usable: create 0x%X start 0x%X affinity-change 0x%X\n", c, static_cast<unsigned>(sp[c].createRc),
                         static_cast<unsigned>(sp[c].startRc), static_cast<unsigned>(sp[c].affRc));
            continue;
        }
        std::fprintf(stderr, "[core4] core %d alone: mask 0x%X (create 0x%X, affinity-change 0x%X), reported cores seen 0x%X, %.0f bursts/s, 10 ms windows: min %.0f p5 %.0f median %.0f, "
                     "longest gap %u us, gaps >1 ms %u, >5 ms %u, >20 ms %u\n", c, static_cast<unsigned>(sp[c].mask), static_cast<unsigned>(sp[c].createRc),
                     static_cast<unsigned>(sp[c].affRc), r[0].cpuMaskSeen, r[0].rate, r[0].min10, r[0].p5, r[0].median, r[0].maxGapUs, r[0].over1ms, r[0].over5ms, r[0].over20ms);
    }
    double userRate = 0;
    int users = 0;
    for (int c = 0; c < 3; ++c) if (alone[c] > 0) { userRate += alone[c]; ++users; }
    userRate = users ? userRate / users : 0;
    const bool have3 = alone[3] > 0;
    const double share3 = (have3 && userRate > 0) ? alone[3] / userRate : 0;
    if (have3) {
        std::fprintf(stderr, "[core4] core 3 alone: %.1f %% of a user core (mean of cores 0-2: %.0f bursts/s), %s\n", 100.0 * share3, userRate,
                     (sp[3].cpuMaskSeen & 8) && !(sp[3].cpuMaskSeen & 7) ? "sceKernelGetCpuId() says core 3" : "sceKernelGetCpuId() does NOT say core 3 only");
    }
    // B: cores 0-2 together; C: all four
    Result three[3], four[4];
    { Spinner* l[3] = { &sp[0], &sp[1], &sp[2] }; Phase(l, 3, ms, three); }
    if (have3) { Spinner* l[4] = { &sp[0], &sp[1], &sp[2], &sp[3] }; Phase(l, 4, ms, four); }
    double worst = 1.0;
    for (int c = 0; c < 3; ++c) {
        const double w3 = three[c].rate / (alone[c] > 0 ? alone[c] : 1);
        std::fprintf(stderr, "[core4] core %d with cores 0-2 busy: %.1f %% of alone", c, 100.0 * w3);
        if (have3) {
            const double w4 = four[c].rate / (alone[c] > 0 ? alone[c] : 1);
            std::fprintf(stderr, ", with core 3 busy as well: %.1f %% of alone (min 10 ms window %.0f against median %.0f), longest gap %u us", 100.0 * w4, four[c].min10, four[c].median, four[c].maxGapUs);
            if (w4 / (w3 > 0 ? w3 : 1) < worst) worst = w4 / (w3 > 0 ? w3 : 1);
        }
        std::fprintf(stderr, "\n");
    }
    if (have3)
        std::fprintf(stderr, "[core4] core 3 with all cores busy: %.1f %% of a user core, 10 ms windows min %.0f median %.0f, longest gap %u us (>5 ms: %u, >20 ms: %u)\n",
                     100.0 * four[3].rate / (userRate > 0 ? userRate : 1), four[3].min10, four[3].median, four[3].maxGapUs, four[3].over5ms, four[3].over20ms);
    // D: memory streams
    double streamWorst = 1.0;
    bool streams = true;
    if (const char* t = std::getenv("MSC_VITA_PROBE_MEM")) streams = *t != '0';
    if (streams) {
        for (int c = 0; c < 4; ++c) {
            sm[c].buffer = static_cast<uint8_t*>(std::malloc(kStreamBytes));
            if (!sm[c].buffer) { streams = false; break; }
            std::memset(sm[c].buffer, 1, kStreamBytes);
        }
    }
    if (streams) {
        Result s3[3], s4[4], s1[1];
        { Spinner* l[1] = { &sm[2] }; Phase(l, 1, ms, s1); }
        { Spinner* l[3] = { &sm[0], &sm[1], &sm[2] }; Phase(l, 3, ms, s3); }
        double t3 = s3[0].rate + s3[1].rate + s3[2].rate;
        std::fprintf(stderr, "[core4] memory streams (4 MiB read per core): core 2 alone %.0f MiB/s; cores 0-2 together %.0f + %.0f + %.0f = %.0f MiB/s\n", s1[0].rate, s3[0].rate, s3[1].rate, s3[2].rate, t3);
        if (have3) {
            { Spinner* l[4] = { &sm[0], &sm[1], &sm[2], &sm[3] }; Phase(l, 4, ms, s4); }
            const double t4 = s4[0].rate + s4[1].rate + s4[2].rate + s4[3].rate;
            std::fprintf(stderr, "[core4] memory streams, four cores: %.0f + %.0f + %.0f + %.0f = %.0f MiB/s; cores 0-2 keep %.1f %% of their three-core rate, core 3 gets %.0f MiB/s\n",
                         s4[0].rate, s4[1].rate, s4[2].rate, s4[3].rate, t4, t3 > 0 ? 100.0 * (s4[0].rate + s4[1].rate + s4[2].rate) / t3 : 0.0, s4[3].rate);
            if (t3 > 0) streamWorst = (s4[0].rate + s4[1].rate + s4[2].rate) / t3;
        }
        for (int c = 0; c < 4; ++c) { std::free(sm[c].buffer); sm[c].buffer = nullptr; }
    }
    // verdict
    const char* v;
    if (!have3) v = "NOT usable (no thread could be put on the system core)";
    else if (!(sp[3].cpuMaskSeen & 8)) v = "NOT usable (the thread runs, but sceKernelGetCpuId() never said core 3)";
    else if (share3 >= 0.9 && four[3].over20ms == 0) v = "usable as a full lane (job pool: MSC_JOBS=4 MSC_JOBS_CORES=213)";
    else if (share3 >= 0.3) v = "usable as a quota lane only (long jobs would stall: keep jobs under a millisecond; MSC_JOBS=4 MSC_JOBS_CORES=213 is an experiment)";
    else v = "NOT worth a lane (the system leaves too little of the core)";
    std::fprintf(stderr, "[core4] verdict: core 3 gives %.1f %% of a user core, the three game cores keep %.1f %% (scheduling) / %.1f %% (memory streams) when it is busy: %s\n",
                 100.0 * share3, 100.0 * (have3 ? worst : 1.0), 100.0 * streamWorst, v);
    std::fprintf(stderr, "[core4] ===== probe done =====\n");
    VitaLog::Flush();
    if (only) {
        std::fprintf(stderr, "[core4] MSC_VITA_PROBE_CORE4=only: ending the process\n");
        VitaLog::Flush();
        sceKernelExitProcess(0);
    }
}

#else   // not the Vita: nothing to measure (the armhf check build only compiles the roles)
double MeasureMHz(uint32_t, uint32_t) { return 0.0; }
void LogClock(const char*) {}
void StartClockMonitor() {}
void RunCore4Probe() {}
#endif

} // namespace VitaProbe
