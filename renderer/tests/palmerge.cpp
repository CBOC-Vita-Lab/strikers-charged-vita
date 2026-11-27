// 1.3 MSC_VGX_PALMERGE (vgx_draw.cpp "palette runs"), on the real front end: static skinned draws that differ only in the palette, recorded and presented,
// then every recorded draw executed on the CPU the way the palette vertex shader reads its matrices (the row in a_pos.w, below the clamp; three position
// rows and three normal rows from u_pal). The display-list cache is replaced by a few fake static buffers (the functions it gives the front end, below).
//   palmerge OUT   writes, per frame, the transformed vertices of every draw in draw order (position and normal rows as the shader reads them) to OUT and
//                  prints the draw records per frame. run_palmerge_test.sh runs it with the switch off and on: the two files must be the same, byte for byte,
//                  with fewer records when it is on.
#include "../src/vgx_draw.cpp"
#include <map>
#include <string>

using namespace vgx;

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf&) {}
}

// ---------------------------------------------------------------- fake static buffers (what vgx_dlcache.cpp gives the front end)
namespace {
struct FakeBuf { std::vector<uint8_t> vtx; std::vector<uint16_t> idx; uint32_t serial = 0; bool live = false; uint64_t freedAt = ~0ull; };
std::vector<FakeBuf> g_fake(1);
uint32_t g_serial = 0;
uint64_t g_kept = 0;
std::vector<uint32_t> g_freeList;
}
namespace vgx {
uint32_t AllocStaticBufferId() {
    uint32_t id;
    if (!g_freeList.empty()) { id = g_freeList.back(); g_freeList.pop_back(); }
    else { id = static_cast<uint32_t>(g_fake.size()); g_fake.emplace_back(); }
    g_fake[id] = FakeBuf{};
    g_fake[id].serial = ++g_serial;
    g_fake[id].live = true;
    return id;
}
uint32_t AllocStaticBufferIdFresh() {
    if (!g_freeList.empty() && g_fake[g_freeList.back()].freedAt == CurrentFrame()) {
        g_fake.emplace_back();
        g_fake.back().serial = ++g_serial;
        g_fake.back().live = true;
        return static_cast<uint32_t>(g_fake.size() - 1);
    }
    return AllocStaticBufferId();
}
void FreeStaticBufferId(uint32_t id) {
    if (!id || id >= g_fake.size()) return;
    g_buf.cmds.push_back(Cmd{CMD_BUF_FREE, id});
    g_fake[id].live = false;
    g_fake[id].freedAt = CurrentFrame();
    g_freeList.push_back(id);
}
const uint8_t* StaticSegVerts(uint32_t bufId, uint32_t serial, uint32_t byteOff, uint32_t bytes) {
    if (bufId >= g_fake.size() || !g_fake[bufId].live || g_fake[bufId].serial != serial || byteOff + bytes > g_fake[bufId].vtx.size()) return nullptr;
    return g_fake[bufId].vtx.data() + byteOff;
}
uint64_t PalKeptBytes() { return g_kept; }
uint8_t SBatchWhyNotKept(uint32_t) { return 9; }   // (the fake cache keeps no static-batch copies)
void SBatchWant(uint32_t) {}
void SBatchNoted(uint32_t) {}
void SBatchTrimKept(uint64_t) {}
uint64_t SBatchKeptBytes() { return 0; }
uint64_t SBatchRekept() { return 0; }
bool StaticBufferData(uint32_t, const uint8_t*&, const uint16_t*&) { return false; }
void DlNoteRecord(bool) {}
void DlCacheDiagnostic(uint64_t) {}
void DlCacheReport(FILE*) {}
void DlCacheBranchCounts(uint64_t out[8]) { for (int i = 0; i < 8; ++i) out[i] = 0; }
void CallList(const uint8_t*, uint32_t) {}
} // namespace vgx
extern "C" void VgxStadiumChanged(void) {}

