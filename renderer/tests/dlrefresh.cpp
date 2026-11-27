// MSC_VGX_DLREFRESH (vgx_dlcache.cpp DynRefresh): the real display-list parser, cache, decoder and emitter. A list whose position array the game writes again before
// every draw is kept as decoded and only the arrays that changed are decoded again; the vertices drawn must be, byte for byte, what a full decode of the list gives
// at that call.
//   dlrefresh <skin|rigid|soft> <on|off|mutant> <dump file>
// Every call's list is first decoded in full by the test (DecodeVertices and the skin fill, as DrawStream does) and the vertices the frame draws are compared with
// that (skin, rigid: draws are not transformed on the CPU). The dump holds one line per frame (vertices, hash of every drawn vertex byte): the script compares the
// dump of `on` with the dump of `off` (the path of before: the bypass, the general parser at every call) for every scenario, `soft` included (small draws, transformed
// and merged on the CPU). `mutant` (MSC_VGX_DLREFRESH_BREAK=1: the last vertex of a primitive is not decoded again) must fail both.
// The schedule (200 frames, two calls a frame, positions written again before each): calls without a write (hits), a write that changes nothing, the normals written
// too, the list's index bytes changed (not refreshable), every array written at every call (more than half: the bypass), the position array bound elsewhere (a new variant).
#include "vgx_state.h"
#include "vgx_hle.h"
#include "vgx_ext.h"
#include "vgx_perf_diag.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
void DlCacheBranchCounts(uint64_t out[9]);
}
extern "C" int NpDiagActive(void) { return 0; }
extern "C" uint64_t NpDiagNow(void) { return 0; }
extern "C" void NpDiagEvent(uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}

namespace {
constexpr int kPos = 9, kNrm = 10, kClr0 = 11, kTex0 = 13, kTex1 = 14;
constexpr unsigned kElems = 256;
alignas(4096) uint8_t ram[65536];
uint8_t* const listAt = ram;
uint8_t* const posAt = ram + 8192;
uint8_t* const pos2At = ram + 12288;
uint8_t* const nrmAt = ram + 16384;
uint8_t* const texAt = ram + 20480;
uint8_t* const tex1At = ram + 24576;
uint8_t* const skinWAt = ram + 28672;
uint8_t* const skinIAt = ram + 32768;

std::map<unsigned, vgx::BufUpload> buffers;
std::vector<uint8_t> expect;   // the full decode of every call of the frame, in call order
unsigned failures = 0, frame = 0, framesChecked = 0;
uint64_t vertsDrawn = 0;
bool checkBytes = true;
FILE* dump = nullptr;
unsigned listBytes = 0;
bool skinOn = false;

void Require(bool ok, const char* message) {
    if (!ok) {
        if (failures < 12) std::fprintf(stderr, "FAIL: frame %u: %s\n", frame, message);
        ++failures;
    }
}
void PutBe16(uint8_t* p, unsigned v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void PutBeF(uint8_t* p, float f) { uint32_t u; std::memcpy(&u, &f, 4); p[0] = uint8_t(u >> 24); p[1] = uint8_t(u >> 16); p[2] = uint8_t(u >> 8); p[3] = uint8_t(u); }
void Wrote(const uint8_t* p, unsigned n) { vgx::hle::NotifyGuestWrite(p, n); }

// The frame as the back end gets it: every byte of every drawn vertex, in draw order.
void Observe(vgx::CmdBuf& b) {
    std::vector<uint8_t> drawn;
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
                Require(off + stride <= vertexBytes, "vertex bounds");
                if (off + stride > vertexBytes) continue;
                drawn.insert(drawn.end(), v + off, v + off + stride);
                ++vertsDrawn;
            }
        }
    }
    if (checkBytes) {
        Require(drawn.size() == expect.size(), "the frame draws as many vertex bytes as the full decodes of its calls");
        if (drawn.size() == expect.size() && std::memcmp(drawn.data(), expect.data(), drawn.size()) != 0) {
            size_t at = 0;
            while (drawn[at] == expect[at]) ++at;
            if (failures < 12) std::fprintf(stderr, "FAIL: frame %u: drawn vertices differ from the full decode at byte %lu of %lu\n", frame, (unsigned long)at, (unsigned long)drawn.size());
            ++failures;
        }
    }
    Require(!drawn.empty(), "the frame draws the list");
    if (dump) std::fprintf(dump, "frame %u: %lu vertex bytes, hash %016llx\n", frame, (unsigned long)drawn.size(), (unsigned long long)XXH3_64bits(drawn.data(), drawn.size()));
    ++framesChecked;
    expect.clear();
}

