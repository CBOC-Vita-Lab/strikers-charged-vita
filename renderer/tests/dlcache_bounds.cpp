// 1.2 MSC_VGX_DLCACHE_BOUNDS_GUARD (vgx_dlcache.cpp Build, vgx_state.cpp DecodeVertices): the real display-list parser, cache and emitter. A list whose
// index reaches past the end of its array decodes as zeros, and the cache's identity (which bytes of the array it read) leaves that element out. When the
// array is bound again with the room it should have had, at the same address, nothing the entry depends on has been written: the cached zeros were drawn
// from then on. With the guard on such a list is not cached (the general parser decodes it, as it did the first time), and the picture follows the array.
//   dlcache_bounds <on|off>
// guard off is the control: it must show the stale zeros, or the test proves nothing.
#include "vgx_state.h"
#include "vgx_hle.h"
#include "vgx_perf_diag.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace vgx { bool LogEnabled() { return false; } void SubmitToBackend(CmdBuf& b) { backend::Execute(b); } }
extern "C" void VgxStadiumChanged(void);

namespace {
constexpr int kPos = 9;   // GX_VA_POS
alignas(4096) uint8_t ram[16384];
std::map<unsigned, vgx::BufUpload> buffers;
unsigned failures = 0, frame = 0, vertsSeen = 0, staleVerts = 0, goodVerts = 0;
bool guard = true;
unsigned arrayBytes = 12;   // how much of the array the binding covers
bool inspectCache = false, cacheInspected = false;
uint64_t cacheBytes, cacheStaticBytes, cacheStaticBuffers, cacheLists;
constexpr uint64_t kTeardownTag = 0x54454152444f574eull;

void Require(bool ok, const char* message) {
    if (!ok) {
        if (failures < 12) std::fprintf(stderr, "FAIL: frame %u: %s\n", frame, message);
        ++failures;
    }
}

// The frame as the back end would get it: the positions of every drawn vertex.
void Observe(vgx::CmdBuf& b) {
    unsigned verts = 0, wrong = 0;
    for (const auto& c : b.cmds) {
        if (c.type == vgx::CMD_BUF_UPLOAD) {
            const auto& up = b.bufUploads.at(c.arg);
            buffers[up.bufId] = up;
        } else if (c.type == vgx::CMD_BUF_FREE) {
            buffers.erase(c.arg);
        } else if (c.type == vgx::CMD_DRAW) {
            const auto& d = b.draws.at(c.arg);
            const unsigned stride = vgx::MakeLayout(d.layoutMask).stride;
            const uint8_t* v;
            const uint16_t* idx;
            size_t vertexBytes, indexCount;
            if (d.vbuf) {
                const auto& up = buffers.at(d.vbuf);
                v = up.vtx.data(); idx = up.idx.data();
                vertexBytes = up.vtx.size(); indexCount = up.idx.size();
            } else {
                const size_t chunk = b.vtxChunks.empty() ? 0 : b.vtxChunks.at(d.vchunk);
                v = b.vtx.data() + chunk; idx = b.idx.data();
                vertexBytes = b.vtx.size() - chunk; indexCount = b.idx.size();
            }
            Require(size_t(d.idxOffset) + d.idxCount <= indexCount, "index bounds");
            if (size_t(d.idxOffset) + d.idxCount > indexCount) continue;
            for (unsigned i = 0; i < d.idxCount; ++i) {
                const size_t off = d.vtxOffset + size_t(idx[d.idxOffset + i]) * stride;
                Require(off + 12 <= vertexBytes, "vertex bounds");
                if (off + 12 > vertexBytes) continue;
                float p[3];
                std::memcpy(p, v + off, sizeof p);
                const unsigned element = verts % 3;
                // what the current binding says this vertex is: its element if the array covers it, else zeros
                const bool covered = (element + 1) * 12 <= arrayBytes;
                const float x = covered ? float(element + 1) : 0.0f;
                const bool ok = p[0] == x && p[1] == (covered ? x + 10 : 0.0f) && p[2] == (covered ? x + 20 : 0.0f);
                if (!ok) {
                    ++wrong;
                    if (p[0] == 0.0f && (element + 1) * 12 <= arrayBytes) ++staleVerts;
                } else if (covered) {
                    ++goodVerts;
                }
                ++verts;
            }
        }
    }
    vertsSeen += verts;
    Require(verts == 48, "all 48 indexed vertices drawn");
    Require(wrong == 0, "the drawn positions follow the current array binding");
}

void TeardownCache() {
    // Finish the stadium lifecycle rather than exiting with a deliberately live cache entry.
    vgx::SetFrameSink(nullptr);
    VgxStadiumChanged();
    for (unsigned i = 0; i < 242; ++i) {
        vgx::CallList(ram + 128, 16);   // uncachable NOPs advance the documented 240-use-frame flush delay
        vgx::FrontPresent(true, nullptr);
    }
    inspectCache = true;
    vgx::DlCacheDiagnostic(kTeardownTag);
    inspectCache = false;
    Require(cacheInspected && cacheBytes == 0 && cacheStaticBytes == 0 && cacheStaticBuffers == 0 && cacheLists == 0,
            "stadium teardown frees all cached geometry");
    std::printf("teardown: %llu decoded bytes, %llu static bytes, %llu static buffers, %llu lists\n",
                (unsigned long long)cacheBytes, (unsigned long long)cacheStaticBytes,
                (unsigned long long)cacheStaticBuffers, (unsigned long long)cacheLists);
}
}

