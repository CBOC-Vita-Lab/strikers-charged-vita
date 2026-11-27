// STR-102: the latency record (platform/compat/np_latency.cpp) with its stages stamped by four threads, the way the console does it: the game thread
// latches a frame at the hand-over, the send thread takes the tag and stamps "decoded", the render thread stamps "presented" and the display-queue
// thread stamps "shown" (through a FIFO with release/acquire, like the Vita host's). Every frame must complete exactly once with coherent stamps, and a
// stage that arrives late for a recycled slot must not touch the newer frame. Also the (count, time) pair of np_display_last_vblank's sequence lock.
// From the repository root (add -fsanitize=thread for the race check; -DLEGACY_MAP reproduces the 1.1 map and publication order):
//   clang++ -std=c++17 -O1 -DNP_NATIVE=1 -Inative/include native/tools/fps/lat_race_unit.cpp native/platform/compat/np_latency.cpp -o /tmp/lat_race_unit -pthread
#include "np/latency.h"
#include "np/touch_diag.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static std::atomic<uint64_t> g_clock{1000};
extern "C" uint64_t np_time_ns(void) { return g_clock.fetch_add(10, std::memory_order_relaxed); }
extern "C" int NpDiagEnabled(void) { return 0; }
extern "C" int NpDiagActive(void) { return 0; }
extern "C" uint64_t NpDiagLatch(void) { return 0; }
extern "C" uint64_t NpDiagTake(void) { return 0; }
extern "C" void NpDiagSnapshot(void) {}
extern "C" void NpDiagEvent(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}

extern "C" uint64_t NpLatHandedOverTag(void);
extern "C" uint64_t NpLatShownTag(void);

namespace {
std::atomic<uint64_t> g_taken{0};
struct Fifo {   // the Vita host's SPSC tag FIFOs
    uint64_t tag[64] = {};
    std::atomic<uint32_t> head{0}, tail{0};
    bool Push(uint64_t t) {
        const uint32_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= 64) return false;
        tag[h % 64] = t;
        head.store(h + 1, std::memory_order_release);
        return true;
    }
    bool Pop(uint64_t* t) {
        const uint32_t tl = tail.load(std::memory_order_relaxed);
        if (tl == head.load(std::memory_order_acquire)) return false;
        *t = tag[tl % 64];
        tail.store(tl + 1, std::memory_order_release);
        return true;
    }
} sendToRender, renderToDisplay;
// The renderer's frame -> tag map: one word per entry, release/acquire (renderer/vita/vgx_host_vita.cpp LatMapPut / LatTagOf)
std::atomic<uint64_t> g_map[256];
void Put(uint64_t frame, uint64_t tag) { g_map[frame % 256].store(((frame & 0xFFFFFFull) << 40) | (tag & 0xFFFFFFFFFFull), std::memory_order_release); }
uint64_t TagOf(uint64_t frame) {
    const uint64_t v = g_map[frame % 256].load(std::memory_order_acquire);
    return v != 0 && (v >> 40) == (frame & 0xFFFFFFull) ? (v & 0xFFFFFFFFFFull) : 0;
}
std::atomic<uint64_t> g_sendFrame{0};
// the 1.1 map (-DLEGACY_MAP): two plain fields, published before the decode stamp: this is what the thread sanitizer reports on the old code
struct OldMap { uint64_t frame = 0, tag = 0; } g_old[256];
}

// The (count, time) pair of np_display_last_vblank (platform/sdk/vita/np_plat_vita.cpp): the writer publishes count k with time k * 1000; a reader must
// never see a pair that is not (k, k * 1000). Old: two plain stores (time, then count) and "re-read when the count moved"; new: a sequence lock.
// (The code below is a copy of both versions: keep in step.)
namespace vb {
volatile uint32_t oldCount = 0; volatile uint64_t oldNs = 0;
std::atomic<uint32_t> seq{0}, count{0}; std::atomic<uint64_t> ns{0};
void WriteOld(uint32_t k) { oldNs = (uint64_t)k * 1000; oldCount = k; }
int ReadOld(uint32_t* v, uint64_t* t) {
    for (int i = 0; i < 3; ++i) { const uint32_t c = oldCount; const uint64_t n = oldNs; if (c == oldCount && n) { *v = c; *t = n; return 1; } }
    return 0;
}
void WriteNew(uint32_t k) {
    const uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    ns.store((uint64_t)k * 1000, std::memory_order_relaxed);
    count.store(k, std::memory_order_relaxed);
    seq.store(s + 2, std::memory_order_release);
}
int ReadNew(uint32_t* v, uint64_t* t) {
    for (int i = 0; i < 8; ++i) {
        const uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        const uint32_t c = count.load(std::memory_order_relaxed);
        const uint64_t n = ns.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s1 == seq.load(std::memory_order_relaxed) && n) { *v = c; *t = n; return 1; }
    }
    return 0;
}
long Stress(bool fresh) {
    std::atomic<bool> stop{false};
    std::thread w([&] { for (uint32_t k = 1; !stop.load(std::memory_order_relaxed); ++k) { if (fresh) WriteNew(k); else WriteOld(k); } });
    long bad = 0, got = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
        uint32_t v; uint64_t t;
        if (fresh ? ReadNew(&v, &t) : ReadOld(&v, &t)) { ++got; if (t != (uint64_t)v * 1000) ++bad; }
    }
    stop = true;
    w.join();
    std::printf("vblank pair, %s: %ld reads, %ld incoherent\n", fresh ? "sequence lock" : "1.1 volatile pair", got, bad);
    return bad;
}
}

