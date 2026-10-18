#include "vgx_state.h"
#include "vgx_texdecode.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vgx {
State g;
static CmdBuf commands;
static uint64_t frame = 1;
static int64_t liveBytes = 0;
CmdBuf& Pending() { return commands; }
uint64_t CurrentFrame() { return frame; }
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
std::vector<std::vector<uint8_t>> sources;
std::array<uint8_t, 512> palette{};

vgx::TexUpload MakeUpload(unsigned fmt, unsigned w, unsigned h, bool mip, unsigned pattern) {
    const unsigned levels = mip ? 4 : 1;
    sources.emplace_back(vgx::TexDataSize(w, h, fmt, levels));
    auto& src = sources.back();
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 37 + pattern);
    auto& slot = vgx::g.tex[0];
    slot.valid = true;
    slot.obj = {};
    slot.obj.data = src.data(); slot.obj.width = w; slot.obj.height = h; slot.obj.format = fmt;
    slot.obj.mipmap = mip; slot.obj.maxLod = 3;
    slot.obj.minFilter = mip ? vgx::FILT_LIN_MIP_LIN : vgx::FILT_LINEAR;
    vgx::Pending() = {};
    vgx::TexBind bind; uint16_t rw = 0, rh = 0;
    Check(vgx::ResolveTexture(0, bind, rw, rh), "texture resolves");
    Check(rw == w && rh == h, "resolved dimensions preserved");
    Check(vgx::Pending().uploads.size() == 1, "one upload per new texture");
    return vgx::Pending().uploads[0];
}

void VerifyPixels(const vgx::TexUpload& up, unsigned fmt, bool compact) {
    const size_t pixels = static_cast<size_t>(up.width) * up.height;
    Check(up.levels[0].size() == pixels * (compact ? 2 : 4), "compact intensity payload must use two bytes per pixel");
    std::vector<uint8_t> rgba(pixels * 4);
    vgx::DecodeTexture(sources.back().data(), up.width, up.height, fmt, palette.data(), 0, 256, rgba.data());
    for (size_t i = 0; i < pixels; ++i) {
        const auto* p = &up.levels[0][i * (compact ? 2 : 4)];
        if (compact) {
            Check(p[0] == rgba[i * 4] && p[0] == rgba[i * 4 + 1] && p[0] == rgba[i * 4 + 2], "LA luminance preserves all RGB channels");
            Check(p[1] == rgba[i * 4 + 3], "LA alpha preserves GX intensity/alpha");
        } else {
            Check(std::equal(p, p + 4, rgba.data() + i * 4), "fallback preserves RGBA pixels");
        }
    }
}

void CheckDirectDecoder() {
    std::array<uint8_t, 64> src{};
    std::array<uint8_t, 10> guarded;
    for (unsigned fmt = 0; fmt < 4; ++fmt) {
        const unsigned count = fmt == 3 ? 65536 : 256;
        for (unsigned v = 0; v < count; ++v) {
            src[0] = static_cast<uint8_t>(v);
            src[1] = static_cast<uint8_t>(v >> 8);
            guarded.fill(0xA5);
            Check(vgx::DecodeTextureLA88(src.data(), 1, 1, fmt, guarded.data() + 4), "supported compact decode succeeds");
            unsigned l = 0, a = 0;
            if (fmt == 0) l = a = (v >> 4) * 17;
            else if (fmt == 1) l = a = v;
            else if (fmt == 2) { l = (v & 15) * 17; a = (v >> 4) * 17; }
            else { l = v >> 8; a = v & 255; }
            Check(guarded[4] == l && guarded[5] == a, "independent exhaustive channel oracle");
            for (unsigned i = 0; i < guarded.size(); ++i)
                if (i != 4 && i != 5) Check(guarded[i] == 0xA5, "compact decode leaves destination guards intact");
        }
    }
    const unsigned invalid[] = {4, 5, 6, 8, 9, 10, 14, 255, 0xFFFFFFFFu};
    for (unsigned fmt : invalid) {
        guarded.fill(0xA5);
        Check(!vgx::DecodeTextureLA88(src.data(), 1, 1, fmt, guarded.data() + 4), "unsupported compact format rejected");
        Check(std::all_of(guarded.begin(), guarded.end(), [](uint8_t x) { return x == 0xA5; }), "invalid format leaves destination unchanged");
    }
    Check(!vgx::DecodeTextureLA88(src.data(), 0, 1, 1, guarded.data()), "zero width rejected");
    Check(!vgx::DecodeTextureLA88(src.data(), 1, 0, 1, guarded.data()), "zero height rejected");
    Check(!vgx::DecodeTextureLA88(nullptr, 1, 1, 1, guarded.data()), "null source rejected");
    Check(!vgx::DecodeTextureLA88(src.data(), 1, 1, 1, nullptr), "null destination rejected");
}
}

