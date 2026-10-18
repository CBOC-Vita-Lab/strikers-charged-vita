// The smoke test of the native port's platform layer: a "game" (np_game_main) that exercises what
// the real one will need from the Vita and measures what the performance plan rests on, without
// any game code. Built as np-smoke.vpk (title MSCN00002, lab mode on): the coordinator runs it
// once on the console (`vitalab.py cycle env.txt smoke1 120`) and gets, in log.txt:
//   [smoke] memory      free memory in the three budgets, an 8 MiB arena
//   [smoke] cores       the same fixed work alone on core 0, then on 0+1, 0+1+2 at the same time:
//                       do three cores each deliver the single-core rate (shared L2, bus)?
//   [smoke] jobs        the job pool's fork-join: speed-up with 1, 2, 3 lanes, wake latency, and with
//                       render-like and audio-like threads loading cores 1 and 2 with the workers
//                       above / equal to / below their priority
//   [smoke] audio       a second of tone through the output thread
//   [smoke] frames      frames through NpOnFramePresented (harness, scripts, MSC_MAX_FRAMES)
// MSC_SMOKE_SKIP=cores,jobs,audio,frames skips parts; MSC_SMOKE_FRAMES=N sets the frame count.
#include "np_vita.h"
#include "np_vita_internal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <pthread.h>
#include <atomic>
#include <vector>

#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif

int np_game_main();
extern "C" int NpVitaGameDataOptional() { return 1; }  // the smoke application needs no disc

namespace {

bool Skip(const char* what) {
    const char* e = std::getenv("MSC_SMOKE_SKIP");
    return e && std::strstr(e, what);
}
double Now() { return NpRealSeconds(); }

// A fixed amount of mixed integer / float work on a small (L1-resident) array: about
// 7 instructions per iteration.
float g_sink;
float Work(uint32_t iterations) {
    float a[256];
    for (int i = 0; i < 256; ++i) a[i] = 0.5f + i * 0.001f;
    float acc = 1.0f;
    uint32_t h = 12345;
    for (uint32_t i = 0; i < iterations; ++i) {
        h = h * 1664525u + 1013904223u;
        const float x = a[(h >> 24) & 255];
        acc = acc * 0.99999f + x * 0.00001f;
        a[(h >> 16) & 255] = acc;
    }
    return acc;
}
// The same, over a 2 MiB array (L2-sized): a memory-bound counterpart.
float WorkL2(uint32_t iterations) {
    static std::vector<uint32_t> big(512 * 1024);
    uint32_t h = 777, s = 0;
    for (uint32_t i = 0; i < iterations; ++i) {
        h = h * 1664525u + 1013904223u;
        s += big[(h >> 8) & (big.size() - 1)];
        big[(h >> 12) & (big.size() - 1)] = s;
    }
    return static_cast<float>(s & 0xFFFF);
}

void PinTo(int core) {
#if defined(__vita__)
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000 << core);
#else
    (void)core;
#endif
}

struct CoreRun { int core; uint32_t iters; bool l2; std::atomic<int>* go; double seconds; };
void* CoreThread(void* p) {
    CoreRun& r = *static_cast<CoreRun*>(p);
    PinTo(r.core);
    while (!r.go->load()) {}
    const double t0 = Now();
    g_sink = r.l2 ? WorkL2(r.iters) : Work(r.iters);
    r.seconds = Now() - t0;
    return nullptr;
}
// Runs the work on the given cores at the same time (the calling thread is one of them), returns the seconds of each.
std::vector<double> RunOnCores(const std::vector<int>& cores, uint32_t iters, bool l2) {
    std::atomic<int> go{0};
    std::vector<CoreRun> runs(cores.size());
    std::vector<pthread_t> th(cores.size());
    for (size_t i = 0; i < cores.size(); ++i) runs[i] = {cores[i], iters, l2, &go, 0.0};
    for (size_t i = 1; i < cores.size(); ++i) pthread_create(&th[i], nullptr, CoreThread, &runs[i]);
    PinTo(cores[0]);
    NpSleepNs(20 * 1000 * 1000);   // let the others reach their start line
    go.store(1);
    const double t0 = Now();
    g_sink = l2 ? WorkL2(iters) : Work(iters);
    runs[0].seconds = Now() - t0;
    std::vector<double> out{runs[0].seconds};
    for (size_t i = 1; i < cores.size(); ++i) { pthread_join(th[i], nullptr); out.push_back(runs[i].seconds); }
    return out;
}

