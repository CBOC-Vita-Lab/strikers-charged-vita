// 1.3: MSC_VGX_CMPRA and MSC_VGX_TEX16. The formats themselves, and what the texture cache sends with each switch.
#include "vgx_state.h"
#include "vgx_texdecode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
State g;
uint32_t g_gxd = 0x3FFFFFFFu;
static CmdBuf commands;
CmdBuf& Pending() { return commands; }
const char* ShaderListPath() { return ""; }   // (vgx_draw.cpp, not linked here)
uint64_t CurrentFrame() { return 1; }
namespace stats {
Frame& Cur() { static Frame f; return f; }
void TexFormat(uint32_t, uint16_t, uint16_t, bool) {}
void TexLive(int32_t, int64_t) {}
void Note(const char*) {}
}
}

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Pal565(uint32_t c, int* o) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = (r << 3) | (r >> 2); o[1] = (g << 2) | (g >> 4); o[2] = (b << 3) | (b >> 2);
}
// A DXT5 decoder as the S3TC extension specifies it: the colour block always has four entries.
void DecodeDxt5Block(const uint8_t* b, int out[16][4]) {
    const uint32_t c0 = b[8] | b[9] << 8, c1 = b[10] | b[11] << 8;
    int pal[4][3];
    Pal565(c0, pal[0]);
    Pal565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
    uint64_t abits = 0;
    for (int i = 0; i < 6; ++i) abits |= static_cast<uint64_t>(b[2 + i]) << (i * 8);
    for (int i = 0; i < 16; ++i) {
        const int ci = (b[12 + i / 4] >> ((i & 3) * 2)) & 3;
        const int ai = static_cast<int>((abits >> (i * 3)) & 7);
        Check(ai < 2, "alpha uses the two end entries only");
        for (int k = 0; k < 3; ++k) out[i][k] = pal[ci][k];
        out[i][3] = ai == 0 ? b[0] : b[1];
    }
}

