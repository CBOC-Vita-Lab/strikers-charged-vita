// np_fatal.cpp (1.2 audit): see np/fatal.h. The game's nlBreak and the SDK layer's OSPanic / OSFatal end here before they end the program,
// so that a quiet (NP_LOGGING=0) build still says which path was taken and from where.
#include "np/fatal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" void NpPlatformFatalLine(const char* line) __attribute__((weak));     // the platform's sink (one line, no newline needed)
extern "C" int NpPlatformStackHigh(uintptr_t* high) __attribute__((weak));       // the top of the calling thread's stack; 0 = unknown

#if defined(__vita__)
extern "C" char __executable_start;   // linker script: start of the image
extern "C" char __bss_start;          // ... and the end of its code and read-only data

namespace {
// A Thumb return address: odd, inside the image, and the 16 or 32 bits before it are a call (BL / BLX immediate or BLX register).
bool LooksLikeReturn(uintptr_t v) {
    if (!(v & 1u)) return false;
    const uintptr_t a = v & ~static_cast<uintptr_t>(1);
    if (a < reinterpret_cast<uintptr_t>(&__executable_start) + 8u || a >= reinterpret_cast<uintptr_t>(&__bss_start)) return false;
    const uint16_t* p = reinterpret_cast<const uint16_t*>(a);
    const uint16_t lo = p[-1], hi = p[-2];
    if ((hi & 0xF800u) == 0xF000u && (lo & 0xC000u) == 0xC000u) return true;   // BL / BLX imm
    return (lo & 0xFF87u) == 0x4780u;                                          // BLX reg
}
} // namespace
#endif

extern "C" void NpFatalNote(const char* kind, const void* caller, const char* detail) {
    char line[560];
    int n = snprintf(line, sizeof line, "%s from %p", kind ? kind : "?", caller);
    if (n < 0) return;
    if (detail && *detail && static_cast<size_t>(n) < sizeof line) n += snprintf(line + n, sizeof line - static_cast<size_t>(n), ": %s", detail);
#if defined(__vita__)
    // The return addresses on the stack above this frame, closest first (at most 8; the first is usually `caller`'s own caller).
    uintptr_t high = 0;
    if (NpPlatformStackHigh && !NpPlatformStackHigh(&high)) high = 0;
    // (high == 0: the top of this thread's stack is unknown, and a scan could read unmapped memory: the line then has no backtrace)
    if (high != 0) {
        const uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        uintptr_t end = sp + 2048u;
        if (end > high) end = high;
        int found = 0;
        for (uintptr_t at = (sp + 3u) & ~static_cast<uintptr_t>(3); at + 4u <= end && found < 8 && static_cast<size_t>(n) + 12u < sizeof line; at += 4u) {
            const uintptr_t v = *reinterpret_cast<const uintptr_t*>(at);
            if (!LooksLikeReturn(v)) continue;
            n += snprintf(line + n, sizeof line - static_cast<size_t>(n), "%s0x%08X", found ? " " : " | stack: ", static_cast<unsigned>(v & ~static_cast<uintptr_t>(1)));
            ++found;
        }
    }
#endif
    if (NpPlatformFatalLine) NpPlatformFatalLine(line);
    else fprintf(stderr, "[fatal] %s\n", line);
}