int main(int argc, char** argv) {
    CheckDirectDecoder();
    vgx::g.tlut[0].valid = true;
    vgx::g.tlut[0].obj.data = palette.data();
    vgx::g.tlut[0].obj.entries = 256;
    const bool compact = argc > 1 && std::strcmp(argv[1], "compact") == 0;
    const unsigned dims[][2] = {{1,1}, {3,5}, {7,3}, {8,4}, {9,5}, {16,8}, {31,17}};
    for (unsigned fmt = 0; fmt < 4; ++fmt)
        for (const auto& dim : dims)
            for (unsigned pattern = 0; pattern < 256; ++pattern)
                VerifyPixels(MakeUpload(fmt, dim[0], dim[1], false, pattern), fmt, compact);

    for (unsigned fmt = 0; fmt < 4; ++fmt) {
        auto up = MakeUpload(fmt, 8, 8, true, 17);
        Check(up.format == vgx::TF_RGBA8, "mipmapped intensity stays RGBA8");
        Check(up.genMips || up.levels.size() == 4, "mipmap route preserved");
        VerifyPixels(up, fmt, false);
    }
    auto npotMip = MakeUpload(1, 3, 5, true, 7);
    Check(npotMip.format == vgx::TF_RGBA8 && npotMip.levels.size() == 1, "mipmap-flagged NPOT texture stays RGBA8");
    VerifyPixels(npotMip, 1, false);
    MakeUpload(1, 8, 8, true, 9);
    vgx::g.tex[0].obj.minFilter = vgx::FILT_LINEAR;
    vgx::Pending() = {};
    vgx::TexBind noMipBind; uint16_t noMipW = 0, noMipH = 0;
    Check(vgx::ResolveTexture(0, noMipBind, noMipW, noMipH), "mipmap texture with non-mip filter resolves");
    Check(vgx::Pending().uploads.size() == 1 && vgx::Pending().uploads[0].format == vgx::TF_RGBA8, "mipmap flag excludes compact path even with non-mip filter");
    VerifyPixels(vgx::Pending().uploads[0], 1, false);
    for (unsigned fmt : {4u, 5u, 6u, 8u, 9u, 10u, 14u, 255u}) {
        auto up = MakeUpload(fmt, 7, 3, false, 43);
        Check(up.format == vgx::TF_RGBA8, "other and invalid formats stay on old route");
        VerifyPixels(up, fmt, false);
    }

    auto first = MakeUpload(1, 3, 5, false, 0);
    vgx::TexBind bind; uint16_t w = 0, h = 0;
    vgx::Pending() = {};
    Check(vgx::ResolveTexture(0, bind, w, h) && vgx::Pending().uploads.empty(), "unchanged cache hit must not upload");
    sources.back()[0] ^= 0xFF;
    vgx::InvalidateAllTextures();
    Check(vgx::ResolveTexture(0, bind, w, h), "changed texture resolves");
    Check(vgx::Pending().uploads.size() == 1, "changed texture reuploads");
    auto changed = vgx::Pending().uploads[0];
    Check(changed.texId == first.texId && changed.levels[0] != first.levels[0], "rewrite retains id but changes pixels");
    VerifyPixels(changed, 1, compact);

    const auto copied = vgx::AcquireCopyTexture(sources.back().data(), 3, 5, 1);
    vgx::Pending() = {};
    Check(vgx::ResolveTexture(0, bind, w, h) && bind.texId == copied, "EFB copy wins over guest bytes");
    Check(vgx::Pending().uploads.empty(), "EFB copy path must not decode LA88");
    vgx::DestroyCopyTexture(sources.back().data());
    vgx::frame += 31;
    vgx::SetTextureBudget(1);
    vgx::Pending() = {};
    vgx::TexCacheEndFrame();
    Check(vgx::liveBytes == 0, "eviction accounts for actual texture payloads");
    Check(vgx::ResolveTexture(0, bind, w, h), "evicted texture resolves again");
    Check(bind.texId != first.texId && vgx::Pending().uploads.size() == 1, "eviction reuploads under new id");
    VerifyPixels(vgx::Pending().uploads[0], 1, compact);
    Check(vgx::liveBytes == (compact ? 30 : 60), "live byte accounting matches LA/RGBA storage");
    std::printf("PASS: texture cache %s; 7168 tiled fixtures, exhaustive I/IA channels, guards, invalid inputs, mip/format fallbacks, rewrite, EFB priority, eviction\n", compact ? "LA88" : "RGBA8");
}
