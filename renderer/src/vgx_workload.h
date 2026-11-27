// Per-lane cost of a picture, the way the Galaxy port's vgx_workload.h judged its runs: one log line, cheap enough to leave on in a logging build.
// (Strikers has its own threads, so this is a new file with Galaxy's names, not a copy: no material lists, no split front end.)
//
//   MSC_VGX_WORKLOAD_PROBE=0|1  (Vita logging build 1, host 0) one "[lane]" line every 300 pictures, ms per picture:
//       [lane] 300 pics 6.01s 49.9/s game 14.2 send 3.1 fe 12.4 be 7.3/9.8 gpu 18.2 cores 71/63/58/4 dc 612 soft 9120 static 210 th 3
//     game   busy time of the game's core, without the threads that follow time (audio, timers): NpFpsPlatformProbe, one read per line
//     send   CPU of the thread that records the frame (rec::Present: the frame-send worker under core plan C, else the game thread)
//     fe     CPU of the front end's thread, without the back end when both share one thread (th 2)
//     be     back end, CPU / wall (wall includes its waits for the GPU queue and the vblank), sampled on one picture in 8
//     gpu    GPU busy time of the newest finished frame, sampled with be ("-": the timers are off)
//     cores  percent busy per core over the window; dc / soft / static: draw records, CPU-transformed vertices, static draws per picture
//     th     3: front end and back end on threads of their own, 2: on one thread, 0: no back-end sample yet
//     "-" means no measurement in this build or mode. The columns overlap in time and are NOT additive: the limit is the largest lane, not their sum.
//   MSC_VGX_FE_SPLIT=0|1  (0; with the probe) where the front end's time goes, by wall time on its own thread, on one picture in 8 (a clock read per scope, so
//     the sampled pictures are a little slower than the rest), plus the display-list cache's branches and why dynamic draws did not merge. Three more lines.
//
// A build with NP_LOGGING=0 compiles all of this to nothing: Enabled() is a constant false and the scopes are empty.
#pragma once
#include "vgx_gpubusy.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#if !defined(__vita__)
#include <chrono>
#include <ctime>
#endif
#include "vgx_common.h"

#if defined(NP_LOGGING) && !NP_LOGGING
#define VGX_WORKLOAD_BUILD 0
#else
#define VGX_WORKLOAD_BUILD 1
#endif

// All weak: the host harnesses link none of them, and a column without its source prints "-".
extern "C" {
double NpFpsPlatformThreadClockMs(void) __attribute__((weak));
int NpFpsPlatformProbe(void* out) __attribute__((weak));        // (np/fps.h; `out` is CoreProbe below)
uint32_t vglRtLastGpuFrame(void* out) __attribute__((weak));    // (vitaGL.h; `out` is GpuFrame below)
int NpLateCauseOn(void) __attribute__((weak));                  // lessons17 (np_fps.cpp, MSC_LATE_CAUSE): the per-picture lane times for the "[late]" line
void NpLateLaneNote(int lane, float ms) __attribute__((weak));  // (0 front end, 1 render thread, 2 GPU)
#if defined(__vita__)
unsigned int sceKernelGetProcessTimeLow(void);
#endif
}

namespace vgx {
void DlCacheBranchCounts(uint64_t out[9]) __attribute__((weak)); // (vgx_dlcache.cpp; weak: a test of this header alone does not link it)
}

