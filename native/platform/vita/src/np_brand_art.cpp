// The player's own game art for our screens: see np_brand_art.h.
#include "np_brand_art.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace NpBrandArt {
namespace {

constexpr uint32_t kKeyArtHash = 0x57399903u;   // the start screen's 512 x 512 picture (the ball, the boot, the sunset)

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

uint32_t Pack(int r, int g, int b, int a = 255) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
}
int E5(int v) { return (v << 3) | (v >> 2); }
int E6(int v) { return (v << 2) | (v >> 4); }

// A BundleFile (.res / .dmn): u32 sector size, count, directory sector, pad; at directory * sector the entries {hash, block, length}.
struct Bundle {
    FILE* f = nullptr;
    uint32_t sector = 0, count = 0, directory = 0;
    ~Bundle() { if (f) std::fclose(f); }
    bool Open(const char* path) {
        f = std::fopen(path, "rb");
        uint8_t head[16];
        if (!f || std::fread(head, 1, 16, f) != 16) return false;
        sector = Be32(head);
        count = Be32(head + 4);
        directory = Be32(head + 8);
        return sector >= 16 && sector <= 0x10000 && count > 0 && count < 100000;
    }
    // The entry with that hash, or with the hash of the first entry for which `accept` is true when hash is 0 (not needed: callers loop).
    bool Entry(uint32_t index, uint32_t& hash, uint32_t& offset, uint32_t& length) {
        uint8_t e[12];
        if (index >= count || std::fseek(f, long(directory) * long(sector) + long(index) * 12, SEEK_SET) != 0 || std::fread(e, 1, 12, f) != 12) return false;
        hash = Be32(e);
        offset = Be32(e + 4) * sector;
        length = Be32(e + 8);
        return true;
    }
    bool Read(uint32_t offset, size_t length, std::vector<uint8_t>& out) {
        out.resize(length);
        return std::fseek(f, long(offset), SEEK_SET) == 0 && std::fread(out.data(), 1, length, f) == length;
    }
};

// GX CMPR: 8 x 8 tiles of four 4 x 4 blocks {two RGB565 colours, 2-bit indices}. `out` is w x h, 0xAABBGGRR.
void DecodeCmpr(const uint8_t* d, int w, int h, uint32_t* out) {
    size_t o = 0;
    for (int ty = 0; ty < h; ty += 8)
        for (int tx = 0; tx < w; tx += 8)
            for (int sy = 0; sy < 8; sy += 4)
                for (int sx = 0; sx < 8; sx += 4) {
                    const int c0 = int(Be16(d + o)), c1 = int(Be16(d + o + 2));
                    const uint32_t bits = Be32(d + o + 4);
                    o += 8;
                    int col[4][4];
                    const int r0 = E5(c0 >> 11), g0 = E6((c0 >> 5) & 63), b0 = E5(c0 & 31);
                    const int r1 = E5(c1 >> 11), g1 = E6((c1 >> 5) & 63), b1 = E5(c1 & 31);
                    const int a0[4] = {r0, g0, b0, 255}, a1[4] = {r1, g1, b1, 255};
                    for (int k = 0; k < 4; ++k) { col[0][k] = a0[k]; col[1][k] = a1[k]; }
                    for (int k = 0; k < 3; ++k) {
                        if (c0 > c1) { col[2][k] = (2 * a0[k] + a1[k]) / 3; col[3][k] = (a0[k] + 2 * a1[k]) / 3; }
                        else { col[2][k] = (a0[k] + a1[k]) / 2; col[3][k] = 0; }
                    }
                    col[2][3] = 255;
                    col[3][3] = c0 > c1 ? 255 : 0;
                    for (int i = 0; i < 16; ++i) {
                        const int idx = (bits >> (30 - 2 * i)) & 3;
                        const int x = tx + sx + (i & 3), y = ty + sy + (i >> 2);
                        if (x < w && y < h) out[y * w + x] = Pack(col[idx][0], col[idx][1], col[idx][2], col[idx][3]);
                    }
                }
}

struct Rgb { float r, g, b; };