void TestDxt5() {
    // Every combination of block kind on a 16 x 16 level (two tiles a side).
    const uint32_t w = 16, h = 16;
    std::vector<uint8_t> src(vgx::TexLevelSize(w, h, 0xE));
    uint32_t rnd = 12345;
    const auto next = [&] { rnd = rnd * 1664525u + 1013904223u; return rnd >> 16; };
    for (size_t off = 0; off + 8 <= src.size(); off += 8) {
        uint32_t c0 = next() & 0xFFFF, c1 = next() & 0xFFFF;
        const uint32_t kind = (off / 8) % 3;
        if (kind == 0 && c0 <= c1) std::swap(c0, c1);   // four colours
        if (kind == 1 && c0 > c1) std::swap(c0, c1);    // three colours and transparent
        if (kind == 2) c1 = c0;                         // one colour
        src[off] = static_cast<uint8_t>(c0 >> 8); src[off + 1] = static_cast<uint8_t>(c0);
        src[off + 2] = static_cast<uint8_t>(c1 >> 8); src[off + 3] = static_cast<uint8_t>(c1);
        for (int i = 0; i < 4; ++i) src[off + 4 + i] = static_cast<uint8_t>(next());
    }
    std::vector<uint8_t> rgba(w * h * 4), dxt(vgx::Dxt5Size(w, h));
    vgx::DecodeTexture(src.data(), w, h, 0xE, nullptr, 0, 0, rgba.data());
    vgx::CmprToDxt5(src.data(), w, h, dxt.data());
    unsigned transparent = 0;
    for (uint32_t by = 0; by < 4; ++by)
        for (uint32_t bx = 0; bx < 4; ++bx) {
            const uint8_t* b = dxt.data() + (by * 4 + bx) * 16;
            const uint32_t e0 = b[8] | b[9] << 8, e1 = b[10] | b[11] << 8;
            Check(e0 >= e1, "the larger endpoint goes first");
            int px[16][4];
            DecodeDxt5Block(b, px);
            int ep0[3], ep1[3];
            Pal565(e0, ep0);
            Pal565(e1, ep1);
            for (int i = 0; i < 16; ++i) {
                const uint8_t* want = &rgba[((by * 4 + i / 4) * w + bx * 4 + (i & 3)) * 4];
                Check(px[i][3] == want[3], "alpha is the decoded alpha, texel by texel");
                transparent += want[3] == 0;
                for (int k = 0; k < 3; ++k) {
                    // A sixth of the endpoints' distance for a middle colour; 3/8 against 1/3 (GX's weights and DXT's, as the DXT1 path has) for the four-colour entries.
                    const int slack = std::abs(ep0[k] - ep1[k]) / 6 + 2;
                    Check(std::abs(px[i][k] - want[k]) <= slack, "colour within the stated bound");
                }
            }
        }
    Check(transparent > 0, "the test level has transparent texels");
    // MSC_VGX_CMPRA=2: CmprDxt5MaxError is the largest colour error of the three-colour blocks' texels, within one step (the decoders round their blends differently).
    uint32_t brute = 0;
    for (uint32_t by = 0; by < 4; ++by)
        for (uint32_t bx = 0; bx < 4; ++bx) {
            const uint8_t* sb = src.data() + ((by >> 1) * 2 + (bx >> 1)) * 4 * 8 + ((by & 1) * 2 + (bx & 1)) * 8;
            const uint32_t c0 = sb[0] << 8 | sb[1], c1 = sb[2] << 8 | sb[3];
            if (c0 >= c1) continue;
            int px[16][4];
            DecodeDxt5Block(dxt.data() + (by * 4 + bx) * 16, px);
            for (int i = 0; i < 16; ++i) {
                const uint8_t* want = &rgba[((by * 4 + i / 4) * w + bx * 4 + (i & 3)) * 4];
                for (int k = 0; k < 3; ++k) brute = std::max<uint32_t>(brute, static_cast<uint32_t>(std::abs(px[i][k] - want[k])));
            }
        }
    const uint32_t reported = vgx::CmprDxt5MaxError(src.data(), w, h);
    Check(reported > 0 && std::abs(static_cast<int>(reported) - static_cast<int>(brute)) <= 1, "CmprDxt5MaxError matches the measured error of the three-colour blocks");
    // A level of four-colour blocks and one-colour blocks has no middle colour: error 0.
    std::vector<uint8_t> plain(src.size(), 0);
    for (size_t off = 0; off + 8 <= plain.size(); off += 8) { plain[off] = 0xF8; plain[off + 2] = 0x00; plain[off + 3] = 0x1F; std::memset(&plain[off + 4], 0x1B, 4); }
    Check(vgx::CmprDxt5MaxError(plain.data(), w, h) == 0, "no three-colour block, no error");
}

void Test16() {
    using L = std::vector<std::vector<uint8_t>>;
    L l565{{0x08, 0x04, 0x10, 255, 0xFF, 0xFF, 0xFF, 255, 0, 0, 0, 255}}; // 5-bit 1 -> 8, 6-bit 1 -> 4, 5-bit 2 -> 16
    Check(vgx::Rgba16Format(l565, true) == vgx::TF_RGB565, "565 found");
    vgx::PackRgba16(l565[0], vgx::TF_RGB565);
    Check(l565[0].size() == 6, "two bytes per texel");
    Check((l565[0][0] | l565[0][1] << 8) == (1 << 11 | 1 << 5 | 2), "565 word, little-endian, red on top");
    L l5551{{0x08, 0x08, 0x10, 0, 0xFF, 0x00, 0x00, 255}};
    Check(vgx::Rgba16Format(l5551, true) == vgx::TF_RGBA5551, "one-bit alpha is 5551");
    L noMip = l5551;
    Check(vgx::Rgba16Format(noMip, false) == 0, "no 5551 when it is not allowed");
    vgx::PackRgba16(l5551[0], vgx::TF_RGBA5551);
    Check((l5551[0][0] | l5551[0][1] << 8) == (1 << 11 | 1 << 6 | 2 << 1), "5551 word");
    Check((l5551[0][2] | l5551[0][3] << 8) == (31 << 11 | 1), "5551 word 2");
    L l4444{{0x11, 0x22, 0x33, 0x44}};
    Check(vgx::Rgba16Format(l4444, true) == vgx::TF_RGBA4444, "4444 found");
    vgx::PackRgba16(l4444[0], vgx::TF_RGBA4444);
    Check((l4444[0][0] | l4444[0][1] << 8) == 0x1234, "4444 word");
    L none{{0x11, 0x22, 0x33, 0x45}};
    Check(vgx::Rgba16Format(none, true) == 0, "a texel that does not fit keeps RGBA8");
    L part{{0x08, 0x08, 0x10, 0x80}};
    Check(vgx::Rgba16Format(part, true) == 0, "a translucent texel is not one-bit alpha");
    L second{{0x08, 0x04, 0x10, 255}, {0x09, 0x04, 0x10, 255}};
    Check(vgx::Rgba16Format(second, true) == 0, "every level has to fit");
    // Exhaustive: all 5-bit and 6-bit values are recognised and only they.
    unsigned n5 = 0, n6 = 0;
    for (unsigned v = 0; v < 256; ++v) {
        L a{{static_cast<uint8_t>(v), 0, 0, 255}}, b{{0, static_cast<uint8_t>(v), 0, 255}};
        n5 += vgx::Rgba16Format(a, false) == vgx::TF_RGB565;
        n6 += vgx::Rgba16Format(b, false) == vgx::TF_RGB565;
    }
    Check(n5 == 32 && n6 == 64, "every replica value and no other");
}