extern "C" int NpDiagActive(void) { return inspectCache; }
extern "C" uint64_t NpDiagNow(void) { return 0; }
extern "C" void NpDiagEvent(uint32_t kind, uint32_t flags, uint64_t id, uint64_t a, uint64_t b, uint64_t c, uint64_t, uint64_t e) {
    if (kind == NPD_GEOMETRY && flags == 0 && id == kTeardownTag) {
        cacheInspected = true;
        cacheBytes = a; cacheStaticBytes = b; cacheStaticBuffers = c; cacheLists = e;
    }
}

int main(int argc, char** argv) {
    guard = argc < 2 || std::strcmp(argv[1], "off") != 0;
    setenv("MSC_VGX_DLCACHE_BOUNDS_GUARD", guard ? "1" : "0", 1);
    setenv("MSC_VGX_ATLAS", "0", 1);
    vgx::ResetState();
    vgx::hle::RegisterGuestRam(ram, sizeof ram);
    // position: indexed (8 bit), 3 floats; format 0
    vgx::SetVcd(kPos, 2);
    vgx::SetVat(0, kPos, 1, 4, 0);
    // the list: 48 vertices, indices 0 1 2 0 1 2 ...
    uint8_t* list = ram;
    unsigned at = 0;
    list[at++] = 0x90; list[at++] = 0; list[at++] = 48;
    for (unsigned i = 0; i < 48; ++i) list[at++] = static_cast<uint8_t>(i % 3);
    uint8_t* arr = ram + 4096;
    for (unsigned i = 0; i < 3; ++i) {
        const float p[3] = {float(i + 1), float(i + 11), float(i + 21)};
        std::memcpy(arr + i * 12, p, sizeof p);
    }
    vgx::hle::NotifyGuestWrite(ram, sizeof ram);
    vgx::SetFrameSink(Observe);
    for (frame = 0; frame < 8; ++frame) {
        // frames 0..2: the binding covers one element only; from frame 3 all three (same address, no write)
        arrayBytes = frame < 3 ? 12 : 36;
        vgx::ArrayRef& a = vgx::g.arrays[kPos];
        a.data = arr; a.size = arrayBytes; a.stride = 12; a.le = true;
        vgx::CallList(list, at);
        vgx::FrontPresent(true, nullptr);
    }
    std::printf("guard %s: %u vertices drawn, %u stale (zeros where the array had data), %u failures\n", guard ? "on" : "off", vertsSeen, staleVerts, failures);
    vgx::DlCacheReport(stdout);
    if (guard) {
        Require(staleVerts == 0, "guard: no stale zeros after the array is bound with room");
        Require(goodVerts > 0, "guard: the recovered geometry is drawn");
    } else {
        // control: the 1.1 cache keeps the zeros, so the picture differs from the binding (the failures counted above are that difference)
        if (staleVerts == 0) { std::fprintf(stderr, "FAIL: control: the unguarded cache did not show the stale zeros: the test proves nothing\n"); return 1; }
        std::printf("control: the unguarded cache drew stale zeros for %u vertices after the array was bound with room (as in 1.1)\n", staleVerts);
    }
    const unsigned beforeTeardown = failures;
    TeardownCache();
    return guard ? bool(failures) : failures != beforeTeardown;
}
