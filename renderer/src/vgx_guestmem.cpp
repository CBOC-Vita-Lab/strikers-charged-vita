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
};
Bank g_banks[4];
int g_numBanks = 0;
std::atomic<uint64_t> g_epoch{0}; // bumped by every write notification

inline const Bank* FindBank(const uint8_t* p, uint32_t n) {
    for (int i = 0; i < g_numBanks; ++i) {
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
bool HaveGuestRam() { return g_numBanks != 0; }
uint64_t GuestWriteEpoch() { return g_epoch.load(std::memory_order_acquire); }

namespace hle {
void RegisterGuestRam(const void* hostBase, uint32_t size) {
    if (!hostBase || !size || g_numBanks >= 4) return;
    Bank& b = g_banks[g_numBanks];
    b.base = static_cast<const uint8_t*>(hostBase);
    b.size = size;
    const size_t n = (static_cast<size_t>(size) >> kShift) + 1;
    b.gen.reset(new std::atomic<uint32_t>[n]);
    for (size_t i = 0; i < n; ++i) b.gen[i].store(1, std::memory_order_relaxed);
    ++g_numBanks; // published last: readers on the render thread start after initialisation
}
void NotifyGuestWrite(const void* hostPtr, uint32_t size) {
    if (!size) return;
    const uint8_t* p = static_cast<const uint8_t*>(hostPtr);
    const Bank* b = FindBank(p, size);
    if (!b) return;
    const uint32_t first = static_cast<uint32_t>(p - b->base) >> kShift, last = static_cast<uint32_t>(p - b->base + size - 1) >> kShift;
    for (uint32_t i = first; i <= last; ++i) b->gen[i].fetch_add(1, std::memory_order_relaxed);
    g_epoch.fetch_add(1, std::memory_order_release);
}
} // namespace hle

} // namespace vgx
