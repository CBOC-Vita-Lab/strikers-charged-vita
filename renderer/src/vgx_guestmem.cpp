// Guest RAM bookkeeping for the caches: which host ranges are guest memory,
// and a write-generation counter per 4 KiB of it.
//
// The console's graphics processor read display lists, vertex arrays and
// textures from physical RAM, so a game has to flush the CPU data cache over
// anything it changed before drawing with it. The runtime's HLE of those
// cache calls (and of disc reads and similar bulk writes) reports the range
// here. A cached decode of guest data stays valid while the counters under it
// have not moved; when they have, the bytes are compared (hashed) again.
#include "vgx_guestmem.h"
#include "vgx_common.h"
#include "vgx_hle.h"

#include <atomic>
#include <memory>

namespace vgx {

namespace {
constexpr int kShift = 12;
struct Bank {
    const uint8_t* base = nullptr;
    uint32_t size = 0;
    std::unique_ptr<std::atomic<uint32_t>[]> gen;
    std::unique_ptr<std::atomic<uint8_t>[]> watch; // MSC_VGX_DLWATCH: 1 = a cache entry depends on this page
};
Bank g_banks[4];
std::atomic<int> g_numBanks{0}; // (1.1: atomic: a bank may be registered while the renderer runs, e.g. the skinning pose pool)
std::atomic<uint64_t> g_epoch{0}; // bumped by every write notification
std::atomic<uint64_t> g_watchEpoch{0}; // bumped by every write notification that touches a watched page
// gem2b MSC_VGX_DLWATCH_BUCKETS: the same, per bucket of 16 pages (64 KiB) of a bank: g_watchBucket[BucketOf(bank, page)] is bumped, once per write notification, for every bucket that holds a
// watched page the notification wrote. An entry keeps the set of buckets its watched pages are in (WatchRangeMask) and compares the sum of those counters (monotonic: the sum moves
// whenever one does), so a write to a page nobody of the entry depends on no longer makes it read its generations again.
constexpr int kBucketShift = 4, kBuckets = 64;
std::atomic<uint64_t> g_watchBucket[kBuckets];
inline uint32_t BucketOf(int bank, uint32_t page) { return ((page >> kBucketShift) + static_cast<uint32_t>(bank) * 21u) & (kBuckets - 1); }
// The last kLogSize write notifications, slot = epoch % kLogSize. A slot's
// stamp is 0 while it is being written and epoch+1 once complete, so a reader
// can tell a slot that was reused or is half written.
constexpr uint64_t kLogSize = 2048;
struct LogRec {
    std::atomic<uint64_t> stamp{0};
    std::atomic<const uint8_t*> p{nullptr};
    std::atomic<uint32_t> n{0};
};
LogRec g_log[kLogSize];

inline const Bank* FindBank(const uint8_t* p, uint32_t n) {
    const int nb = g_numBanks.load(std::memory_order_acquire);
    for (int i = 0; i < nb; ++i) {
        const Bank& b = g_banks[i];
        if (p >= b.base && static_cast<uint64_t>(p - b.base) + n <= b.size) return &b;
    }
    return nullptr;
}
} // namespace

uint64_t GuestWriteGeneration(const void* ptr, uint32_t n) {
    if (n == 0) return kNotGuest;
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    const Bank* b = FindBank(p, n);
    if (!b) return kNotGuest;
    const uint32_t first = static_cast<uint32_t>(p - b->base) >> kShift, last = static_cast<uint32_t>(p - b->base + n - 1) >> kShift;
    uint64_t sum = 0;
    uint32_t i = first;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
    if (Gxd(GXD_LOOPS_NEON) && last - first >= 7u) { // (a mesh array spans tens of pages: four counters per load, summed in 64 bits)
        uint64x2_t acc = vdupq_n_u64(0);
        const uint32_t* g = reinterpret_cast<const uint32_t*>(b->gen.get());
        for (; i + 3u <= last; i += 4) acc = vpadalq_u32(acc, vld1q_u32(g + i)); // (aligned: the array comes from new[] of 4-byte elements; vld1q without hint)
        sum = vgetq_lane_u64(acc, 0) + vgetq_lane_u64(acc, 1);
    }
#endif
    for (; i <= last; ++i) sum += b->gen[i].load(std::memory_order_relaxed);
    return sum; // counters only grow, so the sum changes whenever one of them does
}
bool IsGuestRam(const void* ptr, uint32_t n) { return FindBank(static_cast<const uint8_t*>(ptr), n ? n : 1) != nullptr; }
bool HaveGuestRam() { return g_numBanks.load(std::memory_order_relaxed) != 0; }
uint64_t GuestWriteEpoch() { return g_epoch.load(std::memory_order_acquire); }
uint64_t GuestWatchEpoch() { return g_watchEpoch.load(std::memory_order_acquire); }
uint64_t GuestWatchSum(uint64_t mask) {
    uint64_t sum = 0;
    while (mask) {
        const int b = __builtin_ctzll(mask);
        mask &= mask - 1;
        sum += g_watchBucket[b].load(std::memory_order_acquire);
    }
    return sum;
}
void WatchRange(const void* ptr, uint32_t n) {
    if (n == 0) return;
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    const Bank* b = FindBank(p, n);
    if (!b) return;
    const uint32_t first = static_cast<uint32_t>(p - b->base) >> kShift, last = static_cast<uint32_t>(p - b->base + n - 1) >> kShift;
    for (uint32_t i = first; i <= last; ++i)
        if (b->watch[i].load(std::memory_order_relaxed) == 0) b->watch[i].store(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst); // (the flags are visible to a writer before the epoch and the generations are read)
}

uint64_t WatchRangeMask(const void* ptr, uint32_t n) {
    WatchRange(ptr, n);   // (flags first, fence inside; the mask is only a name for the buckets)
    if (n == 0) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    const Bank* b = FindBank(p, n);
    if (!b) return 0;
    const int bank = static_cast<int>(b - g_banks);
    const uint32_t first = static_cast<uint32_t>(p - b->base) >> kShift, last = static_cast<uint32_t>(p - b->base + n - 1) >> kShift;
    uint64_t mask = 0;
    const uint32_t g0 = first >> kBucketShift, g1 = last >> kBucketShift;
    if (g1 - g0 >= static_cast<uint32_t>(kBuckets)) mask = ~0ull;
    else for (uint32_t g = g0; g <= g1; ++g) mask |= 1ull << BucketOf(bank, g << kBucketShift);
    return mask;
}

bool GuestWritesTouch(uint64_t from, uint64_t to, const HostRange* ranges, int count) {
    if (to <= from) return false;
    if (to - from > kLogSize / 2) return true;
    for (uint64_t seq = from; seq < to; ++seq) {
        LogRec& r = g_log[seq % kLogSize];
        if (r.stamp.load(std::memory_order_acquire) != seq + 1) return true;
        const uint8_t* p = r.p.load(std::memory_order_relaxed);
        const uint32_t n = r.n.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r.stamp.load(std::memory_order_relaxed) != seq + 1) return true;
        for (int i = 0; i < count; ++i) {
            const uint8_t* q = static_cast<const uint8_t*>(ranges[i].p);
            if (!q || !ranges[i].n) continue;
            if (p < q + ranges[i].n && q < p + n) return true;
        }
    }
    return false;
}