// The list decoded in full, as DrawStream does it, appended to `expect`; then the call.
void Call() {
    const uint8_t* p = listAt;
    const uint8_t* const end = p + listBytes;
    while (p < end) {
        const uint8_t cmd = *p++;
        if (cmd == 0) continue;
        const unsigned count = (unsigned(p[0]) << 8) | p[1];
        p += 2;
        const vgx::Plan& plan = vgx::CurrentPlan(cmd & 7);
        const bool skin = vgx::g_skin.on && vgx::SkinPosIndexed(plan);
        const uint32_t mask = skin ? (plan.layoutMask | vgx::VL_SKIN) : plan.layoutMask;
        const unsigned stride = vgx::MakeLayout(mask).stride;
        const size_t at = expect.size();
        expect.resize(at + size_t(count) * stride);
        vgx::DecodeVertices(plan, p, count, expect.data() + at, nullptr, skin && vgx::SkinDirect() ? stride : 0);
        if (skin) { if (vgx::SkinDirect()) vgx::SkinFill(plan, p, count, expect.data() + at); else vgx::SkinExpand(plan, p, count, expect.data() + at); }
        p += size_t(count) * plan.streamSize;
    }
    vgx::CallList(listAt, listBytes);
}

// positions: skin scenario big-endian f32 x 3, the others little-endian s16 x 3 (frac 8)
bool posF32 = true;
void WritePos(uint8_t* at, unsigned seed) {
    for (unsigned i = 0; i < kElems; ++i) {
        if (posF32) {
            PutBeF(at + i * 12, float(i) * 0.25f + float(seed) * 0.125f);
            PutBeF(at + i * 12 + 4, float(i % 17) - float(seed % 31));
            PutBeF(at + i * 12 + 8, float(seed) + float(i) * 0.5f);
        } else {
            const int16_t v[3] = {int16_t(i * 3 + seed), int16_t(int(i) - int(seed % 97)), int16_t(seed * 5 + i)};
            std::memcpy(at + i * 6, v, 6);
        }
    }
    Wrote(at, kElems * (posF32 ? 12u : 6u));
}
void WriteNrm(unsigned seed) {
    for (unsigned i = 0; i < kElems; ++i) {
        const float n[3] = {float(i + seed) * 0.001f, 1.0f - float(seed % 5) * 0.1f, float(i % 3)};
        if (skinOn) std::memcpy(nrmAt + i * 12, n, 12);            // little-endian in the skin scenario
        else for (int c = 0; c < 3; ++c) PutBeF(nrmAt + i * 12 + c * 4, n[c]);
    }
    Wrote(nrmAt, kElems * 12);
}
void WriteTex(unsigned seed) {
    for (unsigned i = 0; i < kElems; ++i) {
        PutBe16(texAt + i * 4, i * 5 + seed);
        PutBe16(texAt + i * 4 + 2, i + seed * 3);
        const float t[2] = {float(i) / 256.0f + float(seed), float(seed % 7)};
        std::memcpy(tex1At + i * 8, t, 8);
    }
    Wrote(texAt, kElems * 4);
    Wrote(tex1At, kElems * 8);
}
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: dlrefresh <skin|rigid|soft> <on|off|mutant> <dump file>\n"); return 2; }
    const std::string scene = argv[1], mode = argv[2];
    skinOn = scene == "skin";
    posF32 = skinOn;
    const bool soft = scene == "soft";
    checkBytes = !soft;
    setenv("MSC_VGX_DLREFRESH", mode == "off" ? "0" : "1", 1);
    if (mode == "mutant") setenv("MSC_VGX_DLREFRESH_BREAK", "1", 1);
    else unsetenv("MSC_VGX_DLREFRESH_BREAK");
    setenv("MSC_VGX_ATLAS", "0", 1);
    if (!soft) setenv("MSC_VGX_SOFTXFORM", "0", 1);
    dump = std::fopen(argv[3], "w");
    if (!dump) { std::perror(argv[3]); return 2; }
    vgx::ResetState();
    vgx::hle::RegisterGuestRam(ram, sizeof ram);

    unsigned at = 0;
    unsigned idxOf01 = 0;   // where the position index of the first vertex is in the list
    if (skinOn) {
        // position, normal, texture 0: 16-bit indices; texture 1: 8-bit index. One primitive of 300 vertices.
        vgx::SetVcd(kPos, 3); vgx::SetVcd(kNrm, 3); vgx::SetVcd(kTex0, 3); vgx::SetVcd(kTex1, 2);
        vgx::SetVat(0, kPos, 1, 4, 0); vgx::SetVat(0, kNrm, 0, 4, 0); vgx::SetVat(0, kTex0, 1, 3, 10); vgx::SetVat(0, kTex1, 1, 4, 0);
        listAt[at++] = 0x90; listAt[at++] = 300 >> 8; listAt[at++] = 300 & 255;
        idxOf01 = at;
        for (unsigned i = 0; i < 300; ++i) {
            PutBe16(listAt + at, (i * 7) % kElems); at += 2;
            PutBe16(listAt + at, (i * 3) % kElems); at += 2;
            PutBe16(listAt + at, (i * 11) % kElems); at += 2;
            listAt[at++] = uint8_t(i * 5);
        }
        for (unsigned i = 0; i < kElems; ++i) {
            for (int s = 0; s < 4; ++s) PutBeF(skinWAt + i * 16 + s * 4, s == 0 ? 0.75f : s == 1 ? 0.25f : 0.0f);
            skinIAt[i * 4] = uint8_t(i % 2); skinIAt[i * 4 + 1] = uint8_t((i + 1) % 2); skinIAt[i * 4 + 2] = 0; skinIAt[i * 4 + 3] = 0;
        }
        vgx::g_skin.on = true;
        vgx::g_skin.nBones = 2;
        vgx::g_skin.nVerts = kElems;
        vgx::g_skin.weights = skinWAt;
        vgx::g_skin.indices = skinIAt;
    } else {
        // position: 16-bit index; normal: 8-bit index; colour 0 and texture 0 in the stream. Two primitives.
        vgx::SetVcd(kPos, 3); vgx::SetVcd(kNrm, 2); vgx::SetVcd(kClr0, 1); vgx::SetVcd(kTex0, 1);
        vgx::SetVat(0, kPos, 1, 3, 8); vgx::SetVat(0, kNrm, 0, 4, 0); vgx::SetVat(0, kClr0, 1, 5, 0); vgx::SetVat(0, kTex0, 1, 2, 4);
        const unsigned counts[2] = {soft ? 60u : 210u, soft ? 30u : 120u};
        for (int pr = 0; pr < 2; ++pr) {
            listAt[at++] = 0x90; listAt[at++] = uint8_t(counts[pr] >> 8); listAt[at++] = uint8_t(counts[pr]);
            if (pr == 0) idxOf01 = at;
            for (unsigned i = 0; i < counts[pr]; ++i) {
                PutBe16(listAt + at, (i * 7 + pr * 13) % kElems); at += 2;
                listAt[at++] = uint8_t(i * 3 + pr);
                listAt[at++] = uint8_t(i); listAt[at++] = uint8_t(255 - i); listAt[at++] = uint8_t(pr * 100); listAt[at++] = 255;
                PutBe16(listAt + at, i * 9); at += 2;
                PutBe16(listAt + at, i + pr); at += 2;
            }
        }
    }
    listBytes = at;
    if (listBytes > 4096) { std::fprintf(stderr, "the list does not fit its page\n"); return 2; }
    auto bind = [](int attr, uint8_t* data, unsigned size, unsigned stride, bool le) {
        vgx::ArrayRef& a = vgx::g.arrays[attr];
        a.data = data; a.size = size; a.stride = stride; a.le = le;
    };
    const unsigned posStride = posF32 ? 12u : 6u;
    bind(kPos, posAt, kElems * posStride, posStride, !posF32);
    bind(kNrm, nrmAt, kElems * 12, 12, skinOn);
    if (skinOn) { bind(kTex0, texAt, kElems * 4, 4, false); bind(kTex1, tex1At, kElems * 8, 8, true); }
    WritePos(posAt, 0);
    WritePos(pos2At, 0);
    WriteNrm(0);
    WriteTex(0);
    Wrote(ram, sizeof ram);
    vgx::SetFrameSink(Observe);
    for (frame = 0; frame < 200; ++frame) {
        uint8_t* const pos = frame >= 180 ? pos2At : posAt;
        if (frame == 180) bind(kPos, pos2At, kElems * posStride, posStride, !posF32);   // (the array bound elsewhere: a new variant)
        if (frame == 20 || frame == 160) WriteNrm(frame);                               // (two arrays change)
        if (frame == 30) {                                                              // (the index data changes: not refreshable)
            std::swap(listAt[idxOf01], listAt[idxOf01 + 1]);
            listAt[idxOf01 + 1] ^= 1;
            Wrote(listAt, listBytes);
        }
        const bool all = frame >= 170 && frame < 173;                                   // (every array changes at every call: more than half, the bypass)
        if (all) { WriteNrm(frame * 2); WriteTex(frame * 2); }
        if (frame == 25) Wrote(pos, kElems * posStride);                                // (a write that changes nothing)
        else WritePos(pos, frame * 2 + 1);
        Call();
        if (frame % 7 != 5) WritePos(pos, frame * 2 + 2);                               // (else: called again unchanged)
        if (all) { WriteNrm(frame * 2 + 1); WriteTex(frame * 2 + 1); }
        Call();
        vgx::FrontPresent(true, nullptr);
    }
    uint64_t c[9] = {};
    vgx::DlCacheBranchCounts(c);
    std::fclose(dump);
    std::printf("%s %s: %u frames, %llu vertices drawn, %llu calls refreshed, %llu bypassed, %llu builds, %llu hits, %u failures\n", scene.c_str(), mode.c_str(), framesChecked,
                (unsigned long long)vertsDrawn, (unsigned long long)c[8], (unsigned long long)c[3], (unsigned long long)c[4], (unsigned long long)c[0], failures);
    vgx::DlCacheReport(stdout);
    if (mode == "off") {
        if (c[8] != 0 || c[3] < 200) { std::fprintf(stderr, "FAIL: off: expected no refresh and the bypass of before (%llu refreshed, %llu bypassed)\n", (unsigned long long)c[8], (unsigned long long)c[3]); return 1; }
    } else if (c[8] < 150 || c[3] == 0) {
        std::fprintf(stderr, "FAIL: %s: the refresh path (%llu calls) or the bypass after it (%llu calls) was not taken: the test proves nothing\n", mode.c_str(), (unsigned long long)c[8], (unsigned long long)c[3]);
        return 3;
    }
    return failures ? 1 : 0;
}
