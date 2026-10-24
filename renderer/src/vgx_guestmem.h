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
// Did any write notified between epochs [from, to) overlap one of the ranges?
// True when it cannot tell (too many writes in between, or the log entry was
// overwritten meanwhile), so a caller can always treat true as "raced".
struct HostRange { const void* p; uint32_t n; };
bool GuestWritesTouch(uint64_t from, uint64_t to, const HostRange* ranges, int count);
}