void Memory() {
    std::fprintf(stderr, "[smoke] memory: ");
#if defined(__vita__)
    SceKernelFreeMemorySizeInfo f;
    std::memset(&f, 0, sizeof(f));
    f.size = sizeof(f);
    if (sceKernelGetFreeMemorySize(&f) >= 0)
        std::fprintf(stderr, "free main %d KiB, cdram %d KiB, phycont %d KiB; ", f.size_user / 1024, f.size_cdram / 1024, f.size_phycont / 1024);
#endif
    void* arena = NpAllocArena(8u << 20, 4096, "smoke_arena");
    if (arena) {
        std::memset(arena, 0xCD, 8u << 20);
        std::fprintf(stderr, "8 MiB arena at %p ok\n", arena);
    } else std::fprintf(stderr, "8 MiB arena FAILED\n");
}

void Cores() {
    const uint32_t iters = 3000000;
    for (int l2 = 0; l2 < 2; ++l2) {
        const char* kind = l2 ? "L2-bound" : "L1-bound";
        const uint32_t n = l2 ? iters / 3 : iters;
        const auto solo = RunOnCores({0}, n, l2);
        const auto two = RunOnCores({0, 1}, n, l2);
        const auto three = RunOnCores({0, 1, 2}, n, l2);
        const auto two02 = RunOnCores({0, 2}, n, l2);
        const double mx2 = *std::max_element(two.begin(), two.end()), mx3 = *std::max_element(three.begin(), three.end());
        std::fprintf(stderr, "[smoke] cores (%s): alone %.1f ms; cores 0+1 together %.1f/%.1f ms (slowest %.0f%% of alone); cores 0+2 %.1f/%.1f ms; "
                             "cores 0+1+2 together %.1f/%.1f/%.1f ms (slowest %.0f%% of alone)\n",
                     kind, solo[0] * 1e3, two[0] * 1e3, two[1] * 1e3, 100.0 * mx2 / solo[0], two02[0] * 1e3, two02[1] * 1e3,
                     three[0] * 1e3, three[1] * 1e3, three[2] * 1e3, 100.0 * mx3 / solo[0]);
    }
}

// ---- job pool
struct JobCtx { uint32_t itersPerItem; };
void JobFn(void* ctx, size_t b, size_t e, uint32_t) {
    JobCtx& c = *static_cast<JobCtx*>(ctx);
    for (size_t i = b; i < e; ++i) g_sink = Work(c.itersPerItem);
}
struct Burner { int core; uint32_t burstUs, gapUs; std::atomic<bool>* stop; int prio; };
void* BurnerThread(void* p) {
    Burner& b = *static_cast<Burner*>(p);
    PinTo(b.core);
#if defined(__vita__)
    if (b.prio) sceKernelChangeThreadPriority(sceKernelGetThreadId(), b.prio);
#endif
    while (!b.stop->load()) {
        const double end = Now() + b.burstUs * 1e-6;
        while (Now() < end) g_sink = Work(2000);
        NpSleepNs(static_cast<int64_t>(b.gapUs) * 1000);
    }
    return nullptr;
}
struct Dist { double mean, p95, max; };
Dist Measure(int calls, size_t items, uint32_t itersPerItem) {
    JobCtx c{itersPerItem};
    std::vector<double> t;
    for (int i = 0; i < calls; ++i) {
        const double t0 = Now();
        NpParallelFor(items, 1, JobFn, &c);
        t.push_back((Now() - t0) * 1e3);
        NpSleepNs(2 * 1000 * 1000);   // a frame has other work between its jobs
    }
    std::sort(t.begin(), t.end());
    double s = 0;
    for (double x : t) s += x;
    return {s / t.size(), t[t.size() * 95 / 100], t.back()};
}
void Jobs() {
    // 10 items of about 0.4 ms each (ten characters being posed), 100 calls per configuration
    const uint32_t per = 900000;
    {
        const double t0 = Now(); Work(per); const double one = (Now() - t0) * 1e3;
        std::fprintf(stderr, "[smoke] jobs: one item = %.2f ms of work\n", one);
    }
    for (uint32_t lanes = 1; lanes <= 3; ++lanes) {
        NpJobsShutdown();
        char v[8]; std::snprintf(v, sizeof(v), "%u", lanes);
        setenv("MSC_JOBS", v, 1);
        NpJobsInit(0);
        const Dist d = Measure(100, 10, per);
        std::fprintf(stderr, "[smoke] jobs: %u lane(s) idle machine: 10 items per call: mean %.2f ms, p95 %.2f, max %.2f\n", lanes, d.mean, d.p95, d.max);
        NpJobsLogStats("lane test");
    }
    // render-like (core 1: 14 ms of work every 33 ms) and audio-like (core 2: 4 ms every 16 ms) threads
    std::atomic<bool> stop{false};
    Burner render{1, 14000, 19000, &stop, 0}, audio{2, 4000, 12000, &stop, 0};
    pthread_t tr, ta;
    pthread_create(&tr, nullptr, BurnerThread, &render);
    pthread_create(&ta, nullptr, BurnerThread, &audio);
    const struct { const char* name; const char* prio; } prios[] = {
        {"above the render thread (default plan: default - 16)", nullptr}, {"equal to the render thread", "0x10000100"}, {"below the render thread", "0x10000130"}};
    for (const auto& p : prios) {
        NpJobsShutdown();
        setenv("MSC_JOBS", "3", 1);
        if (p.prio) setenv("MSC_JOBS_PRIO", p.prio, 1); else unsetenv("MSC_JOBS_PRIO");
        NpJobsInit(0);
        const Dist d = Measure(100, 10, per);
        std::fprintf(stderr, "[smoke] jobs: 3 lanes with render+audio-like load on cores 1 and 2, workers %s: mean %.2f ms, p95 %.2f, max %.2f\n", p.name, d.mean, d.p95, d.max);
        NpJobsLogStats("under load");
    }
    stop.store(true);
    pthread_join(tr, nullptr);
    pthread_join(ta, nullptr);
    NpJobsShutdown();
    unsetenv("MSC_JOBS_PRIO");
    unsetenv("MSC_JOBS");
    NpJobsInit(0);
}