Rgb Bilinear(const uint32_t* img, int w, int h, float u, float v) {
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    if (u > w - 1.001f) u = w - 1.001f;
    if (v > h - 1.001f) v = h - 1.001f;
    const int x = int(u), y = int(v);
    const float fx = u - x, fy = v - y;
    const uint32_t p00 = img[y * w + x], p10 = img[y * w + x + 1], p01 = img[(y + 1) * w + x], p11 = img[(y + 1) * w + x + 1];
    auto ch = [&](int shift) {
        const float a = float((p00 >> shift) & 255) * (1 - fx) + float((p10 >> shift) & 255) * fx;
        const float b = float((p01 >> shift) & 255) * (1 - fx) + float((p11 >> shift) & 255) * fx;
        return a * (1 - fy) + b * fy;
    };
    return {ch(0), ch(8), ch(16)};
}

int Clamp(float v) { return v < 0 ? 0 : (v > 255 ? 255 : int(v + 0.5f)); }

}  // namespace

bool Art::LoadPicture(const char* mainui) {
    Bundle b;
    if (!b.Open(mainui)) return false;
    uint32_t offset = 0, length = 0;
    bool found = false;
    for (uint32_t i = 0; i < b.count && !found; ++i) {
        uint32_t hash;
        if (!b.Entry(i, hash, offset, length)) return false;
        found = hash == kKeyArtHash;
    }
    if (!found || length < 0x20 + 512 * 512 / 2) return false;
    std::vector<uint8_t> tex;
    if (!b.Read(offset, 0x20 + 512 * 512 / 2, tex)) return false;
    // the header: u32 levels, u32 game format (2 = CMPR), ..., u16 width, u16 height at 0x0E
    if (Be32(tex.data() + 4) != 2 || Be16(tex.data() + 0x0E) != 512 || Be16(tex.data() + 0x10) != 512) return false;
    std::vector<uint32_t> art(512 * 512);
    DecodeCmpr(tex.data() + 0x20, 512, 512, art.data());
    tex = std::vector<uint8_t>();

    // The same layout as the package's pictures: the sky (the picture's left part, enlarged: it is a soft glow) fills the screen, the picture
    // itself at its own proportions (a square as tall as the screen) on the right, fading in; the whole dimmed so that text reads.
    std::vector<uint32_t> out(size_t(kWidth) * kHeight);
    const float skyX = 0, skyY = 40, skyW = 204, skyH = 307;   // the part of the picture used as the sky
    const float scale = std::fmax(kWidth / skyW, kHeight / skyH);
    const float sx0 = skyX + (skyW - kWidth / scale) / 2, sy0 = skyY + (skyH - kHeight / scale) / 2;
    const int frontX = kWidth - kHeight;
    const float dim = 0.52f;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            Rgb c = Bilinear(art.data(), 512, 512, sx0 + x / scale, sy0 + y / scale);
            c.r *= 0.55f; c.g *= 0.55f; c.b *= 0.55f;
            if (x >= frontX) {
                const float t = float(x - frontX) / kHeight;
                const float m = t >= 0.45f ? 1.0f : t / 0.45f;
                const Rgb f = Bilinear(art.data(), 512, 512, (x - frontX) * (511.0f / (kHeight - 1)), y * (511.0f / (kHeight - 1)));
                c.r += (f.r - c.r) * m; c.g += (f.g - c.g) * m; c.b += (f.b - c.b) * m;
            }
            // a shade on the left (where the labels are) and at the bottom (the help text)
            const float left = 1.0f - 0.35f * (1.0f - std::fmin(1.0f, x / (0.55f * kWidth)));
            const float down = y > 440 ? 1.0f - 0.35f * float(y - 440) / (kHeight - 440) : 1.0f;
            const float k = dim * left * down;
            out[size_t(y) * kWidth + x] = Pack(Clamp(c.r * k), Clamp(c.g * k), Clamp(c.b * k));
        }
    backdrop_.swap(out);
    return true;
}

