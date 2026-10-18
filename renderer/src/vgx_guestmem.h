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
}
