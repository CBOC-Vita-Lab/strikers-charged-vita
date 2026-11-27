// gxp17 (23 Nov 2026): the pure part of the sampler's binary dump (vita_sampler.cpp, MSC_SAMPLER=1): a fixed-size open-addressing histogram of (pc, lr, meta) -> count and the writer of
// ux0:data/mscharged/sampler-<n>.bin. No system calls, no allocation (the caller hands the storage over), so tests/test_sampler_bin.cpp runs the real code on the host and
// tools/console-1.2/sampler_report.py reads the file. The shape follows the Galaxy port's sampler dump (same owner), written for this port's names.
//
// File layout (little endian, uint32 words unless noted): BinHeader (kBinHeaderBytes), then nEntries x BinEntry (16 bytes).
//   BinHeader: magic "MSCSAMP1" (8 bytes), version (1), index (n of sampler-<n>.bin), periodUs, intervalMs (length of this interval), windowMs (since start, at the end of
//     this interval), textBase / textSize (the running module's code segment, run-time addresses), anchorAddr (run-time address of msc_sampler_anchor; the ELF's address of the same
//     symbol gives the load offset), nEntries, dropped (samples that did not fit the table), runSamples, waitSamples, failed (failed context reads), pcWord, lrWord, ticks,
//     lateTicks (wake-ups that came 3 intervals late), samplerUs (CPU time of the sampler thread in this interval), tag[32] (MSC_SAMPLER_TAG, a run label), anchorName[32],
//     then kBinSlots x BinSlot.
//   BinSlot (48 bytes): uid, used, stops (suspend..resume pairs), holdUs (total time from before the suspend to after the resume), holdMaxUs, fails, name[24].
//   BinEntry: pc, lr, meta (slot | kind << 8 | waitType << 16, MakeMeta), count.
//     kind 0 = a running sample: pc and lr are code addresses. kind 1 = the thread was waiting: pc = the wait object's uid, lr = 0, waitType in meta.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace NpSampler {

constexpr int kBinSlots = 4;
constexpr uint32_t kKindRun = 0, kKindWait = 1;
constexpr uint32_t MakeMeta(uint32_t slot, uint32_t kind, uint32_t waitType) { return (slot & 0xFFu) | ((kind & 1u) << 8) | (waitType << 16); }

struct BinSlot {
    uint32_t uid, used, stops, holdUs, holdMaxUs, fails;
    char name[24];
};
struct BinHeader {
    char magic[8];
    uint32_t version, index, periodUs, intervalMs, windowMs, textBase, textSize, anchorAddr, nEntries, dropped, runSamples, waitSamples, failed, pcWord, lrWord, ticks, lateTicks, samplerUs;
    char tag[32];
    char anchorName[32];
    BinSlot slots[kBinSlots];
};
struct BinEntry { uint32_t pc, lr, meta, count; };
constexpr uint32_t kBinHeaderBytes = sizeof(BinHeader);
static_assert(sizeof(BinSlot) == 48, "BinSlot layout");
static_assert(sizeof(BinEntry) == 16, "BinEntry layout");
static_assert(sizeof(BinHeader) == 8 + 18 * 4 + 32 + 32 + kBinSlots * 48, "BinHeader layout (no padding)");

class Hist {
public:
    void Init(BinEntry* storage, uint32_t cap) { t_ = storage; cap_ = cap; Clear(); }   // cap: a power of two
    void Clear() { if (t_) std::memset(t_, 0, sizeof(BinEntry) * cap_); used_ = 0; dropped_ = 0; }
    // count == 0 marks a free entry, so a stored entry always has count >= 1. A table 7/8 full takes no new keys (the sample counts as dropped).
    void Add(uint32_t pc, uint32_t lr, uint32_t meta) {
        if (!cap_) return;
        uint32_t i = ((pc * 0x9e3779b1u) ^ (lr * 0x85ebca6bu) ^ (meta * 0xc2b2ae35u)) >> 7 & (cap_ - 1);
        for (uint32_t n = 0; n < 64; ++n, i = (i + 1) & (cap_ - 1)) {
            BinEntry& e = t_[i];
            if (e.count && e.pc == pc && e.lr == lr && e.meta == meta) { ++e.count; return; }
            if (!e.count) {
                if (used_ >= cap_ - cap_ / 8) break;
                e.pc = pc; e.lr = lr; e.meta = meta; e.count = 1; ++used_;
                return;
            }
        }
        ++dropped_;
    }
    uint32_t Used() const { return used_; }
    uint32_t Dropped() const { return dropped_; }
    const BinEntry* Data() const { return t_; }
    uint32_t Capacity() const { return cap_; }
private:
    BinEntry* t_ = nullptr;
    uint32_t cap_ = 0, used_ = 0, dropped_ = 0;
};

// Writes the header (nEntries and dropped filled in here) and the used entries. false on a short write.
inline bool WriteBin(FILE* f, BinHeader& h, const Hist& hist) {
    h.nEntries = hist.Used();
    h.dropped = hist.Dropped();
    if (std::fwrite(&h, sizeof h, 1, f) != 1) return false;
    uint32_t written = 0;
    for (uint32_t i = 0; i < hist.Capacity(); ++i)
        if (hist.Data()[i].count) { if (std::fwrite(&hist.Data()[i], sizeof(BinEntry), 1, f) != 1) return false; ++written; }
    return written == h.nEntries;
}

}  // namespace NpSampler