bool Art::LoadFont(const char* fontRes) {
    Bundle b;
    if (!b.Open(fontRes)) return false;
    std::string description;
    uint32_t pageOffset = 0, pageLength = 0;
    bool havePage = false;
    for (uint32_t i = 0; i < b.count; ++i) {
        uint32_t hash, offset, length;
        if (!b.Entry(i, hash, offset, length) || length > 4 * 1024 * 1024) return false;
        uint8_t magic[3];
        if (std::fseek(b.f, long(offset), SEEK_SET) != 0 || std::fread(magic, 1, 3, b.f) != 3) return false;
        if (std::memcmp(magic, "NLG", 3) == 0) {
            std::vector<uint8_t> text;
            if (length > 65536 || !b.Read(offset, length, text)) return false;
            description.assign(reinterpret_cast<const char*>(text.data()), text.size());
        } else if (!havePage) {
            havePage = true;
            pageOffset = offset;
            pageLength = length;
        }
    }
    if (description.empty() || !havePage || pageLength < 0x20 + 512 * 512 + 512) return false;
    // the page: CI8 (game format 8), 512 x 512, one level; the RGB5A3 palette follows the texels
    std::vector<uint8_t> tex;
    if (!b.Read(pageOffset, 0x20 + 512 * 512 + 512, tex)) return false;
    if (Be32(tex.data() + 4) != 8 || Be16(tex.data() + 0x0E) != 512 || Be16(tex.data() + 0x10) != 512 || Be32(tex.data() + 0x14) != 256) return false;
    const uint8_t* pal = tex.data() + 0x20 + 512 * 512;
    uint8_t palA[256], palL[256];
    for (int i = 0; i < 256; ++i) {
        const int v = int(Be16(pal + 2 * i));
        if (v & 0x8000) { palA[i] = 255; palL[i] = uint8_t(E5((v >> 10) & 31)); }
        else { const int a = (v >> 12) & 7; palA[i] = uint8_t((a << 5) | (a << 2) | (a >> 1)); palL[i] = uint8_t(((v >> 8) & 15) * 17); }
    }
    std::vector<uint8_t> a(512 * 512), l(512 * 512);
    const uint8_t* d = tex.data() + 0x20;
    for (int ty = 0; ty < 512; ty += 4)
        for (int tx = 0; tx < 512; tx += 8)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 8; ++x) {
                    const uint8_t n = *d++;
                    a[(ty + y) * 512 + tx + x] = palA[n];
                    l[(ty + y) * 512 + tx + x] = palL[n];
                }

    // the description: "Glyph <c> Width <advance> <render width> <offset>" in the order the glyphs are packed on the page
    int px = 0, py = 0;
    size_t at = 0;
    while (at < description.size()) {
        size_t end = description.find('\n', at);
        if (end == std::string::npos) end = description.size();
        std::string line = description.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 9, "PageSize ") == 0) {
            pageSize_ = std::atoi(line.c_str() + 9);
        } else if (line.compare(0, 7, "Height ") == 0) {
            lineHeight_ = std::atoi(line.c_str() + 7);
            const char* p;
            if ((p = std::strstr(line.c_str(), "RenderHeight "))) renderHeight_ = std::atoi(p + 13);
            if ((p = std::strstr(line.c_str(), " Ascent "))) ascent_ = std::atoi(p + 8);
            if ((p = std::strstr(line.c_str(), "RenderAscent "))) renderAscent_ = std::atoi(p + 13);
        } else if (line.compare(0, 6, "Glyph ") == 0) {
            int adv = 0, width = 0, off = 0;
            char name[16] = {};
            if (std::sscanf(line.c_str() + 6, "%15s Width %d %d %d", name, &adv, &width, &off) != 4) continue;
            const int c = name[1] == 0 ? uint8_t(name[0]) : std::atoi(name);
            if (pageSize_ != 512 || width <= 0 || width > 128 || c <= 0 || c >= 128) continue;
            if (px + width > pageSize_) { px = 0; py += renderHeight_; }
            if (py + renderHeight_ > pageSize_) break;
            glyphs_[c] = {int16_t(px), int16_t(py), int16_t(width), int16_t(adv), int16_t(off), true};
            px += width;
        }
    }
    if (!glyphs_['I'].present || renderHeight_ <= 0 || renderHeight_ > 128) return false;
    glyphA_.swap(a);
    glyphL_.swap(l);
    return true;
}

bool Art::Load(const char* gameRoot) {
    Release();
    if (!gameRoot) return false;
    try {   // a screen in a running game has little memory to spare: running out is "no art", never a crash
        const std::string root = std::string(gameRoot) + "/files/Art/fe/";
        if (!LoadPicture((root + "mainui.dmn").c_str())) { Release(); return false; }
        if (!LoadFont((root + "fonts/engfontheading36.res").c_str())) ReleaseFont();   // the font is optional
        return true;
    } catch (...) {
        Release();
        return false;
    }
}

void Art::ReleaseFont() {
    std::vector<uint8_t>().swap(glyphA_);
    std::vector<uint8_t>().swap(glyphL_);
    std::memset(glyphs_, 0, sizeof(glyphs_));
}

