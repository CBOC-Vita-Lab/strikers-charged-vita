// Frame presenter: see vgx_present.h.
#include "vgx_present.h"
#include "vgx_interp.h"
#include "vgx_present_sched.h"
#include "vgx_common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

namespace vgx::rec { void PlayerWaitForData(); }
namespace vgx { void SetFrameSink(void (*sink)(CmdBuf& buf)); }

namespace vgx::present {

namespace {

struct Params {
    int hz = -1; // -1: not read yet
    bool wall = true;
    int log = 0;
    double drawMs = 0;
    double throttleMs = 0, throttleJitter = 0;
    const char* csv = nullptr;
    double vInterval = 1000.0 / 15.0; // virtual clock: the game time between two built frames
    SchedCfg cfg;
} g_p;

void InitOnce();
void Init() {
    // (called from the game thread and the render thread at start-up)
    static std::once_flag once;
    std::call_once(once, InitOnce);
}
void InitOnce() {
    g_p.hz = 0;
    const long hz = EnvInt("MSC_VGX_PRESENT", 0);
    if (hz <= 0) return;
    g_p.hz = hz == 1 ? 30 : static_cast<int>(hz);
    g_p.cfg.tickMs = 1000.0 / g_p.hz;
    if (const char* c = std::getenv("MSC_VGX_PRESENT_CLOCK")) g_p.wall = std::strcmp(c, "virtual") != 0;
    auto envf = [](const char* n, double d) { const char* v = std::getenv(n); return (v && *v) ? std::atof(v) : d; };
    g_p.cfg.marginMs = envf("MSC_VGX_PRESENT_MARGIN", 20.0);
    g_p.cfg.gain = envf("MSC_VGX_PRESENT_GAIN", 0.10);
    g_p.cfg.controller = g_p.cfg.gain > 0;
    g_p.cfg.bypassMs = envf("MSC_VGX_PRESENT_BYPASS", 0.0);
    g_p.cfg.extrapMs = envf("MSC_VGX_PRESENT_EXTRAP", 0.0);
    g_p.log = static_cast<int>(EnvInt("MSC_VGX_PRESENT_LOG", 0));
    g_p.csv = std::getenv("MSC_VGX_PRESENT_CSV");
    g_p.drawMs = envf("MSC_VGX_PRESENT_DRAWMS", 0.0);
    g_p.vInterval = envf("MSC_VGX_PRESENT_VINTERVAL", 1000.0 / 15.0);
    if (const char* t = std::getenv("MSC_VGX_THROTTLE")) {
        double a = 0, b = 0;
        if (std::sscanf(t, "%lf:%lf", &a, &b) >= 1) { g_p.throttleMs = a; g_p.throttleJitter = b; }
    }
    interp::SetMode(1); // (the presenter uses the interpolation module's machinery; its fixed in-between pictures are not used)
}

// ---- game time of a frame. The game thread is the bottleneck: it begins frame N the moment frame N-1 has been handed over (rec::Present
// returns when the player has decoded the frame: NotifyDecoded, a few records before Sink() runs on the render thread). So the game time
// of a frame is the time Sink() saw the frame before it: no clock is read on the game thread (a harness with a virtual clock advances
// that clock per read on the game thread, and the runs would no longer repeat), and nothing in the runtime has to change. A game
// thread that sleeps between frames (the retrace pacing, when it is faster than the display) makes this early by the sleep; that is the
// regime where every frame is shown as itself anyway.
double g_lastArrival = -1, g_prevG = -1;
RepeatHook g_repeat = nullptr;
bool g_noteCapture = false, g_noteSkip = false; // the flags of the frame being decoded (render thread)

// ---- render thread
struct Slot {
    CmdBuf buf;
    uint64_t id = 0;
    bool inUse = false, capture = false, skip = false;
    uint32_t pics = 0;
    double g = 0;
};
Slot g_slots[3];
uint64_t g_nextId = 1;
Slot* Find(uint64_t id) {
    for (Slot& s : g_slots) if (s.inUse && s.id == id) return &s;
    return nullptr;
}
void Flags(const Slot& s) {
    backend::RequestCapture(s.capture);
    backend::SetSkipDraws(s.skip);
}
void BusyMs(double ms) {
    if (ms <= 0) return;
    const double t0 = NowMs();
    while (NowMs() - t0 < ms) {}
}

class RenderBackend : public Backend {
public:
    bool Prepare(uint64_t id, bool wantPairs) override {
        Slot* s = Find(id);
        if (!s) return false;
        s->pics = 0;
        return interp::Begin(s->buf, wantPairs);
    }
    void Blend(uint64_t id, float t, bool first) override {
        Slot* s = Find(id);
        if (!s) return;
        Flags(*s);
        interp::Blend(s->buf, t, first, false);
        BusyMs(g_p.drawMs);
        NotePicture(*s, t);
    }
    void Self(uint64_t id, bool first, bool hidden) override {
        Slot* s = Find(id);
        if (!s) return;
        Flags(*s);
        interp::Self(s->buf, first, hidden);
        if (!hidden) { BusyMs(g_p.drawMs); NotePicture(*s, 1.0f); }
    }
    void Finish(uint64_t id) override {
        Slot* s = Find(id);
        if (!s) return;
        interp::End(s->buf);
        s->inUse = false;
    }
    void Repeat() override {
        BusyMs(g_p.drawMs * 0.05);
        if (g_repeat) g_repeat();
    }
    static void NotePicture(Slot& s, float t);
};
RenderBackend g_be;
Scheduler* g_sched = nullptr;

// What the host's picture hook (interp.h's PictureHook) is told: frame, picture index within the frame, drawn, t.
void RenderBackend::NotePicture(Slot& s, float t) {
    if (const interp::PictureHook h = interp::GetPictureHook()) h(s.buf.frame, static_cast<int>(s.pics), true, t);
    ++s.pics;
}

double g_nextTick = 0;
bool g_started = false;
VblankCountFn g_vblank = nullptr;
uint32_t g_vcNext = 0;
bool g_vcInit = false;
FILE* g_csv = nullptr;
uint64_t g_tickNo = 0, g_slips = 0;

void LogTick(const TickInfo& ti, double now) {
    ++g_tickNo;
    if (g_p.csv && !g_csv) {
        g_csv = std::fopen(g_p.csv, "w");
        if (g_csv) std::fprintf(g_csv, "tick,now,c,delay,kind,frame,t,rate,margin,queued\n");
    }
    if (g_csv) {
        std::fprintf(g_csv, "%llu,%.3f,%.3f,%.3f,%d,%llu,%.3f,%.3f,%.1f,%d\n", static_cast<unsigned long long>(g_tickNo), now, ti.c, now - ti.c, static_cast<int>(ti.kind),
                     static_cast<unsigned long long>(ti.frame), ti.t, ti.rate, ti.margin, ti.queued);
        if (g_tickNo % 64 == 0) std::fflush(g_csv);
    }
    static const char* const kn[] = {"-", "repeat", "BLEND", "SELF", "hold", "STALL"};
    if (g_p.log >= 2)
        std::fprintf(stderr, "[present] tick %llu now %.1f c %.1f delay %.1f %s frame %llu t %.2f rate %.3f margin %.1f queued %d\n", static_cast<unsigned long long>(g_tickNo), now, ti.c, now - ti.c,
                     kn[static_cast<int>(ti.kind) % 6], static_cast<unsigned long long>(ti.frame), ti.t, ti.rate, ti.margin, ti.queued);
    if (g_p.log >= 1 && g_sched && g_tickNo % 300 == 0) {
        const SchedStats& st = g_sched->stats();
        std::fprintf(stderr,
                     "[present] %llu ticks, %llu frames: %llu blends, %llu self, %llu repeats (%llu stalls, %llu holds for cuts, %llu empty), %llu hidden, %llu bypass ticks, %llu slips; delay mean %.1f max %.1f ms, "
                     "interval %.1f ms, margin %.1f, rate %.3f%s\n",
                     static_cast<unsigned long long>(st.ticks), static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.blends), static_cast<unsigned long long>(st.selfs),
                     static_cast<unsigned long long>(st.repeats), static_cast<unsigned long long>(st.stalls), static_cast<unsigned long long>(st.holds), static_cast<unsigned long long>(st.empty),
                     static_cast<unsigned long long>(st.hidden), static_cast<unsigned long long>(st.bypassTicks), static_cast<unsigned long long>(g_slips),
                     st.delayN ? st.sumDelay / st.delayN : 0.0, st.maxDelay, g_sched->interval(), g_sched->margin(), g_sched->rate(), g_sched->bypass() ? " BYPASS" : "");
    }
}

void EnsureSched() {
    if (!g_sched) g_sched = new Scheduler(g_p.cfg, g_be);
}

} // namespace

