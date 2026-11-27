// np_latency.cpp (W5 RT): pad read -> refresh that shows the frame. See include/np/latency.h.
#include "np/latency.h"
#include "np/touch_diag.h"
#include "np/host_alloc.h"
#pragma weak NpDiagSnapshot

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

extern "C" uint64_t np_time_ns(void);
extern "C" int NpVirtualTimeActive(void) __attribute__((weak));   // harness: the virtual clock
extern "C" int64_t NpVirtualNowNs(void) __attribute__((weak));     // ... read without advancing it (a read on the game thread advances it)
extern "C" void NpFpsLateWaitStats(uint32_t* early, uint32_t* fallback, double* waitedMs) __attribute__((weak));

namespace {
enum Stage { kPad, kHandover, kDecoded, kPresented, kHanded, kShown, kStages };
// STR-102 (1.2): a record belongs to one frame tag (`owner`, written last with release after the stamps are cleared) and every stage checks it, so a
// late stage of an old frame cannot stamp the record of a newer one that reuses the slot; the stamps are written by different threads (game thread,
// the thread that sends the frame, the render thread, the display-queue thread) and handed on through release/acquire pairs (the send tag, the
// renderer's frame -> tag map, the display FIFO), so they are relaxed atomics: nothing is torn, nothing is a data race.
struct Rec {
    std::atomic<uint64_t> owner{0};
    std::atomic<uint64_t> t[kStages];
    std::atomic<uint64_t> entered{0};   // wave 11 (press16): when the display callback was entered for this frame (before the gate's waits): the GPU had finished it by then
};
constexpr unsigned kRing = 128;
Rec g_ring[kRing];
std::atomic<uint64_t> g_sendTag{0};
uint64_t g_seq = 0;            // game thread
std::atomic<uint64_t> g_shownTag{0};   // 1.1: the newest frame (tag) handed to the display, also without MSC_LAT (FramesInFlightGate)
uint64_t g_padNs = 0;          // game thread: first pad use since the last latch
bool g_havePad = false;
int g_on = -1;
unsigned g_logEvery = 300;
std::mutex g_mtx;
np::HostVector<uint32_t> g_win;   // (1.3: host heap: filled on the game thread, kept for the run; np/host_alloc.h) delays of the frames shown since the last NpLatTakeWindow (us)
np::HostVector<uint32_t> g_log[kStages];   // [0]: whole delay; [s]: stage s-1 -> s
uint32_t g_noPad = 0, g_lost = 0;
std::atomic<bool> g_host{false};

// Wave 6 (MSC_PAD_LATE, np_fps.cpp NpFpsPadLateWait): the smallest time a picture sat finished on the GPU waiting for its refresh (handed -> shown) and the
// longest gap between two shown pictures, since the game thread last asked.
std::atomic<uint32_t> g_slackMinUs{0xFFFFFFFFu}, g_slackN{0}, g_gapMaxUs{0};
std::atomic<uint32_t> g_slackLbMinUs{0xFFFFFFFFu}, g_slackLbN{0};   // wave 11 (press16): the same from the callback entry to the screen (a lower bound of the time a finished picture waited)
uint64_t g_prevShownNs = 0;   // (under g_mtx)

bool On() {
    if (g_on < 0) {
        const char* e = std::getenv("MSC_LAT");
        g_on = (e && *e == '0') ? 0 : 1;
        if (const char* l = std::getenv("MSC_LAT_LOG")) g_logEvery = static_cast<unsigned>(std::atoi(l));
    }
    return g_on > 0;
}
uint64_t Now() {
    // (in the harness's virtual-time runs a clock read on the game thread advances the clock: these stamps must not change the run)
    if (&NpVirtualTimeActive && &NpVirtualNowNs && NpVirtualTimeActive()) return static_cast<uint64_t>(NpVirtualNowNs());
    return np_time_ns();
}
Rec* Get(uint64_t tag) {
    if (!tag) return nullptr;
    Rec* r = &g_ring[(tag - 1) % kRing];
    return r->owner.load(std::memory_order_acquire) == tag ? r : nullptr;   // (a recycled slot belongs to a newer frame)
}
inline uint64_t At(const Rec* r, int s) { return r->t[s].load(std::memory_order_relaxed); }
inline void Put(Rec* r, int s, uint64_t v) { r->t[s].store(v, std::memory_order_relaxed); }
void Pct(np::HostVector<uint32_t>& v, uint32_t& p50, uint32_t& p95) {
    if (v.empty()) { p50 = p95 = 0; return; }
    std::sort(v.begin(), v.end());
    p50 = v[v.size() / 2];
    size_t i = (v.size() * 95) / 100;
    if (i >= v.size()) i = v.size() - 1;
    p95 = v[i];
}
void LogLine() {   // (with g_mtx held)
    const size_t n = g_log[0].size();
    if (!n) return;
    static const char* const name[kStages] = {"pad->shown", "pad->handover", "handover->decoded", "decoded->presented", "presented->handed", "handed->shown"};
    std::fprintf(stderr, "[lat] %lu frames%s: ", static_cast<unsigned long>(n), g_host.load() ? " (host build: no display, the end is the decode)" : "");
    for (int s = 0; s < kStages; ++s) {
        if (g_host.load() && s >= kPresented) break;
        uint64_t sum = 0;
        for (uint32_t v : g_log[s]) sum += v;
        uint32_t p50, p95;
        Pct(g_log[s], p50, p95);
        std::fprintf(stderr, "%s%s p50 %.1f p95 %.1f mean %.1f ms", s ? ", " : "", name[s], p50 / 1000.0, p95 / 1000.0,
                     g_log[s].empty() ? 0.0 : static_cast<double>(sum) / g_log[s].size() / 1000.0);
        g_log[s].clear();
    }
    std::fprintf(stderr, "; frames without a pad read %u, records lost %u", g_noPad, g_lost);
    if (&NpFpsLateWaitStats) {
        uint32_t early = 0, fallback = 0;
        double ms = 0;
        NpFpsLateWaitStats(&early, &fallback, &ms);
        std::fprintf(stderr, "; late wait (MSC_LATE_WAIT) made in place %u, at the swap %u, %.1f ms waited in all", early, fallback, ms);
    }
    std::fputc('\n', stderr);
    g_noPad = g_lost = 0;
}
void Complete(Rec* r) {
    // (a copy of the stamps: the record is not read twice with a newer frame's values in between)
    uint64_t t[kStages];
    for (int s = 0; s < kStages; ++s) t[s] = At(r, s);
    const uint64_t entered = r->entered.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!t[kPad]) { ++g_noPad; return; }
    if (t[kHanded] && t[kShown] >= t[kHanded])
    {
        const uint32_t slack = static_cast<uint32_t>(std::min<uint64_t>((t[kShown] - t[kHanded]) / 1000, 0xFFFFFFF0ull));
        for (uint32_t cur = g_slackMinUs.load(std::memory_order_relaxed); slack < cur && !g_slackMinUs.compare_exchange_weak(cur, slack);) {}
        g_slackN.fetch_add(1, std::memory_order_relaxed);
    }
    if (entered && t[kShown] >= entered)
    {
        const uint32_t lb = static_cast<uint32_t>(std::min<uint64_t>((t[kShown] - entered) / 1000, 0xFFFFFFF0ull));
        for (uint32_t cur = g_slackLbMinUs.load(std::memory_order_relaxed); lb < cur && !g_slackLbMinUs.compare_exchange_weak(cur, lb);) {}
        g_slackLbN.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_prevShownNs && t[kShown] > g_prevShownNs)
    {
        const uint32_t gap = static_cast<uint32_t>(std::min<uint64_t>((t[kShown] - g_prevShownNs) / 1000, 0xFFFFFFF0ull));
        for (uint32_t cur = g_gapMaxUs.load(std::memory_order_relaxed); gap > cur && !g_gapMaxUs.compare_exchange_weak(cur, gap);) {}
    }
    g_prevShownNs = t[kShown];
    const uint64_t total = t[kShown] - t[kPad];
    if (total > 2000000000ull) { ++g_lost; return; }   // (a stall of seconds is not a delay)
    g_win.push_back(static_cast<uint32_t>(total / 1000));
    g_log[0].push_back(static_cast<uint32_t>(total / 1000));
    for (int s = kHandover; s < kStages; ++s)
        if (t[s] && t[s - 1] && t[s] >= t[s - 1]) g_log[s].push_back(static_cast<uint32_t>((t[s] - t[s - 1]) / 1000));
    if (g_logEvery && g_log[0].size() >= g_logEvery) LogLine();
}
} // namespace