void Audio() {
    if (!NpAudioOpen(32000)) { std::fprintf(stderr, "[smoke] audio: open FAILED\n"); return; }
    NpAudioSetVolume(0.2f);
    std::vector<int16_t> block(96 * 2);   // 3 ms
    double phase = 0;
    int pushed = 0, dropped = 0;
    const double t0 = Now();
    for (int i = 0; i < 330; ++i) {      // 330 blocks = about 1 s
        for (int f = 0; f < 96; ++f) {
            const int16_t v = static_cast<int16_t>(8000 * std::sin(phase));
            phase += 2 * 3.14159265 * 440.0 / 32000.0;
            block[f * 2] = block[f * 2 + 1] = v;
        }
        if (NpAudioPushLE16(block.data(), 96)) ++pushed; else ++dropped;
        NpSleepNs(3 * 1000 * 1000);
    }
    std::fprintf(stderr, "[smoke] audio: 440 Hz tone, %d blocks pushed, %d dropped, %.2f s, space now %d frames\n", pushed, dropped, Now() - t0, NpAudioSpaceFrames());
    VitaAudio::LogReport("the audio test");
}

void Frames() {
    const char* e = std::getenv("MSC_SMOKE_FRAMES");
    const int n = e ? std::atoi(e) : 90;
    const double t0 = Now();
    uint32_t lastButtons = 0;
    for (int f = 0; f < n; ++f) {
        NpVitaInput in;
        if (NpVitaReadInput(&in) && in.buttons != lastButtons) {
            std::fprintf(stderr, "[smoke] input: buttons %08X sticks %d,%d %d,%d touches %d\n", in.buttons, in.lx, in.ly, in.rx, in.ry, in.touchCount);
            lastButtons = in.buttons;
        }
        NpPadState pad{};
        NpVitaMapGcPad(&in, &pad);
        NpHarnessFillPad(&pad);
        g_sink = Work(200000);                 // some "frame work"
        NpSleepNs(30 * 1000 * 1000);
        NpOnFramePresented();
    }
    std::fprintf(stderr, "[smoke] frames: %d frames through NpOnFramePresented in %.2f s\n", n, Now() - t0);
}

} // namespace

int np_game_main() {
    std::fprintf(stderr, "[smoke] ===== NP platform smoke test =====\n");
#if defined(__vita__)
    std::fprintf(stderr, "[smoke] CPU %d MHz, jobs lanes %u\n", NpVitaCpuMHz(), NpJobsLanes());
#endif
    Memory();
    if (!Skip("cores")) Cores();
    if (!Skip("jobs")) Jobs();
    if (!Skip("audio")) Audio();
    if (!Skip("frames")) Frames();
    std::fprintf(stderr, "[smoke] ===== done =====\n");
    return 0;
}