int main() {
    setenv("MSC_LAT_LOG", "0", 1);
    const long oldBad = vb::Stress(false);
    const long newBad = vb::Stress(true);
    (void)oldBad;
    const int frames = 60000;
    std::atomic<int> shown{0};
    std::atomic<bool> bad{false};
    std::thread render([&] {
        uint64_t seen = 0;
        while (seen < (uint64_t)frames) {
            const uint64_t f = g_sendFrame.load(std::memory_order_relaxed);   // (relaxed: the map entry is what orders the stamps)
            if (f <= seen) { std::this_thread::yield(); continue; }
            for (uint64_t k = seen + 1; k <= f; ++k) {
#ifdef LEGACY_MAP
                const uint64_t tag = g_old[k % 256].frame == k ? g_old[k % 256].tag : 0;
#else
                const uint64_t tag = TagOf(k);
#endif
                if (!tag) { bad = true; continue; }   // (the frame counter only says the entry is being published: the entry must be complete once the counter is seen)
                NpLatPresented(tag);
                while (!renderToDisplay.Push(tag)) std::this_thread::yield();
            }
            seen = f;
        }
    });
    std::thread display([&] {
        while (shown.load() < frames) {
            uint64_t tag;
            if (!renderToDisplay.Pop(&tag)) { std::this_thread::yield(); continue; }
            NpLatShown(tag, 0, 1);
            shown.fetch_add(1);
        }
    });
    // game thread + send thread in one loop here (the game thread latches, the send thread takes the tag): two threads would need the real frame send
    std::thread send([&] {
        uint64_t n = 0;
        while (n < (uint64_t)frames) {
            const uint64_t tag = NpLatTakeSendTag();
            if (!tag) { std::this_thread::yield(); continue; }
            ++n;
            g_taken.store(n, std::memory_order_release);   // (the game thread may latch the next frame now)
#ifdef LEGACY_MAP
            g_old[n % 256].tag = tag; g_old[n % 256].frame = n;
            NpLatDecoded(tag);
#else
            NpLatDecoded(tag);
            Put(n, tag);
#endif
            g_sendFrame.store(n, std::memory_order_relaxed);
            // at most 40 frames ahead of the display
            while (n > (uint64_t)shown.load() + 40) std::this_thread::yield();
        }
    });
    for (int i = 0; i < frames; ++i) {
        while (g_taken.load(std::memory_order_acquire) < (uint64_t)i) std::this_thread::yield();   // (the real game thread waits for the send)
        NpLatPadUse(g_clock.load(), 0);
        NpLatLatch();
    }
    send.join();
    render.join();
    display.join();
    uint32_t n, p50, p95;
    NpLatTakeWindow(&n, &p50, &p95);
    std::printf("frames shown %d, in the window %u, p50 %u us, p95 %u us, shown tag %llu\n", shown.load(), n, p50, p95, (unsigned long long)NpLatShownTag());
    int fail = 0;
    if (bad) { std::printf("FAIL: a frame had no tag in the map\n"); ++fail; }
    if (NpLatShownTag() == 0) { std::printf("FAIL: no shown tag\n"); ++fail; }
    if (newBad) { std::printf("FAIL: the sequence lock returned an incoherent pair\n"); ++fail; }
    // a late stage for a recycled slot is ignored: tag 1's slot is reused by tag 1 + 128
    NpLatLatch();
    const uint64_t cur = NpLatHandedOverTag();
    NpLatDecoded(cur - 128);   // (a stale tag of the same slot)
    NpLatShown(cur - 128, 0, 0);
    std::printf("%s\n", fail ? "FAILED" : "ok");
    return fail ? 1 : 0;
}