extern "C" {

void NpLatPadUse(uint64_t rawReadNs, int chan) {
    if (NpDiagEnabled()) return;
    if (chan != 0 || !On()) return;
    if (!g_havePad) {
        g_havePad = true;
        g_padNs = rawReadNs;
    }
}

void NpLatLatch(void) {
    if (NpDiagEnabled()) { if(NpDiagSnapshot)NpDiagSnapshot(); NpDiagLatch(); return; }
    if (!On()) {
        // (no record, but every frame still carries its number to the display: the frames-in-flight limit counts with it)
        g_sendTag.store(++g_seq, std::memory_order_release);
        g_havePad = false;
        return;
    }
    const uint64_t tag = ++g_seq;
    Rec* r = &g_ring[(tag - 1) % kRing];
    r->owner.store(0, std::memory_order_relaxed);   // (the slot's old frame is over: its late stages find no owner)
    for (int s = 0; s < kStages; ++s) Put(r, s, 0);
    r->entered.store(0, std::memory_order_relaxed);
    Put(r, kPad, g_havePad ? g_padNs : 0);
    Put(r, kHandover, Now());
    r->owner.store(tag, std::memory_order_release);   // (published before the send tag that names it)
    g_havePad = false;
    if (g_sendTag.exchange(tag, std::memory_order_acq_rel) != 0) {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_lost;   // the previous frame's send never took its tag
    }
}

uint64_t NpLatTakeSendTag(void) {
    if (NpDiagEnabled()) return NpDiagTake();
    return g_sendTag.exchange(0, std::memory_order_acq_rel);
}

void NpLatDecoded(uint64_t tag) {
    if (NpDiagEnabled()) { NpDiagEvent(NPD_DECODE,0,tag,0,0,0,0,0); return; }
    if (!On()) return;
    if (Rec* r = Get(tag)) Put(r, kDecoded, Now());
}

void NpLatPresented(uint64_t tag) {
    if (NpDiagEnabled()) return;
    if (!On()) return;
    if (Rec* r = Get(tag)) Put(r, kPresented, Now());
}

// wave 11 (press16): the display callback has been entered for this frame (the first thing GateDisplay does, before it waits for the picture's turn)
void NpLatEntered(uint64_t tag) {
    if (NpDiagEnabled() || !On()) return;
    if (Rec* r = Get(tag)) r->entered.store(Now(), std::memory_order_relaxed);
}

void NpLatShown(uint64_t tag, uint64_t shownNs, int handedToo) {
    if (NpDiagEnabled()) return;
    for (uint64_t cur = g_shownTag.load(std::memory_order_relaxed); tag > cur && !g_shownTag.compare_exchange_weak(cur, tag);) {}
    if (!On()) return;
    Rec* r = Get(tag);
    if (!r) return;
    const uint64_t now = Now();
    if (handedToo) Put(r, kHanded, now);
    Put(r, kShown, shownNs ? shownNs : now);
#if !defined(__vita__)
    g_host.store(true);
    Put(r, kShown, At(r, kDecoded) ? At(r, kDecoded) : now);
#endif
    Complete(r);
}

// Wave 6: the pictures shown since the last call: how many, the smallest handed -> shown slack and the longest gap between two of them (microseconds).
int NpLatSlackTake(uint32_t* minSlackUs, uint32_t* maxGapUs) {
    const uint32_t n = g_slackN.exchange(0, std::memory_order_acq_rel);
    *minSlackUs = g_slackMinUs.exchange(0xFFFFFFFFu, std::memory_order_acq_rel);
    *maxGapUs = g_gapMaxUs.exchange(0, std::memory_order_acq_rel);
    return static_cast<int>(n);
}

// wave 11 (press16, MSC_LAT_SAMPLE): the pictures shown since the last call and the smallest callback entry -> shown time (us): at least as long as the picture sat finished
int NpLatSlackLbTake(uint32_t* minLbUs) {
    const uint32_t n = g_slackLbN.exchange(0, std::memory_order_acq_rel);
    *minLbUs = g_slackLbMinUs.exchange(0xFFFFFFFFu, std::memory_order_acq_rel);
    return static_cast<int>(n);
}

uint64_t NpLatHandedOverTag(void) { return g_seq; }   // (game thread)
uint64_t NpLatShownTag(void) { return g_shownTag.load(std::memory_order_acquire); }

void NpLatTakeWindow(uint32_t* n, uint32_t* p50Us, uint32_t* p95Us) {
    if (NpDiagEnabled()) { *n=*p50Us=*p95Us=0; return; }
    std::lock_guard<std::mutex> lk(g_mtx);
    *n = static_cast<uint32_t>(g_win.size());
    Pct(g_win, *p50Us, *p95Us);
    g_win.clear();
}

} // extern "C"