bool Enabled() {
    Init();
    return g_p.hz > 0;
}
bool Wall() {
    Init();
    return g_p.hz > 0 && g_p.wall;
}
double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void SetRepeatHook(RepeatHook hook) { g_repeat = hook; }
double DelayMs() { return g_sched && !g_sched->bypass() ? g_sched->delayMs() : 0.0; }
void SetVblankSource(VblankCountFn fn) { g_vblank = fn; }
void NoteFlags(bool capture, bool skipDraws) {
    g_noteCapture = capture;
    g_noteSkip = skipDraws;
}

void Throttle() {
    static const bool off = [] {
        const char* t = std::getenv("MSC_VGX_THROTTLE");
        return !(t && *t);
    }();
    if (off) return;
    Init();
    if (g_p.hz == 0 && g_p.throttleMs == 0 && g_p.throttleJitter == 0) { // (Init stops early without MSC_VGX_PRESENT: read the knob here)
        double a = 0, b = 0;
        if (std::sscanf(std::getenv("MSC_VGX_THROTTLE"), "%lf:%lf", &a, &b) >= 1) { g_p.throttleMs = a; g_p.throttleJitter = b; }
    }
    static const long from = EnvInt("MSC_VGX_THROTTLE_FROM", 0);
    static long n = 0;
    if (++n < from) return;
    if (g_p.throttleMs <= 0 && g_p.throttleJitter <= 0) return;
    static std::mt19937 rng(12345);
    const double ms = g_p.throttleMs + (g_p.throttleJitter > 0 ? std::uniform_real_distribution<double>(0, g_p.throttleJitter)(rng) : 0.0);
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(ms * 1000)));
}