namespace hle {
void RegisterGuestRam(const void* hostBase, uint32_t size) {
    const int nb = g_numBanks.load(std::memory_order_relaxed);
    if (!hostBase || !size || nb >= 4) return;
    Bank& b = g_banks[nb];
    b.base = static_cast<const uint8_t*>(hostBase);
    b.size = size;
    const size_t n = (static_cast<size_t>(size) >> kShift) + 1;
    b.gen.reset(new std::atomic<uint32_t>[n]);
    for (size_t i = 0; i < n; ++i) b.gen[i].store(1, std::memory_order_relaxed);
    b.watch.reset(new std::atomic<uint8_t>[n]);
    for (size_t i = 0; i < n; ++i) b.watch[i].store(0, std::memory_order_relaxed);
    g_numBanks.store(nb + 1, std::memory_order_release); // published last (readers acquire it)
}
void NotifyGuestWrite(const void* hostPtr, uint32_t size) {
    if (!size) return;
    const uint8_t* p = static_cast<const uint8_t*>(hostPtr);
    const Bank* b = FindBank(p, size);
    if (!b) return;
    const uint32_t first = static_cast<uint32_t>(p - b->base) >> kShift, last = static_cast<uint32_t>(p - b->base + size - 1) >> kShift;
    for (uint32_t i = first; i <= last; ++i) b->gen[i].fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst); // (generation added, then the flags are looked at: pairs with WatchRange's fence)
    {
        static const bool brk = (EnvInt("MSC_VGX_BREAK", 0) & 2) != 0; // (mutant for the verify runs: the watch epoch no longer moves)
        const int bank = static_cast<int>(b - g_banks);
        uint64_t hit = 0;   // (buckets that hold a watched page this notification wrote)
        bool any = false;
        for (uint32_t i = first; i <= last; ++i)
            if (b->watch[i].load(std::memory_order_relaxed)) { any = true; hit |= 1ull << BucketOf(bank, i); }
        if (any && !brk) {
            g_watchEpoch.fetch_add(1, std::memory_order_acq_rel);
            static const bool brkB = (EnvInt("MSC_VGX_BREAK", 0) & 4) != 0; // (mutant: the writer bumps the bucket next to the page's)
            while (hit) { const int k = __builtin_ctzll(hit); hit &= hit - 1; g_watchBucket[brkB ? (k + 1) & (kBuckets - 1) : k].fetch_add(1, std::memory_order_acq_rel); }
        }
    }
    const uint64_t seq = g_epoch.fetch_add(1, std::memory_order_acq_rel);
    LogRec& r = g_log[seq % kLogSize];
    r.stamp.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r.p.store(p, std::memory_order_relaxed);
    r.n.store(size, std::memory_order_relaxed);
    r.stamp.store(seq + 1, std::memory_order_release);
}
} // namespace hle

} // namespace vgx
