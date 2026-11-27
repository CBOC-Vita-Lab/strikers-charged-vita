// 1.3 MSC_VGX_PROMOSOLO and MSC_VGX_DL_TINY (vgx_dlcache.cpp): the real display-list parser, cache and emitter. Lists are built as the game builds them (glxDisplayList.cpp:
// one primitive, no state commands). A CPU-transformed draw that merges with neighbours stays on the CPU path whatever its size; one that merges with nothing gets a static
// buffer after n replays, at most kSoloPerFrame a frame; every vertex is drawn either way.
//   dl_promosolo <off|on|budget|tiny>
#include "vgx_state.h"
#include "vgx_hle.h"
#include "vgx_perf_diag.h"
#include "vgx_workload.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vgx { bool LogEnabled() { return false; } void SubmitToBackend(CmdBuf& b) { backend::Execute(b); } }

namespace {
constexpr int kPos = 9;
constexpr unsigned kVerts = 15, kLists = 40, kListSize = 64;
alignas(4096) uint8_t ram[16384];
unsigned failures = 0, frame = 0, drawnVerts = 0, records = 0, staticRecords = 0;

void Require(bool ok, const char* message) {
    if (!ok) {
        if (failures < 12) std::fprintf(stderr, "FAIL: frame %u: %s\n", frame, message);
        ++failures;
    }
}
void Observe(vgx::CmdBuf& b) {
    records = staticRecords = drawnVerts = 0;
    for (const auto& c : b.cmds)
        if (c.type == vgx::CMD_DRAW) {
            const auto& d = b.draws.at(c.arg);
            ++records;
            if (d.vbuf) ++staticRecords;
            drawnVerts += d.vtxCount;
        }
}
uint8_t* ListAt(unsigned i) { return ram + i * kListSize; }
unsigned MakeList(unsigned i) {
    uint8_t* l = ListAt(i);
    unsigned at = 0;
    l[at++] = 0x90; l[at++] = 0; l[at++] = kVerts;
    for (unsigned v = 0; v < kVerts; ++v) l[at++] = static_cast<uint8_t>(v % 3);
    return at;
}
// A depth-mode write the neighbouring draw does not share: the merge test fails on the render state.
void Separate(unsigned k) { vgx::WriteBP((0x40u << 24) | ((k & 1u) ? 0x000017u : 0x000007u)); }
uint64_t Promoted() { uint64_t c[8]; vgx::DlCacheBranchCounts(c); return c[6]; }
uint64_t PromotedSolo() { uint64_t c[8]; vgx::DlCacheBranchCounts(c); return c[7]; }
uint64_t Hits() { uint64_t c[8]; vgx::DlCacheBranchCounts(c); return c[0]; }
}

extern "C" int NpDiagActive(void) { return 0; }
extern "C" uint64_t NpDiagNow(void) { return 0; }
extern "C" void NpDiagEvent(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "on";
    const bool on = !std::strcmp(mode, "on"), budget = !std::strcmp(mode, "budget"), tiny = !std::strcmp(mode, "tiny");
    setenv("MSC_VGX_SOFTXFORM", "256", 1);
    setenv("MSC_VGX_ATLAS", "0", 1);
    setenv("MSC_VGX_PROMOSOLO", on || budget ? "3" : "0", 1);
    if (tiny) setenv("MSC_VGX_DL_TINY", "64", 1);
    vgx::ResetState();
    vgx::hle::RegisterGuestRam(ram, sizeof ram);
    vgx::SetVcd(kPos, 2);
    vgx::SetVat(0, kPos, 1, 4, 0);
    unsigned bytes = 0;
    for (unsigned i = 0; i < kLists; ++i) bytes = MakeList(i);
    uint8_t* arr = ram + 8192;
    for (unsigned i = 0; i < 3; ++i) {
        const float p[3] = {float(i + 1), float(i + 11), float(i + 21)};
        std::memcpy(arr + i * 12, p, sizeof p);
    }
    vgx::hle::NotifyGuestWrite(ram, sizeof ram);
    vgx::ArrayRef& a = vgx::g.arrays[kPos];
    a.data = arr; a.size = 36; a.stride = 12; a.le = true;
    vgx::SetFrameSink(Observe);

    if (budget) {
        // Every list alone: promoted at the rate the budget allows, never more.
        for (frame = 0; frame < 12; ++frame) {
            const uint64_t before = Promoted();
            for (unsigned i = 0; i < kLists; ++i) { Separate(i); vgx::CallList(ListAt(i), bytes); }
            vgx::FrontPresent(true, nullptr);
            Require(Promoted() - before <= 16, "no more than kSoloPerFrame promotions a frame");
            Require(drawnVerts == kLists * kVerts, "every vertex is drawn");
            Require(records == kLists, "one record per lone list");
        }
        Require(Promoted() == kLists && PromotedSolo() == kLists, "the budget only delays them");
        Require(staticRecords == kLists, "all of them static in the end");
    } else {
        // lists 0, 1, 2: a run, each a draw that merges (the head of the run through the draw after it); list 3: alone between two state changes.
        const unsigned rounds = 12;
        for (frame = 0; frame < rounds; ++frame) {
            Separate(0);
            for (unsigned i = 0; i < 3; ++i) vgx::CallList(ListAt(i), bytes);
            Separate(1);
            vgx::CallList(ListAt(3), bytes);
            vgx::FrontPresent(true, nullptr);
            Require(drawnVerts == 4 * kVerts, "every vertex is drawn");
        }
        vgx::DlCacheReport(stdout);
        if (on) {
            Require(Promoted() == 1 && PromotedSolo() == 1, "the lone list alone is promoted");
            Require(records == 2 && staticRecords == 1, "the run stays one merged record, the lone list is a static draw");
        } else {
            Require(Promoted() == 0, "off: nothing small is promoted");
            Require(records == 2 && staticRecords == 0, "off: the run is one record, the lone list another, both in the arena");
        }
        if (tiny) Require(Hits() == 0, "a list within MSC_VGX_DL_TINY never reaches the cache");
        else Require(Hits() > 0, "the default keeps lists of 16 bytes and more in the cache");
    }
    std::printf("promosolo %s: %u records, %u static, %llu promoted, %llu by PROMOSOLO, %u failures\n", mode, records, staticRecords, (unsigned long long)Promoted(),
                (unsigned long long)PromotedSolo(), failures);
    return failures != 0;
}