void Sink(CmdBuf& buf) {
    Init();
    EnsureSched();
    if (buf.cmds.empty() || buf.cmds.back().type != CMD_PRESENT) { // not a frame (a tool's resource preamble)
        backend::Execute(buf);
        return;
    }
    // a free slot (the wall clock presenter may hold two frames: the one being shown and the next)
    Slot* s = nullptr;
    for (;;) {
        for (Slot& x : g_slots) if (!x.inUse) { s = &x; break; }
        if (s) break;
        Service(); // (virtual mode never gets here: every frame is finished in its own Sink)
        Idle();
    }
    s->inUse = true;
    s->id = g_nextId++;
    s->capture = g_noteCapture;
    s->skip = g_noteSkip;
    std::swap(s->buf, buf);
    // Virtual clock (a harness with a virtual time: its clock advances with what the game thread does, and the render thread would read it at
    // a moment that depends on how the two threads race): frame n happened at n * MSC_VGX_PRESENT_VINTERVAL, the interval a harness run at 15 built
    // frames per second has (66.67 ms): the pictures are then the ones the old fixed in-between had.
    const double now = g_p.wall ? NowMs() : static_cast<double>(s->id) * g_p.vInterval;
    const double g = !g_p.wall ? static_cast<double>(s->id - 1) * g_p.vInterval : (g_lastArrival >= 0 ? g_lastArrival : now - 2.0 * g_p.cfg.tickMs); // (the first frame: any anchor)
    g_lastArrival = now;
    s->g = g;
    if (g_p.log >= 2) std::fprintf(stderr, "[present] frame %llu arrives at %.1f game time %.1f (interval %.1f)\n", static_cast<unsigned long long>(s->id), now, g, g - (g_prevG >= 0 ? g_prevG : g));
    g_prevG = g;
    g_sched->Arrive(s->id, g, now);
    if (g_p.wall) {
        if (!g_started) { g_started = true; g_nextTick = now; }
        // the pictures are made by Service(), which the player loop calls between decoding slices
        Service();
    } else {
        g_sched->RunVirtual(now, [&](const TickInfo& ti) { LogTick(ti, now); });
    }
}

void Service() {
    if (!g_p.wall || !g_started || !g_sched) return;
    double now = NowMs();
    int guard = 0;
    if (g_vblank) {
        // ticks at vblanks: every vper vblanks of the counter; the content clock still advances by the nominal tick
        const uint32_t vper = static_cast<uint32_t>(60 / g_p.hz > 0 ? 60 / g_p.hz : 1);
        const uint32_t vc = g_vblank();
        if (!g_vcInit) { g_vcInit = true; g_vcNext = vc; }
        while (static_cast<int32_t>(vc - g_vcNext) >= 0) {
            const TickInfo ti = g_sched->Tick(now);
            LogTick(ti, now);
            g_vcNext += vper;
            if (static_cast<int32_t>(vc - g_vcNext) > static_cast<int32_t>(3 * vper)) { ++g_slips; g_vcNext = vc + vper; break; } // far behind: no burst
            if (++guard > 4) break;
            now = NowMs();
        }
        return;
    }
    while (now >= g_nextTick) {
        const TickInfo ti = g_sched->Tick(g_nextTick);
        LogTick(ti, g_nextTick);
        g_nextTick += g_p.cfg.tickMs;
        now = NowMs();
        if (now - g_nextTick > 3.0 * g_p.cfg.tickMs) { // the thread was away for long: do not make up for it with a burst
            ++g_slips;
            g_nextTick = now + g_p.cfg.tickMs;
            break;
        }
        if (++guard > 4) break;
    }
}

void Idle() {
    rec::PlayerWaitForData(); // (at most a millisecond)
}

} // namespace vgx::present