namespace vgx::workload {

struct CoreProbe { double cpuMs[4]; double timeMs[4]; int gameCore; }; // the layout of NpFpsProbeOut (np/fps.h)
struct GpuFrame { uint32_t busy_us, span_us, tail_us; uint16_t scenes, timed; uint32_t pixel_us, tiling_us, display_us; }; // the layout of vglRtGpuFrame (vitaGL.h; the library writes all of it)

constexpr uint32_t kWindow = 300;     // pictures per line
constexpr uint32_t kBackendStride = 8, kFeStride = 8, kProducerStride = 64;

enum FeSlot : unsigned { FeTiny, FeGeomHit, FeOther, FeStream, FeDecode, FeEmit, FeResolve, FeSoft, FeRecord, FeStatic, FeFrameEnd, FeHandOff, FeSlotCount };
inline constexpr const char* FeNames[] = {"list-tiny", "list-hit", "list-other", "parser", "decode", "emit", "resolve-state", "cpu-transform", "merge-record",
                                          "static-draw", "frame-end", "hand-off"};
// Dynamic draws by whether they joined the record before them, and if not the first merge test that failed.
enum MergeWhy : unsigned { MergeJoined, MergeNoPrev, MergeStatic, MergeLayout, MergeShader, MergeVerts, MergeBlocks, MergeRs, MergeTex, MergeOther, MergeWhyCount };
inline constexpr const char* MergeNames[] = {"joined", "no-prev-draw", "prev-static", "prim-layout", "shader", "vertices", "uniform-blocks", "render-state", "textures", "other"};

// What one line says. Negative: not measured. The formatter is a function of this alone, for the test.
struct Line {
    uint32_t pictures = 0;
    double seconds = 0, game = -1, send = -1, fe = -1, beCpu = -1, beWall = -1, gpu = -1;
    double core[4] = {-1, -1, -1, -1};
    double drawCalls = 0, softVerts = 0, staticDraws = 0;
    int threads = 0;
};
inline int FormatColumn(char* out, size_t cap, const char* name, double v) {
    return v < 0 ? std::snprintf(out, cap, " %s -", name) : std::snprintf(out, cap, " %s %.1f", name, v);
}
inline size_t FormatLine(const Line& l, char* out, size_t cap) {
    size_t n = 0;
    const auto room = [&] { return n < cap ? cap - n : 0; };
    const auto add = [&](int w) { if (w > 0) n += static_cast<size_t>(w); };
    add(std::snprintf(out, cap, "[lane] %u pics %.2fs %.1f/s", l.pictures, l.seconds, l.seconds > 0 ? l.pictures / l.seconds : 0.0));
    add(FormatColumn(out + n, room(), "game", l.game));
    add(FormatColumn(out + n, room(), "send", l.send));
    add(FormatColumn(out + n, room(), "fe", l.fe));
    if (l.beCpu < 0) add(std::snprintf(out + n, room(), " be -"));
    else add(std::snprintf(out + n, room(), " be %.1f/%.1f", l.beCpu, l.beWall));
    add(FormatColumn(out + n, room(), "gpu", l.gpu));
    if (l.core[0] < 0) add(std::snprintf(out + n, room(), " cores -"));
    else add(std::snprintf(out + n, room(), " cores %.0f/%.0f/%.0f/%.0f", l.core[0], l.core[1], l.core[2], l.core[3]));
    add(std::snprintf(out + n, room(), " dc %.0f soft %.0f static %.0f th %d", l.drawCalls, l.softVerts, l.staticDraws, l.threads));
    return n < cap ? n : (cap ? cap - 1 : 0);
}
// The front end's CPU without the back end when they share a thread (never below zero: the back end is a sample, the thread's time is a total).
inline double FeOnly(double threadMsPerPicture, double beCpuMsPerPicture, bool sameThread) {
    if (!sameThread || beCpuMsPerPicture < 0) return threadMsPerPicture;
    return threadMsPerPicture > beCpuMsPerPicture ? threadMsPerPicture - beCpuMsPerPicture : 0.0;
}

#if VGX_WORKLOAD_BUILD

inline bool Enabled() {
#if defined(__vita__)
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_WORKLOAD_PROBE", 1) != 0 || ab::Requested()));
#else
    const bool on = static_cast<bool>(VGX_KNOB(EnvInt("MSC_VGX_WORKLOAD_PROBE", 0) != 0 || ab::Requested()));
#endif
    return on;
}
inline bool FeSplitOn() {
    const bool on = static_cast<bool>(VGX_KNOB(Enabled() && EnvInt("MSC_VGX_FE_SPLIT", 0) != 0));
    return on;
}
// Microseconds. (Not steady_clock on the console: a system call and a 64-bit divide, about 6.7 us, vgx_backend_gl.cpp ClockCostMs.)
inline uint32_t NowUs() {
#if defined(__vita__)
    return sceKernelGetProcessTimeLow();
#else
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
// CPU of the calling thread, ms; negative: no clock.
inline double ThreadCpuMs() {
    if (&NpFpsPlatformThreadClockMs) return NpFpsPlatformThreadClockMs();
#if defined(__vita__)
    return -1.0;
#else
    timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) return -1.0;
    return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) * 1e-6;
#endif
}

