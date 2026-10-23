// np_latency.cpp (W5 RT): pad read -> refresh that shows the frame. See include/np/latency.h.
#include "np/latency.h"
#include "np/touch_diag.h"
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
struct Rec {
    uint64_t t[kStages];
};
constexpr unsigned kRing = 128;
Rec g_ring[kRing];
std::atomic<uint64_t> g_sendTag{0};
uint64_t g_seq = 0;            // game thread
uint64_t g_padNs = 0;          // game thread: first pad use since the last latch
bool g_havePad = false;
int g_on = -1;
unsigned g_logEvery = 300;
std::mutex g_mtx;
std::vector<uint32_t> g_win;   // delays of the frames shown since the last NpLatTakeWindow (us)
std::vector<uint32_t> g_log[kStages];   // [0]: whole delay; [s]: stage s-1 -> s
uint32_t g_noPad = 0, g_lost = 0;
bool g_host = false;

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
Rec* Get(uint64_t tag) { return tag ? &g_ring[(tag - 1) % kRing] : nullptr; }
void Pct(std::vector<uint32_t>& v, uint32_t& p50, uint32_t& p95) {
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
    std::fprintf(stderr, "[lat] %lu frames%s: ", static_cast<unsigned long>(n), g_host ? " (host build: no display, the end is the decode)" : "");
    for (int s = 0; s < kStages; ++s) {
        if (g_host && s >= kPresented) break;
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
    if (!r->t[kPad]) { ++g_noPad; return; }
    std::lock_guard<std::mutex> lk(g_mtx);
    const uint64_t total = r->t[kShown] - r->t[kPad];
    if (total > 2000000000ull) { ++g_lost; return; }   // (a stall of seconds is not a delay)
    g_win.push_back(static_cast<uint32_t>(total / 1000));
    g_log[0].push_back(static_cast<uint32_t>(total / 1000));
    for (int s = kHandover; s < kStages; ++s)
        if (r->t[s] && r->t[s - 1] && r->t[s] >= r->t[s - 1]) g_log[s].push_back(static_cast<uint32_t>((r->t[s] - r->t[s - 1]) / 1000));
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
    if (!On()) return;
    const uint64_t tag = ++g_seq;
    Rec* r = Get(tag);
    for (uint64_t& t : r->t) t = 0;
    r->t[kPad] = g_havePad ? g_padNs : 0;
    r->t[kHandover] = Now();
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
    if (Rec* r = Get(tag)) r->t[kDecoded] = Now();
}

void NpLatPresented(uint64_t tag) {
    if (NpDiagEnabled()) return;
    if (Rec* r = Get(tag)) r->t[kPresented] = Now();
}

void NpLatShown(uint64_t tag, uint64_t shownNs, int handedToo) {
    if (NpDiagEnabled()) return;
    Rec* r = Get(tag);
    if (!r) return;
    const uint64_t now = Now();
    if (handedToo) r->t[kHanded] = now;
    r->t[kShown] = shownNs ? shownNs : now;
#if !defined(__vita__)
    g_host = true;
    r->t[kShown] = r->t[kDecoded] ? r->t[kDecoded] : now;
#endif
    Complete(r);
}

void NpLatTakeWindow(uint32_t* n, uint32_t* p50Us, uint32_t* p95Us) {
    if (NpDiagEnabled()) { *n=*p50Us=*p95Us=0; return; }
    std::lock_guard<std::mutex> lk(g_mtx);
    *n = static_cast<uint32_t>(g_win.size());
    Pct(g_win, *p50Us, *p95Us);
    g_win.clear();
}

} // extern "C"
