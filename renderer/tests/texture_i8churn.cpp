#include "vgx_state.h"
#include "vgx_texdecode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vgx {
State g;
uint32_t g_gxd = 0x3FFFFFFFu;
static CmdBuf commands;
static uint64_t frame = 1;
static int64_t liveBytes = 0;
CmdBuf& Pending() { return commands; }
uint64_t CurrentFrame() { return frame; }
const char* ShaderListPath() { return ""; }
namespace stats {
Frame& Cur() { static Frame f; return f; }
void TexFormat(uint32_t, uint16_t, uint16_t, bool) {}
void TexLive(int32_t, int64_t bytes) { liveBytes += bytes; }
void Note(const char*) {}
}
}

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

// walk15 MSC_VGX_I8CHURN: an I8 texture rewritten picture after picture goes as one byte a texel from its 4th upload in a row; everything else keeps today's upload.
void Rewrite(std::vector<uint8_t>& src, unsigned seed) { for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 29 + seed * 131 + (seed >> 3)); }

vgx::TexUpload Bind(std::vector<uint8_t>& src, unsigned w, unsigned h, unsigned fmt, bool mip, bool& uploaded) {
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj = {};
    slot.obj.data = src.data(); slot.obj.width = w; slot.obj.height = h; slot.obj.format = fmt;
    slot.obj.mipmap = mip; slot.obj.maxLod = 3;
    slot.obj.minFilter = mip ? vgx::FILT_LIN_MIP_LIN : vgx::FILT_LINEAR;
    vgx::InvalidateAllTextures();
    vgx::Pending() = {};
    vgx::TexBind bind; uint16_t rw = 0, rh = 0;
    Check(vgx::ResolveTexture(0, bind, rw, rh), "texture resolves");
    uploaded = !vgx::Pending().uploads.empty();
    return uploaded ? vgx::Pending().uploads[0] : vgx::TexUpload{};
}

void SameAsRgba(const vgx::TexUpload& up, const std::vector<uint8_t>& src, unsigned w, unsigned h) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    vgx::DecodeTexture(src.data(), w, h, 1, nullptr, 0, 0, rgba.data());
    if (up.format == vgx::TF_I8) {
        Check(up.levels.size() == 1 && up.levels[0].size() == static_cast<size_t>(w) * h, "one byte a texel");
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            Check(up.levels[0][i] == rgba[i * 4] && rgba[i * 4] == rgba[i * 4 + 1] && rgba[i * 4] == rgba[i * 4 + 2] && rgba[i * 4] == rgba[i * 4 + 3], "the byte is I,I,I,I of the RGBA decode");
    } else {
        Check(up.format == vgx::TF_RGBA8 && up.levels[0] == rgba, "the RGBA8 upload is the reference decode");
    }
}
}

int main(int argc, char** argv) {
    const bool on = argc > 1 && std::strcmp(argv[1], "on") == 0;
    // the decoder: every I8 tile fixture, against the LA88 decoder's even bytes and the RGBA reference
    for (unsigned w : {8u, 16u, 640u}) for (unsigned h : {4u, 8u, 256u}) for (unsigned seed = 0; seed < 8; ++seed) {
        std::vector<uint8_t> src(static_cast<size_t>(w) * h), a(static_cast<size_t>(w) * h), la(static_cast<size_t>(w) * h * 2);
        Rewrite(src, seed);
        Check(vgx::DecodeTextureI8(src.data(), w, h, a.data()), "I8 decode");
        Check(vgx::DecodeTextureLA88(src.data(), w, h, 1, la.data()), "LA88 decode");
        for (size_t i = 0; i < a.size(); ++i) Check(a[i] == la[i * 2] && la[i * 2] == la[i * 2 + 1], "I8 decode equals the LA88 pair");
    }
    { uint8_t d[64], s[64] = {}; Check(!vgx::DecodeTextureI8(s, 7, 4, d) && !vgx::DecodeTextureI8(s, 8, 3, d) && !vgx::DecodeTextureI8(nullptr, 8, 4, d) && !vgx::DecodeTextureI8(s, 8, 4, nullptr) && !vgx::DecodeTextureI8(s, 0, 4, d), "bad shapes refused"); }

    // a churning texture
    const unsigned W = 16, H = 8;
    std::vector<uint8_t> src(vgx::TexDataSize(W, H, 1, 1));
    bool up = false;
    for (unsigned n = 1; n <= 8; ++n) {
        vgx::frame += 1;
        Rewrite(src, n);
        const auto u = Bind(src, W, H, 1, false, up);
        Check(up, "a rewritten texture uploads");
        const bool one = on && n >= 4;
        Check((u.format == vgx::TF_I8) == one, one ? "from the 4th upload in a row: one byte a texel" : "the first uploads (and every upload with the switch off) are RGBA8");
        SameAsRgba(u, src, W, H);
    }
    // a gap longer than the window starts the count again
    vgx::frame += 40;
    Rewrite(src, 99);
    Check(Bind(src, W, H, 1, false, up).format == vgx::TF_RGBA8 && up, "after a gap the next upload is RGBA8 again");
    // one-off I8 (a different texture, uploaded once) stays RGBA8
    std::vector<uint8_t> once(vgx::TexDataSize(W, H, 1, 1) + 64);
    Rewrite(once, 5);
    vgx::frame += 1;
    { const auto u = Bind(once, W, H, 1, false, up); Check(up && u.format == vgx::TF_RGBA8, "a texture uploaded once is RGBA8"); }
    // shapes and formats outside the path
    {
        std::vector<uint8_t> odd(vgx::TexDataSize(12, 8, 1, 1)); std::vector<uint8_t> i4(vgx::TexDataSize(W, H, 0, 1)); std::vector<uint8_t> mip(vgx::TexDataSize(W, H, 1, 4));
        for (unsigned n = 1; n <= 8; ++n) {
            vgx::frame += 1;
            Rewrite(odd, n); Rewrite(i4, n); Rewrite(mip, n);
            Check(Bind(odd, 12, 8, 1, false, up).format == vgx::TF_RGBA8, "width 12 (not whole tiles) stays RGBA8");
            Check(Bind(i4, W, H, 0, false, up).format == vgx::TF_RGBA8, "I4 stays RGBA8");
            Check(Bind(mip, W, H, 1, true, up).format == vgx::TF_RGBA8, "mipmapped I8 stays RGBA8");
        }
    }
    std::printf("PASS: I8 churn (%s): decoder equals LA88, the 4th upload in a row takes one byte, gaps/one-offs/odd shapes/I4/mips keep RGBA8\n", on ? "on" : "off");
}