// ---------------------------------------------------------------- the test
namespace {
uint32_t g_rng = 12345;
uint32_t Rand() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
float RandF() { return static_cast<float>(static_cast<int>(Rand() % 2001u) - 1000) / 37.0f; }

struct Seg { uint32_t bufId; uint32_t count; uint32_t maxRow; uint8_t gxPrim; };
std::vector<Seg> g_segs;
// The buffers the back end would hold: the fake ones as made, and every CMD_BUF_UPLOAD seen
std::map<uint32_t, BufUpload> g_gpu;
FILE* g_out = nullptr;
std::vector<unsigned> g_recs;
unsigned g_uploads = 0;

Seg MakeSeg(uint32_t count, uint32_t maxRow, uint8_t gxPrim) {
    Seg s{AllocStaticBufferId(), count, maxRow, gxPrim};
    FakeBuf& b = g_fake[s.bufId];
    const uint32_t stride = MakeLayout(0).stride;
    b.vtx.resize(static_cast<size_t>(count) * stride);
    for (uint32_t i = 0; i < count; ++i) {
        float v[4] = {RandF(), RandF(), RandF(), static_cast<float>(i == 0 ? maxRow : Rand() % (maxRow + 1))};
        std::memcpy(b.vtx.data() + static_cast<size_t>(i) * stride, v, 16);
    }
    b.idx.resize(PrimIndices(gxPrim, count, 0, nullptr));
    PrimIndices(gxPrim, count, 0, b.idx.data());
    g_kept += b.vtx.size();
    BufUpload up;
    up.bufId = s.bufId;
    up.vtx = b.vtx;
    up.idx = b.idx;
    g_gpu[s.bufId] = up;
    return s;
}
// New matrices in the position and normal matrix memory (what the game loads before each list).
void LoadMatrices() {
    uint32_t w[120];
    for (int i = 0; i < 120; ++i) { const float f = RandF(); std::memcpy(&w[i], &f, 4); }
    WriteXF(0x0000, 120, w);
    uint32_t n[90];
    for (int i = 0; i < 90; ++i) { const float f = RandF(); std::memcpy(&n[i], &f, 4); }
    WriteXF(0x0400, 90, n);
}
void Draw(const Seg& s) {
    LoadMatrices();
    SetPendingPosMtxMax(s.maxRow);
    g_staticSegVerts = g_fake[s.bufId].vtx.data();
    g_staticSegSerial = g_fake[s.bufId].serial;
    EmitStaticDraw(s.gxPrim, 0, s.count, s.bufId, 0, 0, 0);
    g_staticSegVerts = nullptr;
}
// The draws of a frame, executed: per index, eye = (dot(u_pal[pi], p) ...), n = (dot(u_pal[P + pi].xyz, p.xyz) ...), as the vertex shader does.
void Sink(CmdBuf& b) {
    unsigned recs = 0;
    for (const Cmd& c : b.cmds) {
        if (c.type == CMD_BUF_UPLOAD) { g_gpu[b.bufUploads[c.arg].bufId] = b.bufUploads[c.arg]; ++g_uploads; continue; }
        if (c.type == CMD_BUF_FREE) { g_gpu.erase(c.arg); continue; }
        if (c.type != CMD_DRAW) continue;
        const DrawRec& d = b.draws[c.arg];
        ++recs;
        const VsKey& k = g_vsReg.keys[d.vsId];
        if (k.perVtxPos != 2) { std::printf("FAIL: a draw without the short palette\n"); std::exit(1); }
        const int P = PalPosRows(k);
        auto it = g_gpu.find(d.vbuf);
        if (it == g_gpu.end()) { std::printf("FAIL: draw from buffer %u that the back end does not have\n", d.vbuf); std::exit(1); }
        const BufUpload& up = it->second;
        const uint32_t stride = MakeLayout(d.layoutMask).stride;
        const float* pal = b.uni.data() + d.ublock[UB_PAL];
        if (d.ublock[UB_PAL] + static_cast<size_t>(2 * P) * 4 > b.uni.size()) { std::printf("FAIL: palette block outside the arena\n"); std::exit(1); }
        for (uint32_t i = 0; i < d.idxCount; ++i) {
            const uint32_t ix = up.idx[d.idxOffset + i];
            const size_t at = d.vtxOffset + static_cast<size_t>(ix) * stride;
            if (at + 16 > up.vtx.size()) { std::printf("FAIL: index outside the buffer\n"); std::exit(1); }
            float p[4];
            std::memcpy(p, up.vtx.data() + at, 16);
            int pi = static_cast<int>(p[3] + 0.5f);
            if (pi > P - 3) pi = P - 3;
            float o[6];
            for (int r = 0; r < 3; ++r) {
                const float* row = pal + (pi + r) * 4;
                o[r] = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
                const float* nr = pal + (P + pi + r) * 4;
                o[3 + r] = nr[0] * p[0] + nr[1] * p[1] + nr[2] * p[2];
            }
            std::fwrite(o, sizeof o, 1, g_out);
        }
    }
    g_recs.push_back(recs);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    g_out = std::fopen(argv[1], "wb");
    if (!g_out) return 2;
    ResetState();
    WriteCP(0x50, 1u | (1u << 9)); // PNMTXIDX direct, position direct
    SetFrameSink(Sink);
    // segments: highest rows 27 (span 30), 12 (15), 9 (12), 27, 3 (6); triangles, a strip and a fan
    const Seg a = MakeSeg(30, 27, 0x90), bq = MakeSeg(20, 12, 0x98), c = MakeSeg(12, 9, 0xA0), d = MakeSeg(33, 27, 0x90), e = MakeSeg(9, 3, 0x90);
    for (int frame = 0; frame < 4; ++frame) {
        if (frame == 2) {
            // a buffer goes (as when its list is built again): the combined buffers made from it go with it
            PalMergeForget(bq.bufId);
        }
        Draw(a); Draw(bq); Draw(c);  // 30 + 15 + 12 = 57: one record at 60 rows
        Draw(d); Draw(a);            // 30 + 30 = 60: one record
        Draw(e);                     // no room after 60: a run of its own, joined by
        Draw(bq); Draw(c);           // 6 + 15 + 12 = 33: 60 rows
        // a render-state change ends a run: cull mode in genMode
        WriteBP((0x00u << 24) | (1u << 14));
        Draw(bq); Draw(c);           // 15 + 12 = 27: one record at the members' 30 rows (no new vertex shader)
        WriteBP(0x00u << 24);
        Draw(e);                     // alone: stays what it is
        FrontPresent(false, nullptr);
    }
    std::fclose(g_out);
    std::printf("records per frame:");
    for (unsigned r : g_recs) std::printf(" %u", r);
    std::printf("\nuploads %u\n", g_uploads);
    return 0;
}