void Art::Release() {
    std::vector<uint32_t>().swap(backdrop_);
    std::vector<uint8_t>().swap(glyphA_);
    std::vector<uint8_t>().swap(glyphL_);
    std::memset(glyphs_, 0, sizeof(glyphs_));
}

int Art::Heading(uint32_t* px, int x, int y, int height, const char* text, bool draw) const {
    if (glyphA_.empty()) return 0;
    const float s = float(height) / float(lineHeight_);
    const int dx = x;
    float pen = float(x);
    const float top = float(y) + float(ascent_ - renderAscent_) * s;   // the glyph picture starts a little above the line
    const float boxH = float(renderHeight_) * s;
    const int sh = height / 12 < 2 ? 2 : height / 12, sw = height / 18 < 1 ? 1 : height / 18;   // shadow offset
    for (const char* t = text; *t; ++t) {
        int c = uint8_t(*t);
        if (c >= 128) continue;
        if (!glyphs_[c].present && c >= 'a' && c <= 'z') c -= 32;
        const Glyph& g = glyphs_[c];
        if (!g.present) { pen += float(glyphs_['I'].advance) * s; continue; }
        if (draw) {
            const float gx = pen + float(g.offset) * s;
            const float gw = float(g.width) * s;
            for (int pass = 0; pass < 2; ++pass) {   // 0: the shadow, 1: the letter
                const int ox = pass == 0 ? sw : 0, oy = pass == 0 ? sh : 0;
                const int x0 = int(gx) + ox, x1 = int(gx + gw + 1) + ox, y0 = int(top) + oy, y1 = int(top + boxH + 1) + oy;
                for (int yy = y0; yy < y1; ++yy) {
                    if (yy < 0 || yy >= kHeight) continue;
                    for (int xx = x0; xx < x1; ++xx) {
                        if (xx < 0 || xx >= kWidth) continue;
                        // source position on the page (centre of the destination pixel), by the area it covers: bilinear on alpha and alpha * luminance
                        const float u = (float(xx - ox) + 0.5f - gx) / s - 0.5f, v = (float(yy - oy) + 0.5f - top) / s - 0.5f;
                        if (u < -1 || v < -1 || u > g.width || v > renderHeight_) continue;
                        float sa = 0, sl = 0;
                        const float fu = std::floor(u), fv = std::floor(v);
                        for (int j = 0; j < 2; ++j)
                            for (int i = 0; i < 2; ++i) {
                                const int sx = int(fu) + i, sy = int(fv) + j;
                                if (sx < 0 || sy < 0 || sx >= g.width || sy >= renderHeight_) continue;
                                const float w = (i ? u - fu : 1 - (u - fu)) * (j ? v - fv : 1 - (v - fv));
                                const size_t at = size_t(g.y + sy) * 512 + size_t(g.x + sx);
                                sa += w * glyphA_[at];
                                sl += w * glyphA_[at] * glyphL_[at];
                            }
                        if (sa < 1.0f) continue;
                        float alpha = sa / 255.0f, r, gr, b;
                        if (pass == 0) { r = gr = b = 8; alpha *= 0.55f; }
                        else {
                            const float lum = sl / sa;                                   // 0 fill .. 249 outline
                            const float ink = std::fmin(1.0f, lum * 1.25f / 255.0f);
                            const float t = std::fmin(1.0f, std::fmax(0.0f, (float(yy) - float(y)) / float(height)));   // flame, top to bottom
                            float fr, fg, fb;
                            if (t < 0.45f) { const float k = t / 0.45f; fr = 255; fg = 244 + (178 - 244) * k; fb = 120 + (38 - 120) * k; }
                            else { const float k = (t - 0.45f) / 0.55f; fr = 255 + (232 - 255) * k; fg = 178 + (84 - 178) * k; fb = 38 + (18 - 38) * k; }
                            r = fr + (28 - fr) * ink; gr = fg + (10 - fg) * ink; b = fb + (12 - fb) * ink;
                        }
                        uint32_t& dst = px[yy * kWidth + xx];
                        const float dr = float(dst & 255), dg = float((dst >> 8) & 255), db = float((dst >> 16) & 255);
                        dst = Pack(Clamp(dr + (r - dr) * alpha), Clamp(dg + (gr - dg) * alpha), Clamp(db + (b - db) * alpha));
                    }
                }
            }
        }
        pen += float(g.advance) * s;
    }
    return int(pen) - dx;
}

}  // namespace NpBrandArt