// ---- front-end thread only
inline bool FeSampling = false;     // this picture is timed
inline uint64_t FeUs[FeSlotCount];
inline uint32_t FeN[FeSlotCount];
inline uint32_t FeSampled = 0;
inline uint32_t MergeCount[MergeWhyCount];
struct FeScope {
    int slot; uint32_t t;
    explicit FeScope(unsigned s) : slot(FeSampling ? static_cast<int>(s) : -1), t(slot >= 0 ? NowUs() : 0) {}
    ~FeScope() {
        if (slot < 0) return;
        FeUs[slot] += NowUs() - t;
        ++FeN[slot];
    }
};
inline void MergeNote(unsigned why) { ++MergeCount[why]; }

// ---- back-end thread: one picture in kBackendStride
inline std::atomic<uint64_t> BeCpuUs{0}, BeWallUs{0}, GpuUs{0};
inline std::atomic<uint32_t> BeSamples{0}, GpuSamples{0};
inline std::atomic<bool> BeSeen{false};
inline pthread_t BeThread; // (written once, before BeSeen)
struct BackendScope {
    bool on = false; double c0 = 0; uint32_t w0 = 0;
    BackendScope() {
        static uint32_t n = 0;
        static uint32_t lastGpu = 0;
        if (!Enabled() || n++ % kBackendStride != 0) return;
        on = true;
        if (!BeSeen.load(std::memory_order_relaxed)) { BeThread = pthread_self(); BeSeen.store(true, std::memory_order_release); }
        c0 = ThreadCpuMs();
        w0 = NowUs();
        if (&vglRtLastGpuFrame) {
            GpuFrame f;
            const uint32_t seq = vglRtLastGpuFrame(&f);
            if (seq && seq != lastGpu && f.timed) {
                lastGpu = seq;
                GpuUs.fetch_add(vgx::gpubusy::CorrectedUs(vgx::gpubusy::Config(), f.busy_us, f.display_us, f.scenes), std::memory_order_relaxed);   // wave 10: work time, not the display scene's wait for a buffer (vgx_gpubusy.h)
                GpuSamples.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ~BackendScope() {
        if (!on) return;
        const uint32_t wall = NowUs() - w0;
        const double c1 = ThreadCpuMs();
        if (c0 >= 0 && c1 >= c0) BeCpuUs.fetch_add(static_cast<uint64_t>((c1 - c0) * 1e3), std::memory_order_relaxed);
        BeWallUs.fetch_add(wall, std::memory_order_relaxed);
        BeSamples.fetch_add(1, std::memory_order_release);
    }
};
// ---- lessons17 (MSC_LATE_CAUSE, np_late_cause.h): the lanes of EVERY picture, for the game thread's "[late]" line. Two thread-clock reads and one GPU-frame poll a picture on the back-end thread, one thread-clock read a picture on the
// front-end thread; what they cost is counted (LateCostUs / LateReads) and printed with the line.
inline bool LateOn() {
    static const bool on = &NpLateCauseOn != nullptr && NpLateCauseOn() != 0 && &NpLateLaneNote != nullptr;
    return on;
}
inline std::atomic<uint64_t> LateCostUs{0}, LateBeAccUs{0};   // (LateBeAccUs: the back end's CPU since the front end's last picture, to take it off the front end when they share a thread)
inline std::atomic<uint32_t> LateReads{0}, LatePictures{0};
struct LateScope {
    bool on = false; double c0 = 0; uint32_t w0 = 0;
    LateScope() {
        if (!LateOn()) return;
        w0 = NowUs();
        c0 = ThreadCpuMs();
        on = true;
        static uint32_t lastGpu = 0;
        if (&vglRtLastGpuFrame) {
            GpuFrame f;
            const uint32_t seq = vglRtLastGpuFrame(&f);
            if (seq && seq != lastGpu && f.timed) {
                lastGpu = seq;
                NpLateLaneNote(2, static_cast<float>(vgx::gpubusy::CorrectedUs(vgx::gpubusy::Config(), f.busy_us, f.display_us, f.scenes)) * 1e-3f);
            }
        }
        LateCostUs.fetch_add(NowUs() - w0, std::memory_order_relaxed);
    }
    ~LateScope() {
        if (!on) return;
        const uint32_t w1 = NowUs();
        const double c1 = ThreadCpuMs();
        if (c0 >= 0 && c1 >= c0) { NpLateLaneNote(1, static_cast<float>(c1 - c0)); LateBeAccUs.fetch_add(static_cast<uint64_t>((c1 - c0) * 1e3), std::memory_order_relaxed); }
        LateCostUs.fetch_add(NowUs() - w1, std::memory_order_relaxed);
        LateReads.fetch_add(2, std::memory_order_relaxed);
        LatePictures.fetch_add(1, std::memory_order_relaxed);
    }
};
inline double LateFeCpu = -1.0;   // (front-end thread only)
inline void LateFePicture() {
    if (!LateOn()) return;
    const uint32_t w0 = NowUs();
    const double c = ThreadCpuMs();
    const uint64_t be = LateBeAccUs.exchange(0, std::memory_order_relaxed);
    if (c >= 0 && LateFeCpu >= 0 && c >= LateFeCpu) {
        double fe = c - LateFeCpu;
        const bool same = BeSeen.load(std::memory_order_acquire) && pthread_equal(BeThread, pthread_self());
        if (same) { fe -= static_cast<double>(be) * 1e-3; if (fe < 0) fe = 0; }
        NpLateLaneNote(0, static_cast<float>(fe));
    }
    LateFeCpu = c;
    LateCostUs.fetch_add(NowUs() - w0, std::memory_order_relaxed);
    LateReads.fetch_add(1, std::memory_order_relaxed);
}
// The cost per picture of the back-end side and the number of clock reads since the last call (the "[late]" line).
inline bool LateCostTake(double* usPerPicture, unsigned long* reads) {
    const uint32_t n = LatePictures.exchange(0, std::memory_order_relaxed);
    const uint64_t us = LateCostUs.exchange(0, std::memory_order_relaxed);
    *reads = LateReads.exchange(0, std::memory_order_relaxed);
    if (!n) return false;
    *usPerPicture = static_cast<double>(us) / n;
    return true;
}

// ---- the recording thread (rec::Present), one clock read per kProducerStride presents
inline std::atomic<uint64_t> SendUs{0};
inline std::atomic<uint32_t> SendPresents{0};
inline void ProducerTick() {
    if (!Enabled()) return;
    static uint32_t n = 0;
    static double last = -1.0;
    static pthread_t who;
    if (++n < kProducerStride) return;
    const double now = ThreadCpuMs();
    const pthread_t me = pthread_self();
    if (last >= 0 && now >= last && pthread_equal(me, who)) { // (a change of thread between two reads: this stretch is not counted)
        SendUs.fetch_add(static_cast<uint64_t>((now - last) * 1e3), std::memory_order_relaxed);
        SendPresents.fetch_add(n, std::memory_order_relaxed);
    }
    last = now;
    who = me;
    n = 0;
}

// ---- once per picture, front-end thread (FrontPresent, before the frame's statistics are reset)
struct WindowState {
    uint32_t pictures = 0, timedStart = 0;
    double cpu0 = -1.0;
    bool started = false;
    double drawCalls = 0, softVerts = 0, staticDraws = 0;
    double coreCpu[4] = {}, coreTime[4] = {};
    bool haveCores = false;
    uint64_t dl[9] = {};
    uint32_t feRound = 0;
};
inline WindowState Win;

inline void ReportSplit(uint32_t pictures) {
    const double n = FeSampled ? static_cast<double>(FeSampled) : 1.0;
    char line[640];
    size_t at = 0;
    for (unsigned i = 0; i < FeSlotCount; ++i) {
        const int w = std::snprintf(line + at, sizeof line - at, " %s=%.2f(%u)", FeNames[i], static_cast<double>(FeUs[i]) / 1e3 / n, static_cast<unsigned>(FeN[i] / n));
        if (w > 0 && at + static_cast<size_t>(w) < sizeof line) at += static_cast<size_t>(w);
        FeUs[i] = 0;
        FeN[i] = 0;
    }
    std::fprintf(stderr, "[lane-fe] ms per picture over %u timed pictures (wall, nested scopes overlap; calls in brackets):%s\n", FeSampled, line);
    FeSampled = 0;
    uint64_t dl[9] = {};
    if (&DlCacheBranchCounts) DlCacheBranchCounts(dl);
    const double p = pictures ? static_cast<double>(pictures) : 1.0;
    std::fprintf(stderr, "[lane-dl] per picture: hits %.1f, rehash-same %.1f, rehash-changed %.1f, bypassed %.1f, builds %.1f, uncacheable %.1f, promoted %.1f, of them unmerged %.1f, refreshed %.1f\n",
                 (dl[0] - Win.dl[0]) / p, (dl[1] - Win.dl[1]) / p, (dl[2] - Win.dl[2]) / p, (dl[3] - Win.dl[3]) / p, (dl[4] - Win.dl[4]) / p, (dl[5] - Win.dl[5]) / p,
                 (dl[6] - Win.dl[6]) / p, (dl[7] - Win.dl[7]) / p, (dl[8] - Win.dl[8]) / p);
    for (int i = 0; i < 9; ++i) Win.dl[i] = dl[i];
    char mline[320];
    size_t mat = 0;
    for (unsigned i = 0; i < MergeWhyCount; ++i) {
        const int w = std::snprintf(mline + mat, sizeof mline - mat, " %s=%.1f", MergeNames[i], MergeCount[i] / p);
        if (w > 0 && mat + static_cast<size_t>(w) < sizeof mline) mat += static_cast<size_t>(w);
        MergeCount[i] = 0;
    }
    std::fprintf(stderr, "[lane-merge] dynamic draws per picture:%s\n", mline);
}

extern "C" __attribute__((weak)) void vgx_vita_stream_line(unsigned pictures);   // vita/vgx_host_vita.cpp (cache25)

inline void Report(uint32_t now, const char* tag = nullptr) {
    WindowState& w = Win;
    const uint32_t pics = w.pictures;
    Line l;
    l.pictures = pics;
    l.seconds = static_cast<double>(now - w.timedStart) / 1e6;
    l.drawCalls = w.drawCalls / pics;
    l.softVerts = w.softVerts / pics;
    l.staticDraws = w.staticDraws / pics;
    const uint32_t bs = BeSamples.exchange(0, std::memory_order_acquire);
    const uint64_t bc = BeCpuUs.exchange(0, std::memory_order_relaxed), bw = BeWallUs.exchange(0, std::memory_order_relaxed);
    if (bs) { l.beCpu = static_cast<double>(bc) / 1e3 / bs; l.beWall = static_cast<double>(bw) / 1e3 / bs; }
    const uint32_t gs = GpuSamples.exchange(0, std::memory_order_relaxed);
    const uint64_t gu = GpuUs.exchange(0, std::memory_order_relaxed);
    if (gs) l.gpu = static_cast<double>(gu) / 1e3 / gs;
    const uint32_t sp = SendPresents.exchange(0, std::memory_order_relaxed);
    const uint64_t su = SendUs.exchange(0, std::memory_order_relaxed);
    if (sp) l.send = static_cast<double>(su) / 1e3 / sp;
    const bool seen = BeSeen.load(std::memory_order_acquire);
    const bool same = seen && pthread_equal(BeThread, pthread_self());
    l.threads = !seen ? 0 : (same ? 2 : 3);
    const double cpu = ThreadCpuMs();
    if (cpu >= 0 && w.cpu0 >= 0 && cpu >= w.cpu0) l.fe = FeOnly((cpu - w.cpu0) / pics, l.beCpu, same);
    w.cpu0 = cpu;
    if (&NpFpsPlatformProbe) {
        CoreProbe p;
        if (NpFpsPlatformProbe(&p)) {
            if (w.haveCores && l.seconds > 0) {
                for (int i = 0; i < 4; ++i) l.core[i] = (p.cpuMs[i] - w.coreCpu[i]) / (l.seconds * 10.0);
                const int g = p.gameCore >= 0 && p.gameCore < 4 ? p.gameCore : 0;
                l.game = ((p.cpuMs[g] - w.coreCpu[g]) - (p.timeMs[g] - w.coreTime[g])) / pics;
            }
            for (int i = 0; i < 4; ++i) { w.coreCpu[i] = p.cpuMs[i]; w.coreTime[i] = p.timeMs[i]; }
            w.haveCores = true;
        }
    }
    char out[256];
    size_t ol = FormatLine(l, out, sizeof out);
    if (tag && ol + 1 < sizeof out) std::snprintf(out + ol, sizeof out - ol, "%s", tag);   // (MSC_AB: " ab KEY=v a w3")
    std::fprintf(stderr, "%s\n", out);
    if (&vgx_vita_stream_line) vgx_vita_stream_line(pics);   // cache25: [streamc] (MSC_VITA_STREAM_CACHED only; the Vita host defines it)
    if (FeSplitOn()) ReportSplit(pics);
    w.pictures = 0;
    w.drawCalls = w.softVerts = w.staticDraws = 0;
    w.timedStart = now;
}

inline void Picture(uint32_t drawCalls, uint32_t softVerts, uint32_t staticDraws) {
    LateFePicture();
    if (!Enabled()) return;
    WindowState& w = Win;
    if (!w.started) { // (the first picture only starts the window: what came before it is the boot)
        w.started = true;
        w.timedStart = NowUs();
        w.cpu0 = ThreadCpuMs();
        return;
    }
    if (ab::Active()) {   // MSC_AB: a flip ends the window (the picture that finds the boundary belongs to the next one), and the line carries the closed window's tag
        const ab::Cut cut = ab::g_drv.OnPicture(NowUs());
        if (cut.boundary && w.pictures > 0) {
            char tag[96];
            ab::FormatTag(tag, sizeof tag, ab::g_spec, cut.closed);
            Report(NowUs(), tag);
        } else if (cut.boundary) w.timedStart = NowUs();
    }
    w.drawCalls += drawCalls;
    w.softVerts += softVerts;
    w.staticDraws += staticDraws;
    if (++w.pictures >= kWindow) {
        char tag[96];
        if (ab::Active()) ab::FormatTag(tag, sizeof tag, ab::g_spec, ab::g_drv.Current()); else tag[0] = 0;
        Report(NowUs(), tag[0] ? tag : nullptr);
    }
}
// Starts the next picture's timing: called once a picture has been handed on.
inline void NextPicture() {
    if (!FeSplitOn()) return;
    FeSampling = Win.feRound++ % kFeStride == 0;
    if (FeSampling) ++FeSampled;
}

#else // !VGX_WORKLOAD_BUILD

inline constexpr bool Enabled() { return false; }
inline constexpr bool FeSplitOn() { return false; }
struct FeScope { explicit FeScope(unsigned) {} };
struct BackendScope { BackendScope() {} };
struct LateScope { LateScope() {} };
inline bool LateCostTake(double*, unsigned long*) { return false; }
inline void MergeNote(unsigned) {}
inline void ProducerTick() {}
inline void Picture(uint32_t, uint32_t, uint32_t) {}
inline void NextPicture() {}

#endif

} // namespace vgx::workload
