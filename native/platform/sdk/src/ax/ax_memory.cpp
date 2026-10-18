// Native replacement for the recompiled build's guest-memory glue of the AX mix (ax_memory.cpp of
// the runtime): the mix works on host pointers here (see ax_internal.h).
#include "ax_internal.h"
#include "ax_mix_kernels.h"
#include "ax_mix_fast.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "np/np_plat.h"

namespace AxDspHle {
namespace {
std::atomic<bool> g_loggedMixAddressOutOfRange{false};
} // namespace

uint32_t g_axTaskPtr = 0;
std::atomic<uint8_t*> g_mixMem1{nullptr};
std::atomic<uint8_t*> g_mixMem2{nullptr};
thread_local bool t_onMixWorker = false;

void ReportMixAddressOutOfRange(uint32_t addr, size_t bytes) {
    if (g_loggedMixAddressOutOfRange.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    np_log("ax", "the mix skipped an access to a null address (0x%08X, %u bytes)", addr,
           static_cast<unsigned>(bytes));
}

uint32_t HashEctor(const uint8_t* data, uint32_t length) {
    if (!data || length == 0 || length > 0x10000u) {
        return 0;
    }
    uint32_t crc = 0;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= data[i];
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

void WriteGuestS32Buffer(uint32_t addr, const int* src, size_t count) {
    if (!addr) {
        return;
    }
    if (uint8_t* host = MixResolveRange(addr, count * sizeof(uint32_t))) {
        MixNoteWrite(host, count * sizeof(uint32_t));
        std::memcpy(host, src, count * sizeof(uint32_t));
    }
}

void ReadGuestS32Buffer(uint32_t addr, int* dst, size_t count) {
    if (const uint8_t* host = MixResolveRange(addr, count * sizeof(uint32_t))) {
        std::memcpy(dst, host, count * sizeof(uint32_t));
        return;
    }
    ReportMixAddressOutOfRange(addr, count * sizeof(uint32_t));
    std::fill(dst, dst + count, 0);
}

} // namespace AxDspHle

extern "C" void np_ax_log(const char* fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    np_log("ax", "%s", line);
}
