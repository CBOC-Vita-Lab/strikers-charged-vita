// Guest RAM classification and write generations (see vgx_guestmem.cpp).
#pragma once
#include <cstdint>

namespace vgx {
constexpr uint64_t kNotGuest = ~0ull;
// Sum of the write-generation counters under [p, p+n); kNotGuest when the
// range is not (entirely) inside registered guest RAM.
uint64_t GuestWriteGeneration(const void* p, uint32_t n);
bool IsGuestRam(const void* p, uint32_t n);
bool HaveGuestRam(); // has the runtime registered its memory banks?
// Changes whenever any write notification arrives (to detect a write that
// raced with a decode).
uint64_t GuestWriteEpoch();
// MSC_VGX_DLWATCH: pages that a cache entry depends on are marked "watched" (WatchRange, once per entry and never cleared); the watch epoch changes only
// when a write notification touches a watched page. An entry whose generations were read and found unchanged at watch epoch W is still valid while
// GuestWatchEpoch() is W: any write to one of its pages after that read would have moved it (the flag is set, then a full fence, then the epoch and the
// generations are read; the writer adds the generation, a full fence, then looks at the flag).
void WatchRange(const void* p, uint32_t n);
uint64_t GuestWatchEpoch();
// gem2b MSC_VGX_DLWATCH_BUCKETS: the same per bucket of 16 pages. WatchRangeMask watches the range (as WatchRange) and returns the buckets its pages are in; GuestWatchSum(mask) is the sum of those
// buckets' counters: an entry whose generations were read and found unchanged while the sum was S is still valid while the sum is S (a write to one of its pages bumps one of its buckets).
uint64_t WatchRangeMask(const void* p, uint32_t n);
uint64_t GuestWatchSum(uint64_t mask);
// Did any write notified between epochs [from, to) overlap one of the ranges?
// True when it cannot tell (too many writes in between, or the log entry was
// overwritten meanwhile), so a caller can always treat true as "raced".
struct HostRange { const void* p; uint32_t n; };
bool GuestWritesTouch(uint64_t from, uint64_t to, const HostRange* ranges, int count);
}
