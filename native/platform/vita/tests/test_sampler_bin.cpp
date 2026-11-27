// gxp17: host test of np_sampler_bin.h (the pc/lr histogram and the .bin writer of the on-console sampler) and, with -DMAKE_BIN, the writer of the synthetic .bin the report script test reads.
// Mutants: run_sampler_bin_test.sh copies the header, changes one line with sed, and the test must fail: (1) the key ignores lr, (2) a full table keeps taking keys, (3) MakeMeta puts the kind in the wrong bit,
// (4) the writer leaves out the last entry.
#include "np_sampler_bin.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

#ifdef MAKE_BIN
// usage: t_makebin OUT.bin ANCHOR_RT HOT_RT CALLER_RT   writes a bin of two threads: "game" 700 samples of pc=HOT lr=CALLER, 100 of pc=HOT lr=0x0, 200 waits; "worker" 50 of pc=CALLER
int main(int, char** argv) {
    static NpSampler::BinEntry store[1024];
    NpSampler::Hist h; h.Init(store, 1024);
    const uint32_t anchor = std::strtoul(argv[2], nullptr, 16), hot = std::strtoul(argv[3], nullptr, 16), caller = std::strtoul(argv[4], nullptr, 16);
    for (int i = 0; i < 700; ++i) h.Add(hot, caller, NpSampler::MakeMeta(0, NpSampler::kKindRun, 0));
    for (int i = 0; i < 100; ++i) h.Add(hot, 0, NpSampler::MakeMeta(0, NpSampler::kKindRun, 0));
    for (int i = 0; i < 200; ++i) h.Add(0x40010123u, 0, NpSampler::MakeMeta(0, NpSampler::kKindWait, 2));
    for (int i = 0; i < 50; ++i) h.Add(caller, 0, NpSampler::MakeMeta(1, NpSampler::kKindRun, 0));
    NpSampler::BinHeader hd; std::memset(&hd, 0, sizeof hd);
    std::memcpy(hd.magic, "MSCSAMP1", 8); hd.version = 1; hd.index = 1; hd.periodUs = 1000; hd.intervalMs = 1000; hd.windowMs = 1000; hd.anchorAddr = anchor; hd.runSamples = 850; hd.waitSamples = 200;
    hd.ticks = 1000; hd.lateTicks = 3; hd.samplerUs = 12000;
    std::snprintf(hd.tag, sizeof hd.tag, "synthetic"); std::snprintf(hd.anchorName, sizeof hd.anchorName, "msc_sampler_anchor");
    hd.slots[0].used = 1; hd.slots[0].uid = 0x40010001; std::snprintf(hd.slots[0].name, 24, "game"); hd.slots[0].stops = 800; hd.slots[0].holdUs = 40000; hd.slots[0].holdMaxUs = 90;
    hd.slots[1].used = 1; hd.slots[1].uid = 0x40010002; std::snprintf(hd.slots[1].name, 24, "worker"); hd.slots[1].stops = 50; hd.slots[1].holdUs = 2000; hd.slots[1].holdMaxUs = 60;
    FILE* f = std::fopen(argv[1], "wb");
    const bool ok = f && NpSampler::WriteBin(f, hd, h);
    if (f) std::fclose(f);
    return ok ? 0 : 1;
}
#else
int main() {
    static NpSampler::BinEntry store[256];
    NpSampler::Hist h; h.Init(store, 256);
    for (int i = 0; i < 10; ++i) h.Add(0x81001000u, 0x81002000u, NpSampler::MakeMeta(0, NpSampler::kKindRun, 0));
    for (int i = 0; i < 5; ++i) h.Add(0x81001000u, 0x81003000u, NpSampler::MakeMeta(0, NpSampler::kKindRun, 0));   // same pc, other caller: its own pair
    for (int i = 0; i < 3; ++i) h.Add(0x81001000u, 0x81002000u, NpSampler::MakeMeta(1, NpSampler::kKindRun, 0));   // other target: its own pair
    for (int i = 0; i < 4; ++i) h.Add(0x40010123u, 0, NpSampler::MakeMeta(0, NpSampler::kKindWait, 2));
    CHECK(h.Used() == 4);
    CHECK(h.Dropped() == 0);
    uint32_t total = 0, c10 = 0, c5 = 0;
    for (uint32_t i = 0; i < h.Capacity(); ++i) { const NpSampler::BinEntry& e = h.Data()[i]; total += e.count; if (e.count == 10) ++c10; if (e.count == 5) ++c5; }
    CHECK(total == 22 && c10 == 1 && c5 == 1);
    // a table that fills drops, and counts the drops: total + dropped == samples added
    h.Clear();
    for (uint32_t k = 1; k <= 1000; ++k) h.Add(k * 4u, 0, 0);
    CHECK(h.Used() == 256 - 256 / 8);
    CHECK(h.Dropped() == 1000 - h.Used());
    // the writer: header + used entries, read back
    h.Clear();
    for (int i = 0; i < 7; ++i) h.Add(0x81001000u, 0x81002000u, 0);
    h.Add(0x81003000u, 0x81002000u, 0);
    NpSampler::BinHeader hd; std::memset(&hd, 0, sizeof hd);
    std::memcpy(hd.magic, "MSCSAMP1", 8);
    FILE* f = std::tmpfile();
    CHECK(f && NpSampler::WriteBin(f, hd, h));
    CHECK(hd.nEntries == 2 && hd.dropped == 0);
    std::fseek(f, 0, SEEK_END);
    CHECK(static_cast<size_t>(std::ftell(f)) == sizeof(NpSampler::BinHeader) + 2 * sizeof(NpSampler::BinEntry));
    std::fseek(f, static_cast<long>(sizeof hd), SEEK_SET);
    NpSampler::BinEntry e[2];
    CHECK(std::fread(e, sizeof(NpSampler::BinEntry), 2, f) == 2);
    CHECK(e[0].count + e[1].count == 8);
    std::fclose(f);
    CHECK(NpSampler::MakeMeta(2, NpSampler::kKindWait, 0x40) == (2u | (1u << 8) | (0x40u << 16)));
    std::printf(fails ? "FAILED\n" : "sampler bin: all checks passed\n");
    return fails ? 1 : 0;
}
#endif