std::vector<uint8_t> g_src;
vgx::TexUpload Upload(unsigned fmt, unsigned w, unsigned h) {
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj = {};
    slot.obj.data = g_src.data(); slot.obj.width = w; slot.obj.height = h; slot.obj.format = fmt;
    slot.obj.minFilter = vgx::FILT_LINEAR;
    vgx::Pending() = {};
    vgx::TexBind bind; uint16_t rw = 0, rh = 0;
    Check(vgx::ResolveTexture(0, bind, rw, rh), "texture resolves");
    Check(vgx::Pending().uploads.size() == 1, "one upload");
    return vgx::Pending().uploads[0];
}

void TestCache(const char* expect) {
    // CMPR, 8 x 8, a three-colour block that uses its transparent entry.
    g_src.assign(vgx::TexLevelSize(8, 8, 0xE), 0);
    for (size_t off = 0; off < g_src.size(); off += 8) { g_src[off + 1] = 1; g_src[off + 2] = 0xF0; std::memset(&g_src[off + 4], 0xE4, 4); }
    const vgx::TexUpload c = Upload(0xE, 8, 8);
    // (the block's endpoints are far apart: its middle colour moves by about 40 steps as DXT5; CMPRA=2 with its default tolerance (12) keeps it decoded, MSC_VGX_CMPRA_TOL=60 lets it go)
    const bool cmpra = std::strcmp(expect, "cmpra") == 0 || std::strcmp(expect, "cmpra2wide") == 0;
    Check(c.format == (cmpra ? vgx::TF_DXT5 : vgx::TF_RGBA8), "CMPR with transparency: DXT5 with the switch (=2: within the tolerance), RGBA8 without");
    Check(c.levels[0].size() == (cmpra ? 64u : 256u), "CMPR payload size");
    // RGB565, 8 x 8.
    std::vector<uint8_t> s565(vgx::TexLevelSize(8, 8, 4));
    for (size_t i = 0; i < s565.size(); ++i) s565[i] = static_cast<uint8_t>(i * 13);
    g_src = s565;
    const vgx::TexUpload u = Upload(4, 8, 8);
    const bool tex16 = std::strcmp(expect, "tex16") == 0;   // ("tex16min": the switch on with the default MSC_VGX_TEX16_MIN, under which this 8 x 8 texture stays 32 bits)
    Check(u.format == (tex16 ? vgx::TF_RGB565 : vgx::TF_RGBA8), "RGB565: 16 bits with the switch, RGBA8 without");
    Check(u.levels[0].size() == (tex16 ? 128u : 256u), "RGB565 payload size");
    if (tex16)
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                // (4 x 4 tiles of big-endian words, row-major tiles)
                const uint8_t* be = &s565[((y / 4 * 2 + x / 4) * 16 + (y & 3) * 4 + (x & 3)) * 2];
                const uint8_t* le = &u.levels[0][(y * 8 + x) * 2];
                Check(le[0] == be[1] && le[1] == be[0], "the GX word, byte-swapped and untiled");
            }
}
} // namespace

int main(int argc, char** argv) {
    TestDxt5();
    Test16();
    TestCache(argc > 1 ? argv[1] : "default");
    std::puts("texture_compact: ok");
    return 0;
}
