#pragma once
// In the native port the memory the AX mixer works on is HOST-endian: the AX library (compiled
// from the decomp) writes its parameter blocks, command lists and buffers with ordinary C stores.
// The recompiled build's mixer read and wrote the console's big-endian words and swapped them; the
// swap sites of that code are kept (the structure of the mixer is unchanged and verified against the
// recomp) but their primitives are plain loads and stores here. Sample DATA (PCM16 on the disc) is
// still read as explicit big-endian byte pairs in the accelerator (ax_internal.h), because it is
// not converted at load.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BigEndian {
inline uint16_t Read16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t Read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void Write16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void Write32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
} // namespace BigEndian

namespace MemoryInline {
inline uint16_t ByteSwap16(uint16_t v) { return v; }
inline uint32_t ByteSwap32(uint32_t v) { return v; }
} // namespace MemoryInline

#define RT_LOGF(tag, ...) np_ax_log(__VA_ARGS__)
#define RT_TAG_AUDIO 0
extern "C" void np_ax_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
