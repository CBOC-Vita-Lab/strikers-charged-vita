// On-device LiveArea & Home-screen bubble generator (C++17 port of tools/brand-from-disc.py art33).
// Reads only the player's own extracted disc files under ux0:data/mscharged/game/files/, composes
// icon0.png (128x128), bg.png (840x500), startup.png (280x158), and pic0.png (960x544) as SceShell-
// compatible 8-bit palette-indexed PNGs (median-cut 256 colors + Floyd-Steinberg dithering), writes
// them atomically to ur0:appmeta/<TITLE_ID>/ and ux0:app/<TITLE_ID>/sce_sys/, and asks ScePromoterUtil
// to refresh LiveArea.
#include "vita_livearea_art.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__vita__)
#include "np_vita_internal.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
extern "C" const char* NpGameRootPath(void);
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)(x))
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace VitaLiveAreaArt {

namespace {

// -------------------------------------------------------------------------------------------------
// Disc asset hashes & constants (matching tools/brand-from-disc.py)
// -------------------------------------------------------------------------------------------------
constexpr uint32_t kKeyArtHash = 0x57399903u;
constexpr uint32_t kBallArtHash = 0x944d752du;
constexpr uint32_t kSunburstHash = 0x250bbfa5u;
constexpr uint32_t kIceburstHash = 0x23b38f66u;
constexpr uint32_t kBoltAHash = 0x9cf58c6eu;
constexpr uint32_t kBoltBHash = 0x292d15fbu;
constexpr uint32_t kBoltCHash = 0x14083241u;
constexpr uint32_t kSparksHash = 0x2fd2a2e6u;
constexpr uint32_t kMarioHash = 0x886f2df4u;
constexpr uint32_t kBowserHash = 0x6c217d2eu;

constexpr const char* kLogoMember = "arc/timg/logo_UK_Banner.tpl";
constexpr const char* kSkyMember = "arc/timg/sky_banner.tpl";

constexpr double kPi = 3.14159265358979323846;

// Shipped generic CBOC-Vita icons (store/artwork.json): auto-branding ONLY replaces these, never custom art.
constexpr size_t kGenericIconLen = 2489;
constexpr uint32_t kGenericIconCrc = 0xFE35C2BCu;
constexpr size_t kGenericTestIconLen = 2434;
constexpr uint32_t kGenericTestIconCrc = 0xE9219312u;

inline uint32_t Be32(const uint8_t* b) { return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]; }
inline uint16_t Be16(const uint8_t* b) { return uint16_t((uint16_t(b[0]) << 8) | b[1]); }
inline uint8_t ClampU8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline uint8_t ClampF(double v) { return ClampU8(static_cast<int>(std::floor(v + 0.5))); }

inline bool Cancelled(const volatile bool* c) { return c && *c; }

void SetWhy(char* why, size_t whyBytes, const char* msg) {
    if (why && whyBytes && !why[0]) std::snprintf(why, whyBytes, "%s", msg);
}

// -------------------------------------------------------------------------------------------------
// Case-insensitive disc file lookup & whole-file read
// -------------------------------------------------------------------------------------------------
bool CaseEq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

bool FindFile(const char* root, const char* rel, std::string& outPath) {
    std::string cur(root ? root : "");
    while (!cur.empty() && cur.back() == '/') cur.pop_back();
    const char* p = rel;
    while (p && *p) {
        const char* slash = std::strchr(p, '/');
        std::string part = slash ? std::string(p, static_cast<size_t>(slash - p)) : std::string(p);
        p = slash ? slash + 1 : nullptr;
        if (part.empty()) continue;

        std::string direct = cur + "/" + part;
        bool exists = false;
#if defined(__vita__)
        SceIoStat st{};
        if (sceIoGetstat(direct.c_str(), &st) >= 0) exists = true;
#else
        struct stat st{};
        if (::stat(direct.c_str(), &st) == 0) exists = true;
#endif
        if (exists) {
            cur = direct;
            continue;
        }
        std::string found;
#if defined(__vita__)
        SceUID d = sceIoDopen(cur.c_str());
        if (d < 0) return false;
        SceIoDirent ent{};
        while (sceIoDread(d, &ent) > 0) {
            if (CaseEq(ent.d_name, part.c_str())) {
                found = cur + "/" + ent.d_name;
                break;
            }
            std::memset(&ent, 0, sizeof ent);
        }
        sceIoDclose(d);
#else
        DIR* d = ::opendir(cur.c_str());
        if (!d) return false;
        while (struct dirent* ent = ::readdir(d)) {
            if (CaseEq(ent->d_name, part.c_str())) {
                found = cur + "/" + ent->d_name;
                break;
            }
        }
        ::closedir(d);
#endif
        if (found.empty()) return false;
        cur = found;
    }
    outPath = cur;
    return !outPath.empty();
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out, size_t maxBytes = 16u * 1024u * 1024u) {
    out.clear();
#if defined(__vita__)
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (sz <= 0 || static_cast<uint64_t>(sz) > maxBytes) { sceIoClose(fd); return false; }
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    size_t got = 0;
    while (got < out.size()) {
        size_t chunk = std::min<size_t>(262144, out.size() - got);
        int n = sceIoRead(fd, out.data() + got, static_cast<SceSize>(chunk));
        if (n <= 0) { sceIoClose(fd); out.clear(); return false; }
        got += static_cast<size_t>(n);
    }
    sceIoClose(fd);
    return true;
#else
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    long sz = std::ftell(f);
    if (sz <= 0 || static_cast<size_t>(sz) > maxBytes) { std::fclose(f); return false; }
    std::rewind(f);
    out.resize(static_cast<size_t>(sz));
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
#endif
}

// -------------------------------------------------------------------------------------------------
// Simple RGBA / RGB image container
// -------------------------------------------------------------------------------------------------
struct Img {
    int w = 0, h = 0, ch = 0;   // ch = 3 (RGB) or 4 (RGBA)
    std::vector<uint8_t> d;
    Img() = default;
    Img(int w_, int h_, int ch_, uint8_t fill = 0) : w(w_), h(h_), ch(ch_), d(static_cast<size_t>(w_) * h_ * ch_, fill) {}
    bool empty() const { return w <= 0 || h <= 0 || ch <= 0 || d.empty(); }
    uint8_t* px(int x, int y) { return d.data() + (static_cast<size_t>(y) * w + x) * ch; }
    const uint8_t* px(int x, int y) const { return d.data() + (static_cast<size_t>(y) * w + x) * ch; }
};

Img ToRgb(const Img& src) {
    if (src.ch == 3) return src;
    Img out(src.w, src.h, 3);
    for (size_t i = 0, n = static_cast<size_t>(src.w) * src.h; i < n; ++i) {
        out.d[3 * i + 0] = src.d[4 * i + 0];
        out.d[3 * i + 1] = src.d[4 * i + 1];
        out.d[3 * i + 2] = src.d[4 * i + 2];
    }
    return out;
}

Img Crop(const Img& src, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, std::min(x0, src.w));
    y0 = std::max(0, std::min(y0, src.h));
    x1 = std::max(x0, std::min(x1, src.w));
    y1 = std::max(y0, std::min(y1, src.h));
    int nw = x1 - x0, nh = y1 - y0;
    if (nw <= 0 || nh <= 0) return {};
    Img out(nw, nh, src.ch);
    for (int y = 0; y < nh; ++y)
        std::memcpy(out.px(0, y), src.px(x0, y0 + y), static_cast<size_t>(nw) * src.ch);
    return out;
}

// -------------------------------------------------------------------------------------------------
// GX texture decoding, BundleFile, Wii opening.bnr (U8 + IMD5 + LZ77 + TPL), GameFont
// -------------------------------------------------------------------------------------------------
struct Px { uint8_t r, g, b, a; };

inline int ExpandBits(int v, int bits) {
    if (bits == 3) return (v << 5) | (v << 2) | (v >> 1);
    if (bits == 4) return v * 17;
    if (bits == 5) return (v << 3) | (v >> 2);
    return (v << 2) | (v >> 4);
}
inline Px Rgb565(uint16_t v) { return {uint8_t(ExpandBits((v >> 11) & 31, 5)), uint8_t(ExpandBits((v >> 5) & 63, 6)), uint8_t(ExpandBits(v & 31, 5)), 255}; }
inline Px Rgb5a3(uint16_t v) {
    if (v & 0x8000u) return {uint8_t(ExpandBits((v >> 10) & 31, 5)), uint8_t(ExpandBits((v >> 5) & 31, 5)), uint8_t(ExpandBits(v & 31, 5)), 255};
    return {uint8_t(ExpandBits((v >> 8) & 15, 4)), uint8_t(ExpandBits((v >> 4) & 15, 4)), uint8_t(ExpandBits(v & 15, 4)), uint8_t(ExpandBits((v >> 12) & 7, 3))};
}

bool GxBlock(int fmt, int& bw, int& bh, int& bpp) {
    switch (fmt) {
        case 0: case 8: case 14: bw = 8; bh = 8; bpp = 4; return true;
        case 1: case 2: case 9: bw = 8; bh = 4; bpp = 8; return true;
        case 3: case 4: case 5: case 10: bw = 4; bh = 4; bpp = 16; return true;
        case 6: bw = 4; bh = 4; bpp = 32; return true;
        default: return false;
    }
}

uint64_t GxLevelSize(int w, int h, int fmt) {
    int bw = 0, bh = 0, bpp = 0;
    if (!GxBlock(fmt, bw, bh, bpp)) return 0;
    return uint64_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bw * bh * bpp / 8;
}

Img DecodeGx(int fmt, int w, int h, const uint8_t* data, size_t dataLen, const std::vector<Px>* pal) {
    int bw = 0, bh = 0, bpp = 0;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || !GxBlock(fmt, bw, bh, bpp)) return {};
    if (GxLevelSize(w, h, fmt) > dataLen) return {};
    int pw = ((w + bw - 1) / bw) * bw;
    int ph = ((h + bh - 1) / bh) * bh;
    Img out(w, h, 4, 0);
    auto put = [&](int x, int y, Px c) {
        if (x < w && y < h) {
            uint8_t* p = out.px(x, y);
            p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
        }
    };
    auto palAt = [&](size_t idx) -> Px {
        if (pal && idx < pal->size()) return (*pal)[idx];
        return {255, 0, 255, 255};
    };
    size_t o = 0;
    for (int ty = 0; ty < ph; ty += bh) {
        for (int tx = 0; tx < pw; tx += bw) {
            if (fmt == 14) {
                for (int sy = 0; sy < 8; sy += 4) {
                    for (int sx = 0; sx < 8; sx += 4) {
                        uint16_t c0 = Be16(data + o), c1 = Be16(data + o + 2);
                        uint32_t bits = Be32(data + o + 4);
                        o += 8;
                        Px cs[4];
                        cs[0] = Rgb565(c0);
                        cs[1] = Rgb565(c1);
                        if (c0 > c1) {
                            cs[2] = {uint8_t((2 * cs[0].r + cs[1].r) / 3), uint8_t((2 * cs[0].g + cs[1].g) / 3), uint8_t((2 * cs[0].b + cs[1].b) / 3), 255};
                            cs[3] = {uint8_t((cs[0].r + 2 * cs[1].r) / 3), uint8_t((cs[0].g + 2 * cs[1].g) / 3), uint8_t((cs[0].b + 2 * cs[1].b) / 3), 255};
                        } else {
                            cs[2] = {uint8_t((cs[0].r + cs[1].r) / 2), uint8_t((cs[0].g + cs[1].g) / 2), uint8_t((cs[0].b + cs[1].b) / 2), 255};
                            cs[3] = {0, 0, 0, 0};
                        }
                        for (int i = 0; i < 16; ++i) put(tx + sx + (i & 3), ty + sy + (i >> 2), cs[(bits >> (30 - 2 * i)) & 3u]);
                    }
                }
            } else if (fmt == 6) {
                for (int i = 0; i < 16; ++i)
                    put(tx + (i & 3), ty + (i >> 2), {data[o + 2 * i + 1], data[o + 32 + 2 * i], data[o + 33 + 2 * i], data[o + 2 * i]});
                o += 64;
            } else if (fmt == 4 || fmt == 5) {
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        uint16_t v = Be16(data + o); o += 2;
                        put(tx + x, ty + y, fmt == 4 ? Rgb565(v) : Rgb5a3(v));
                    }
            } else if (fmt == 3) {
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        uint8_t a = data[o], lum = data[o + 1]; o += 2;
                        put(tx + x, ty + y, {lum, lum, lum, a});
                    }
            } else if (fmt == 0 || fmt == 8) {
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; x += 2) {
                        uint8_t v = data[o++];
                        int nib[2] = {v >> 4, v & 15};
                        for (int k = 0; k < 2; ++k) {
                            Px c = (fmt == 0) ? Px{uint8_t(nib[k] * 17), uint8_t(nib[k] * 17), uint8_t(nib[k] * 17), uint8_t(nib[k] * 17)} : palAt(static_cast<size_t>(nib[k]));
                            put(tx + x + k, ty + y, c);
                        }
                    }
            } else if (fmt == 1 || fmt == 2 || fmt == 9) {
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 8; ++x) {
                        uint8_t v = data[o++];
                        if (fmt == 1) put(tx + x, ty + y, {v, v, v, v});
                        else if (fmt == 2) { uint8_t lum = uint8_t((v & 15) * 17), a = uint8_t((v >> 4) * 17); put(tx + x, ty + y, {lum, lum, lum, a}); }
                        else put(tx + x, ty + y, palAt(v));
                    }
            }
        }
    }
    return out;
}

Img DecodeTpl(const uint8_t* blob, size_t len) {
    if (len < 16 || Be32(blob) != 0x0020AF30u || Be32(blob + 4) < 1) return {};
    size_t table = Be32(blob + 8);
    if (table + 8 > len) return {};
    size_t imgAt = Be32(blob + table), palAt = Be32(blob + table + 4);
    if (imgAt + 12 > len) return {};
    int h = Be16(blob + imgAt), w = Be16(blob + imgAt + 2);
    int fmt = static_cast<int>(Be32(blob + imgAt + 4));
    size_t dataAt = Be32(blob + imgAt + 8);
    if (dataAt > len) return {};
    std::vector<Px> pal;
    if (palAt) {
        if (palAt + 12 > len) return {};
        int entries = Be16(blob + palAt);
        int palFmt = static_cast<int>(Be32(blob + palAt + 4));
        size_t palData = Be32(blob + palAt + 8);
        if (palData + static_cast<size_t>(entries) * 2 > len) return {};
        pal.resize(static_cast<size_t>(entries));
        for (int k = 0; k < entries; ++k) {
            uint16_t v = Be16(blob + palData + 2 * k);
            pal[k] = (palFmt == 1) ? Rgb565(v) : Rgb5a3(v);
        }
    }
    return DecodeGx(fmt, w, h, blob + dataAt, len - dataAt, pal.empty() ? nullptr : &pal);
}

Img LoadBundleTexture(const std::vector<uint8_t>& blob, uint32_t wanted) {
    if (blob.size() < 16) return {};
    uint32_t sector = Be32(blob.data()), count = Be32(blob.data() + 4), dir = Be32(blob.data() + 8);
    if (sector < 16 || sector > 0x10000 || count == 0 || count > 100000) return {};
    uint64_t dirOff = uint64_t(dir) * sector;
    if (dirOff + uint64_t(count) * 12 > blob.size()) return {};
    static const int kGameToGx[9] = {4, 5, 14, 6, 1, 0, 1, 3, 9};
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = blob.data() + dirOff + i * 12;
        uint32_t h = Be32(e), block = Be32(e + 4), length = Be32(e + 8);
        if (h != wanted) continue;
        uint64_t off = uint64_t(block) * sector;
        if (off + 0x20 > blob.size() || off + length > blob.size()) return {};
        const uint8_t* hdr = blob.data() + off;
        uint32_t levels = Be32(hdr), gameFmt = Be32(hdr + 4);
        int w = Be16(hdr + 0x0E), ht = Be16(hdr + 0x10);
        uint32_t entries = Be32(hdr + 0x14);
        if (gameFmt >= 9) return {};
        int fmt = kGameToGx[gameFmt];
        std::vector<Px> pal;
        if (gameFmt == 8) {
            uint64_t total = 0;
            int lw = w, lh = ht;
            for (uint32_t lv = 0; lv < levels && lv < 16; ++lv) {
                total += uint64_t(lw) * lh;
                lw = std::max(lw / 2, 1);
                lh = std::max(lh / 2, 1);
            }
            uint64_t po = off + 0x20 + total;
            uint32_t n = std::min<uint32_t>(entries, 256);
            if (po + uint64_t(n) * 2 > blob.size()) return {};
            pal.resize(n);
            for (uint32_t k = 0; k < n; ++k) pal[k] = Rgb5a3(Be16(blob.data() + po + 2 * k));
        }
        return DecodeGx(fmt, w, ht, blob.data() + off + 0x20, blob.size() - (off + 0x20), pal.empty() ? nullptr : &pal);
    }
    return {};
}

bool U8FindMember(const uint8_t* b, size_t n, size_t base, const char* want, size_t& outOff, size_t& outSize) {
    if (base + 12 > n || std::memcmp(b + base, "U\xaa" "8-", 4) != 0) return false;
    size_t root = Be32(b + base + 4);
    if (base + root + 12 > n) return false;
    size_t count = Be32(b + base + root + 8);
    if (count < 2 || count > 100000 || base + root + count * 12 > n) return false;
    size_t names = base + root + count * 12;
    std::vector<std::string> dirStack;
    std::vector<size_t> endStack = {count};
    bool found = false;
    for (size_t i = 1; i < count; ++i) {
        const uint8_t* e = b + base + root + i * 12;
        uint32_t word = Be32(e), noff = Be32(e + 4), nsize = Be32(e + 8);
        size_t at = names + (word & 0xFFFFFFu);
        if (at >= n) return false;
        const void* nul = std::memchr(b + at, 0, n - at);
        if (!nul) return false;
        std::string name(reinterpret_cast<const char*>(b + at), static_cast<const uint8_t*>(nul) - (b + at));
        while (!endStack.empty() && i >= endStack.back()) {
            endStack.pop_back();
            if (!dirStack.empty()) dirStack.pop_back();
        }
        if ((word >> 24) == 1) {
            dirStack.push_back(name);
            endStack.push_back(nsize);
        } else {
            std::string full;
            for (const std::string& d : dirStack) { full += d; full += '/'; }
            full += name;
            if (full == want && uint64_t(base) + noff + nsize <= n) {
                outOff = base + noff;
                outSize = nsize;
                found = true;
            }
        }
    }
    return found;
}

bool Lz77Decompress(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
    if (n >= 4 && std::memcmp(d, "LZ77", 4) == 0) { d += 4; n -= 4; }
    if (n < 4 || d[0] != 0x10) return false;
    size_t size = size_t(d[1]) | (size_t(d[2]) << 8) | (size_t(d[3]) << 16);
    if (size == 0 || size > 8u * 1024u * 1024u) return false;
    out.resize(size);
    size_t len = 0, p = 4;
    while (len < size) {
        if (p >= n) return false;
        uint8_t flags = d[p++];
        for (int k = 0; k < 8 && len < size; ++k) {
            if (flags & (0x80u >> k)) {
                if (p + 2 > n) return false;
                uint8_t a = d[p], b = d[p + 1];
                p += 2;
                size_t l = (a >> 4) + 3;
                size_t disp = (((a & 15u) << 8) | b) + 1;
                if (disp > len) return false;
                for (size_t i = 0; i < l && len < size; ++i, ++len) out[len] = out[len - disp];
            } else {
                if (p >= n) return false;
                out[len++] = d[p++];
            }
        }
    }
    return true;
}

Img TrimAlpha(const Img& im, uint8_t thresh = 12) {
    if (im.empty() || im.ch != 4) return im;
    int x0 = im.w, y0 = im.h, x1 = 0, y1 = 0;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            if (im.px(x, y)[3] > thresh) {
                x0 = std::min(x0, x); y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
            }
    if (x0 >= x1 || y0 >= y1) return im;
    return Crop(im, x0, y0, x1, y1);
}

bool LoadBannerAssets(const std::vector<uint8_t>& bnr, Img& logoOut, Img& skyOut) {
    size_t at = std::string::npos;
    for (size_t i = 0; i + 4 <= bnr.size(); ++i)
        if (std::memcmp(bnr.data() + i, "U\xaa" "8-", 4) == 0) { at = i; break; }
    if (at == std::string::npos) return false;
    size_t off = 0, sz = 0;
    if (!U8FindMember(bnr.data(), bnr.size(), at, "meta/banner.bin", off, sz)) return false;
    const uint8_t* inner = bnr.data() + off;
    size_t innerSz = sz;
    if (innerSz >= 0x20 && std::memcmp(inner, "IMD5", 4) == 0) { inner += 0x20; innerSz -= 0x20; }
    std::vector<uint8_t> unpacked;
    if (innerSz >= 4 && std::memcmp(inner, "LZ77", 4) == 0) {
        if (!Lz77Decompress(inner, innerSz, unpacked)) return false;
        inner = unpacked.data();
        innerSz = unpacked.size();
    }
    size_t at2 = std::string::npos;
    for (size_t i = 0; i + 4 <= innerSz; ++i)
        if (std::memcmp(inner + i, "U\xaa" "8-", 4) == 0) { at2 = i; break; }
    if (at2 == std::string::npos) return false;

    if (U8FindMember(inner, innerSz, at2, kLogoMember, off, sz)) {
        logoOut = TrimAlpha(DecodeTpl(inner + off, sz), 12);
    }
    if (U8FindMember(inner, innerSz, at2, kSkyMember, off, sz)) {
        Img s = DecodeTpl(inner + off, sz);
        if (!s.empty()) skyOut = ToRgb(s);
    }
    return !logoOut.empty();
}

// -------------------------------------------------------------------------------------------------
// Image processing primitives (Lanczos-3, Gaussian blur, gradients, boosts, rotation, SDF masks)
// -------------------------------------------------------------------------------------------------
// Fast separable float32 triangle/bilinear resampler (zero sin() calls; ~5x faster than Lanczos-3 on Cortex-A9).
Img ResizeFast(const Img& src, int nw, int nh) {
    if (src.empty() || nw <= 0 || nh <= 0) return {};
    if (src.w == nw && src.h == nh) return src;
    const int ch = src.ch;
    const int sw = src.w, sh = src.h;

    // Fast path for upscaling / near-1:1: direct bilinear without intermediate float allocation
    if (nw >= sw && nh >= sh) {
        Img out(nw, nh, ch);
        std::vector<int> x0s(nw), x1s(nw);
        std::vector<uint16_t> xfs(nw);
        const float sxScale = float(sw) / float(nw);
        for (int x = 0; x < nw; ++x) {
            float sx = (x + 0.5f) * sxScale - 0.5f;
            if (sx < 0.0f) sx = 0.0f;
            if (sx > sw - 1.0f) sx = float(sw - 1);
            int ix = static_cast<int>(sx);
            x0s[x] = ix;
            x1s[x] = std::min(ix + 1, sw - 1);
            xfs[x] = static_cast<uint16_t>((sx - ix) * 256.0f + 0.5f);
        }
        const float syScale = float(sh) / float(nh);
        for (int y = 0; y < nh; ++y) {
            float sy = (y + 0.5f) * syScale - 0.5f;
            if (sy < 0.0f) sy = 0.0f;
            if (sy > sh - 1.0f) sy = float(sh - 1);
            int y0 = static_cast<int>(sy);
            int y1 = std::min(y0 + 1, sh - 1);
            uint32_t wy = static_cast<uint32_t>((sy - y0) * 256.0f + 0.5f);
            uint32_t iwy = 256u - wy;
            const uint8_t* r0 = src.px(0, y0);
            const uint8_t* r1 = src.px(0, y1);
            uint8_t* dp = out.px(0, y);
            if (ch == 3) {
                for (int x = 0; x < nw; ++x) {
                    uint32_t wx = xfs[x], iwx = 256u - wx;
                    const uint8_t* p00 = r0 + x0s[x] * 3;
                    const uint8_t* p10 = r0 + x1s[x] * 3;
                    const uint8_t* p01 = r1 + x0s[x] * 3;
                    const uint8_t* p11 = r1 + x1s[x] * 3;
                    for (int c = 0; c < 3; ++c) {
                        uint32_t top = p00[c] * iwx + p10[c] * wx;
                        uint32_t bot = p01[c] * iwx + p11[c] * wx;
                        dp[3 * x + c] = static_cast<uint8_t>((top * iwy + bot * wy + 32768u) >> 16);
                    }
                }
            } else {
                for (int x = 0; x < nw; ++x) {
                    uint32_t wx = xfs[x], iwx = 256u - wx;
                    const uint8_t* p00 = r0 + x0s[x] * 4;
                    const uint8_t* p10 = r0 + x1s[x] * 4;
                    const uint8_t* p01 = r1 + x0s[x] * 4;
                    const uint8_t* p11 = r1 + x1s[x] * 4;
                    uint32_t w00 = iwx * iwy, w10 = wx * iwy, w01 = iwx * wy, w11 = wx * wy;
                    uint32_t aSum = p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11;
                    uint8_t au = static_cast<uint8_t>((aSum + 32768u) >> 16);
                    if (au == 0) {
                        dp[4 * x + 0] = dp[4 * x + 1] = dp[4 * x + 2] = dp[4 * x + 3] = 0;
                    } else {
                        uint32_t pw00 = (w00 * p00[3]) >> 8, pw10 = (w10 * p10[3]) >> 8;
                        uint32_t pw01 = (w01 * p01[3]) >> 8, pw11 = (w11 * p11[3]) >> 8;
                        uint32_t pSum = pw00 + pw10 + pw01 + pw11;
                        if (pSum == 0) {
                            dp[4 * x + 0] = dp[4 * x + 1] = dp[4 * x + 2] = 0;
                        } else {
                            for (int c = 0; c < 3; ++c) {
                                uint32_t v = p00[c] * pw00 + p10[c] * pw10 + p01[c] * pw01 + p11[c] * pw11;
                                dp[4 * x + c] = static_cast<uint8_t>((v + pSum / 2u) / pSum);
                            }
                        }
                        dp[4 * x + 3] = au;
                    }
                }
            }
        }
        return out;
    }

    // Separable triangle filter (support = max(1, scale), no sin(), float32)
    std::vector<float> tmp(static_cast<size_t>(nw) * sh * ch);
    const float xscale = float(sw) / float(nw);
    const float xsup = std::max(1.0f, xscale);
    const float xinv = 1.0f / xsup;
    struct Tap { int idx; float w; };
    std::vector<Tap> taps;
    taps.reserve(32);

    // Precompute horizontal taps per output x, then sweep rows sequentially (cache-friendly!)
    struct Span { int start, count; };
    std::vector<Span> hSpans(nw);
    std::vector<Tap> hTaps;
    hTaps.reserve(static_cast<size_t>(nw) * 6);
    for (int x = 0; x < nw; ++x) {
        float center = (x + 0.5f) * xscale;
        int xmin = std::max(0, static_cast<int>(std::floor(center - xsup)));
        int xmax = std::min(sw, static_cast<int>(std::ceil(center + xsup)));
        int start = static_cast<int>(hTaps.size());
        float wsum = 0.0f;
        for (int i = xmin; i < xmax; ++i) {
            float w = 1.0f - std::fabs((i + 0.5f - center) * xinv);
            if (w > 0.0f) { hTaps.push_back({i, w}); wsum += w; }
        }
        int cnt = static_cast<int>(hTaps.size()) - start;
        float inv = wsum > 0.0f ? (1.0f / wsum) : 1.0f;
        for (int k = 0; k < cnt; ++k) hTaps[start + k].w *= inv;
        hSpans[x] = {start, cnt};
    }

    for (int y = 0; y < sh; ++y) {
        const uint8_t* srcRow = src.px(0, y);
        float* dstRow = tmp.data() + static_cast<size_t>(y) * nw * ch;
        if (ch == 3) {
            for (int x = 0; x < nw; ++x) {
                float r = 0, g = 0, b = 0;
                const Span sp = hSpans[x];
                for (int k = 0; k < sp.count; ++k) {
                    const Tap& t = hTaps[sp.start + k];
                    const uint8_t* p = srcRow + t.idx * 3;
                    r += p[0] * t.w; g += p[1] * t.w; b += p[2] * t.w;
                }
                dstRow[3 * x + 0] = r; dstRow[3 * x + 1] = g; dstRow[3 * x + 2] = b;
            }
        } else {
            for (int x = 0; x < nw; ++x) {
                float r = 0, g = 0, b = 0, a = 0;
                const Span sp = hSpans[x];
                for (int k = 0; k < sp.count; ++k) {
                    const Tap& t = hTaps[sp.start + k];
                    const uint8_t* p = srcRow + t.idx * 4;
                    float aw = (p[3] * (1.0f / 255.0f)) * t.w;
                    r += p[0] * aw; g += p[1] * aw; b += p[2] * aw; a += p[3] * t.w;
                }
                dstRow[4 * x + 0] = r; dstRow[4 * x + 1] = g; dstRow[4 * x + 2] = b; dstRow[4 * x + 3] = a;
            }
        }
    }

    Img out(nw, nh, ch);
    const float yscale = float(sh) / float(nh);
    const float ysup = std::max(1.0f, yscale);
    const float yinv = 1.0f / ysup;
    for (int y = 0; y < nh; ++y) {
        float center = (y + 0.5f) * yscale;
        int ymin = std::max(0, static_cast<int>(std::floor(center - ysup)));
        int ymax = std::min(sh, static_cast<int>(std::ceil(center + ysup)));
        taps.clear();
        float wsum = 0.0f;
        for (int i = ymin; i < ymax; ++i) {
            float w = 1.0f - std::fabs((i + 0.5f - center) * yinv);
            if (w > 0.0f) { taps.push_back({i, w}); wsum += w; }
        }
        float inv = wsum > 0.0f ? (1.0f / wsum) : 1.0f;
        for (auto& t : taps) t.w *= inv;
        uint8_t* dstRow = out.px(0, y);
        if (ch == 3) {
            for (int x = 0; x < nw; ++x) {
                float r = 0, g = 0, b = 0;
                for (const auto& t : taps) {
                    const float* sp = tmp.data() + (static_cast<size_t>(t.idx) * nw + x) * 3;
                    r += sp[0] * t.w; g += sp[1] * t.w; b += sp[2] * t.w;
                }
                dstRow[3 * x + 0] = ClampF(r); dstRow[3 * x + 1] = ClampF(g); dstRow[3 * x + 2] = ClampF(b);
            }
        } else {
            for (int x = 0; x < nw; ++x) {
                float r = 0, g = 0, b = 0, a = 0;
                for (const auto& t : taps) {
                    const float* sp = tmp.data() + (static_cast<size_t>(t.idx) * nw + x) * 4;
                    r += sp[0] * t.w; g += sp[1] * t.w; b += sp[2] * t.w; a += sp[3] * t.w;
                }
                float af = std::max(0.0f, std::min(255.0f, a));
                uint8_t au = ClampF(af);
                if (au == 0) {
                    dstRow[4 * x + 0] = dstRow[4 * x + 1] = dstRow[4 * x + 2] = dstRow[4 * x + 3] = 0;
                } else {
                    float invA = 255.0f / af;
                    dstRow[4 * x + 0] = ClampF(r * invA);
                    dstRow[4 * x + 1] = ClampF(g * invA);
                    dstRow[4 * x + 2] = ClampF(b * invA);
                    dstRow[4 * x + 3] = au;
                }
            }
        }
    }
    return out;
}

Img ResizeLanczos(const Img& src, int nw, int nh) { return ResizeFast(src, nw, nh); }

Img FitWidth(const Img& im, int w) {
    int h = std::max(1, static_cast<int>(std::nearbyint(double(im.h) * w / im.w)));
    return ResizeLanczos(im, w, h);
}

Img FitHeight(const Img& im, int h) {
    int w = std::max(1, static_cast<int>(std::nearbyint(double(im.w) * h / im.h)));
    return ResizeLanczos(im, w, h);
}

// Separable 3-pass box approximation of Gaussian blur (works on 1, 3, or 4 channels).
void BoxBlur1D(const uint8_t* src, uint8_t* dst, int len, int stride, int ch, int r) {
    if (r <= 0 || len <= 1) {
        for (int i = 0; i < len; ++i) std::memcpy(dst + i * stride, src + i * stride, ch);
        return;
    }
    int win = 2 * r + 1;
    int sum[4] = {0, 0, 0, 0};
    for (int k = -r; k <= r; ++k) {
        int idx = std::max(0, std::min(len - 1, k));
        const uint8_t* p = src + idx * stride;
        for (int c = 0; c < ch; ++c) sum[c] += p[c];
    }
    for (int i = 0; i < len; ++i) {
        uint8_t* o = dst + i * stride;
        for (int c = 0; c < ch; ++c) o[c] = static_cast<uint8_t>((sum[c] + r) / win);
        int rem = std::max(0, std::min(len - 1, i - r));
        int add = std::max(0, std::min(len - 1, i + r + 1));
        const uint8_t* pr = src + rem * stride;
        const uint8_t* pa = src + add * stride;
        for (int c = 0; c < ch; ++c) sum[c] += int(pa[c]) - int(pr[c]);
    }
}

void GaussianBlurInPlace(uint8_t* data, int w, int h, int ch, float sigma) {
    if (sigma <= 0.1f || w <= 1 || h <= 1) return;
    // Standard 3-box Gaussian radii
    double wIdeal = std::sqrt((12.0 * sigma * sigma / 3.0) + 1.0);
    int wl = static_cast<int>(std::floor(wIdeal));
    if ((wl & 1) == 0) --wl;
    int wu = wl + 2;
    double mIdeal = (12.0 * sigma * sigma - 3.0 * wl * wl - 12.0 * wl - 9.0) / (-4.0 * wl - 4.0);
    int m = std::max(0, std::min(3, static_cast<int>(std::nearbyint(mIdeal))));
    int radii[3];
    for (int i = 0; i < 3; ++i) radii[i] = ((i < m ? wl : wu) - 1) / 2;

    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * ch);
    for (int pass = 0; pass < 3; ++pass) {
        int r = radii[pass];
        if (r <= 0) continue;
        for (int y = 0; y < h; ++y)
            BoxBlur1D(data + static_cast<size_t>(y) * w * ch, tmp.data() + static_cast<size_t>(y) * w * ch, w, ch, ch, r);
        for (int x = 0; x < w; ++x)
            BoxBlur1D(tmp.data() + x * ch, data + x * ch, h, w * ch, ch, r);
    }
}

// Rotate RGBA image counter-clockwise by deg degrees with expand=true (bilinear).
Img RotateRgbaExpand(const Img& src, double deg) {
    if (src.empty() || src.ch != 4) return src;
    double rad = deg * kPi / 180.0;
    double cs = std::cos(rad), sn = std::sin(rad);
    double cx = src.w * 0.5, cy = src.h * 0.5;
    double corners[4][2] = {{-cx, -cy}, {cx, -cy}, {cx, cy}, {-cx, cy}};
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (int i = 0; i < 4; ++i) {
        // CCW in screen coords (y down): x' = x*cos + y*sin, y' = -x*sin + y*cos
        double rx = corners[i][0] * cs + corners[i][1] * sn;
        double ry = -corners[i][0] * sn + corners[i][1] * cs;
        minX = std::min(minX, rx); maxX = std::max(maxX, rx);
        minY = std::min(minY, ry); maxY = std::max(maxY, ry);
    }
    int nw = std::max(1, static_cast<int>(std::ceil(maxX - minX)));
    int nh = std::max(1, static_cast<int>(std::ceil(maxY - minY)));
    Img out(nw, nh, 4, 0);
    double ncx = nw * 0.5, ncy = nh * 0.5;
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            double dx = (x + 0.5) - ncx;
            double dy = (y + 0.5) - ncy;
            // inverse rotation
            double sx = dx * cs - dy * sn + cx - 0.5;
            double sy = dx * sn + dy * cs + cy - 0.5;
            if (sx < -0.5 || sy < -0.5 || sx > src.w - 0.5 || sy > src.h - 0.5) continue;
            int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            double fx = sx - x0, fy = sy - y0;
            double acc[4] = {0, 0, 0, 0};
            double wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
            int xs[2] = {x0, x0 + 1}, ys[2] = {y0, y0 + 1};
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    if (xs[i] < 0 || xs[i] >= src.w || ys[j] < 0 || ys[j] >= src.h) continue;
                    const uint8_t* p = src.px(xs[i], ys[j]);
                    double wt = wts[j * 2 + i];
                    double a = p[3] * (1.0 / 255.0);
                    acc[0] += p[0] * a * wt;
                    acc[1] += p[1] * a * wt;
                    acc[2] += p[2] * a * wt;
                    acc[3] += p[3] * wt;
                }
            }
            uint8_t* dp = out.px(x, y);
            uint8_t au = ClampF(acc[3]);
            if (au > 0) {
                double invA = 255.0 / acc[3];
                dp[0] = ClampF(acc[0] * invA);
                dp[1] = ClampF(acc[1] * invA);
                dp[2] = ClampF(acc[2] * invA);
                dp[3] = au;
            }
        }
    }
    return out;
}

inline void AlphaOverPx(uint8_t d[4], const uint8_t s[4]) {
    if (s[3] == 0) return;
    if (s[3] == 255 || d[3] == 0) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        return;
    }
    uint32_t sa = s[3], da = d[3];
    uint32_t outA = sa * 255u + da * (255u - sa);
    if (outA == 0) { d[0] = d[1] = d[2] = d[3] = 0; return; }
    for (int c = 0; c < 3; ++c)
        d[c] = static_cast<uint8_t>((s[c] * sa * 255u + d[c] * da * (255u - sa) + outA / 2u) / outA);
    d[3] = static_cast<uint8_t>((outA + 127u) / 255u);
}

void AlphaComposite(Img& dst, const Img& src, int ox, int oy) {
    for (int y = 0; y < src.h; ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= dst.h) continue;
        for (int x = 0; x < src.w; ++x) {
            int dx = ox + x;
            if (dx < 0 || dx >= dst.w) continue;
            AlphaOverPx(dst.px(dx, dy), src.px(x, y));
        }
    }
}

void PasteRgbaOntoRgb(Img& rgb, const Img& rgba, int ox, int oy) {
    for (int y = 0; y < rgba.h; ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= rgb.h) continue;
        for (int x = 0; x < rgba.w; ++x) {
            int dx = ox + x;
            if (dx < 0 || dx >= rgb.w) continue;
            const uint8_t* s = rgba.px(x, y);
            uint8_t a = s[3];
            if (a == 0) continue;
            uint8_t* d = rgb.px(dx, dy);
            if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            else {
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<uint8_t>((d[c] * (255u - a) + s[c] * a + 127u) / 255u);
            }
        }
    }
}

struct ColorStop { double t; uint8_t r, g, b; };

Img VGradient(int w, int h, const ColorStop* stops, int n) {
    Img out(w, h, 3);
    for (int y = 0; y < h; ++y) {
        double t = double(y) / std::max(h - 1, 1);
        uint8_t r = stops[0].r, g = stops[0].g, b = stops[0].b;
        for (int s = 0; s + 1 < n; ++s) {
            if (stops[s].t <= t && t <= stops[s + 1].t) {
                double k = (t - stops[s].t) / std::max(stops[s + 1].t - stops[s].t, 1e-6);
                r = ClampF(stops[s].r + (stops[s + 1].r - stops[s].r) * k);
                g = ClampF(stops[s].g + (stops[s + 1].g - stops[s].g) * k);
                b = ClampF(stops[s].b + (stops[s + 1].b - stops[s].b) * k);
                break;
            }
        }
        uint8_t* row = out.px(0, y);
        for (int x = 0; x < w; ++x) { row[3 * x] = r; row[3 * x + 1] = g; row[3 * x + 2] = b; }
    }
    return out;
}

void ScreenRgbInPlace(Img& base, const Img& over, double opacity = 1.0) {
    for (size_t i = 0, n = static_cast<size_t>(base.w) * base.h * 3; i < n; ++i) {
        int ov = static_cast<int>(over.d[i] * opacity);
        int bv = base.d[i];
        base.d[i] = static_cast<uint8_t>(255 - ((255 - bv) * (255 - ov) + 127) / 255);
    }
}

void AddRgbaScreenInPlace(Img& base, const Img& over, int ox, int oy, double opacity = 1.0) {
    for (int y = 0; y < over.h; ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= base.h) continue;
        for (int x = 0; x < over.w; ++x) {
            int dx = ox + x;
            if (dx < 0 || dx >= base.w) continue;
            const uint8_t* s = over.px(x, y);
            double a = (s[3] * opacity) / 255.0;
            if (a <= 0.0) continue;
            uint8_t* d = base.px(dx, dy);
            for (int c = 0; c < 3; ++c) {
                int ov = ClampF(s[c] * a);
                d[c] = static_cast<uint8_t>(255 - ((255 - d[c]) * (255 - ov) + 127) / 255);
            }
        }
    }
}

void AddRadialGlow(Img& base, double cx, double cy, double radius, uint8_t cr, uint8_t cg, uint8_t cb, double strength) {
    double invR = 1.0 / std::max(radius, 1.0);
    int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    int y1 = std::min(base.h, static_cast<int>(std::ceil(cy + radius)));
    int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    int x1 = std::min(base.w, static_cast<int>(std::ceil(cx + radius)));
    for (int y = y0; y < y1; ++y) {
        double dy = (y - cy) * invR;
        double dy2 = dy * dy;
        for (int x = x0; x < x1; ++x) {
            double dx = (x - cx) * invR;
            double d2 = dx * dx + dy2;
            if (d2 >= 1.0) continue;
            double k = (1.0 - std::sqrt(d2));
            k = k * k * strength;
            uint8_t* d = base.px(x, y);
            int ov[3] = {ClampF(cr * k), ClampF(cg * k), ClampF(cb * k)};
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(255 - ((255 - d[c]) * (255 - ov[c]) + 127) / 255);
        }
    }
}

Img BoostRgb(const Img& src, double satD, double contrastD, double brightD, double sharpD) {
    Img out = src;
    const size_t n = static_cast<size_t>(out.w) * out.h;
    const float sat = static_cast<float>(satD), contrast = static_cast<float>(contrastD);
    const float bright = static_cast<float>(brightD), sharp = static_cast<float>(sharpD);
    // 1. Saturation + accumulate mean luminance in a single pass
    double meanSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = out.d.data() + 3 * i;
        float lum = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        int r = static_cast<int>(lum + (p[0] - lum) * sat + 0.5f);
        int g = static_cast<int>(lum + (p[1] - lum) * sat + 0.5f);
        int b = static_cast<int>(lum + (p[2] - lum) * sat + 0.5f);
        p[0] = ClampU8(r); p[1] = ClampU8(g); p[2] = ClampU8(b);
        meanSum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    }
    // 2. Combined Contrast + Brightness via 256-entry LUT (zero per-pixel float math!)
    const float mean = static_cast<float>(meanSum / std::max<size_t>(1, n));
    uint8_t cbLut[256];
    for (int v = 0; v < 256; ++v) cbLut[v] = ClampF((mean + (v - mean) * contrast) * bright);
    for (size_t i = 0; i < n * 3; ++i) out.d[i] = cbLut[out.d[i]];
    // 3. Sharpness (integer fixed-point 3x3 smooth blend)
    if (std::fabs(sharp - 1.0f) > 1e-3f && out.w >= 3 && out.h >= 3) {
        Img orig = out;
        const int shQ = static_cast<int>(sharp * 256.0f + 0.5f);
        const int invQ = 256 - shQ;
        const int stride = out.w * 3;
        for (int y = 1; y < out.h - 1; ++y) {
            const uint8_t* r0 = orig.d.data() + static_cast<size_t>(y - 1) * stride;
            const uint8_t* r1 = orig.d.data() + static_cast<size_t>(y) * stride;
            const uint8_t* r2 = orig.d.data() + static_cast<size_t>(y + 1) * stride;
            uint8_t* dst = out.d.data() + static_cast<size_t>(y) * stride;
            for (int x = 1; x < out.w - 1; ++x) {
                const int o = x * 3;
                for (int c = 0; c < 3; ++c) {
                    int s = r0[o - 3 + c] + r0[o + c] + r0[o + 3 + c] +
                            r1[o - 3 + c] + 5 * r1[o + c] + r1[o + 3 + c] +
                            r2[o - 3 + c] + r2[o + c] + r2[o + 3 + c];
                    int sm = (s * 20) >> 8;   // ~ s / 12.8 (close to s / 13)
                    dst[o + c] = ClampU8((sm * invQ + r1[o + c] * shQ + 128) >> 8);
                }
            }
        }
    }
    return out;
}

Img BoostRgba(const Img& src, double sat, double contrast, double bright, double sharp) {
    Img rgb = BoostRgb(ToRgb(src), sat, contrast, bright, sharp);
    Img out(src.w, src.h, 4);
    for (size_t i = 0, n = static_cast<size_t>(src.w) * src.h; i < n; ++i) {
        out.d[4 * i + 0] = rgb.d[3 * i + 0];
        out.d[4 * i + 1] = rgb.d[3 * i + 1];
        out.d[4 * i + 2] = rgb.d[3 * i + 2];
        out.d[4 * i + 3] = src.d[4 * i + 3];
    }
    return out;
}

void FeatherLeft(Img& rgba, int left) {
    if (left <= 0 || rgba.ch != 4) return;
    for (int y = 0; y < rgba.h; ++y)
        for (int x = 0; x < std::min(left, rgba.w); ++x) {
            uint8_t* p = rgba.px(x, y);
            p[3] = static_cast<uint8_t>(p[3] * x / left);
        }
}

Img GlowLayer(const Img& rgba, uint8_t cr, uint8_t cg, uint8_t cb, float radius, double opacity, int pad) {
    int cw = rgba.w + 2 * pad, chh = rgba.h + 2 * pad;
    Img canvas(cw, chh, 4, 0);
    for (int y = 0; y < rgba.h; ++y) {
        for (int x = 0; x < rgba.w; ++x) {
            uint8_t a = static_cast<uint8_t>(rgba.px(x, y)[3] * opacity);
            if (!a) continue;
            uint8_t* d = canvas.px(x + pad, y + pad);
            d[0] = cr; d[1] = cg; d[2] = cb; d[3] = a;
        }
    }
    GaussianBlurInPlace(canvas.d.data(), cw, chh, 4, radius);
    AlphaComposite(canvas, rgba, pad, pad);
    return canvas;
}

Img WithShadow(const Img& rgba, int offX, int offY, float blur, double opacity, int& outPad) {
    int pad = static_cast<int>(blur) * 3 + std::max(offX, offY);
    outPad = pad;
    int cw = rgba.w + 2 * pad, chh = rgba.h + 2 * pad;
    Img canvas(cw, chh, 4, 0);
    for (int y = 0; y < rgba.h; ++y) {
        for (int x = 0; x < rgba.w; ++x) {
            uint8_t a = static_cast<uint8_t>(rgba.px(x, y)[3] * opacity);
            if (!a) continue;
            uint8_t* d = canvas.px(x + pad + offX, y + pad + offY);
            d[0] = 0; d[1] = 0; d[2] = 0; d[3] = a;
        }
    }
    GaussianBlurInPlace(canvas.d.data(), cw, chh, 4, blur);
    AlphaComposite(canvas, rgba, pad, pad);
    return canvas;
}

void ApplyVignette(Img& rgb, double strength) {
    for (int y = 0; y < rgb.h; ++y) {
        double ny = (y + 0.5) / rgb.h * 2.0 - 1.0;
        for (int x = 0; x < rgb.w; ++x) {
            double nx = (x + 0.5) / rgb.w * 2.0 - 1.0;
            // Pillow radial_gradient has 0 at center, ~180 at edge midpoint, 255 at corners
            double v = std::min(255.0, std::sqrt(nx * nx + ny * ny) * 180.312229);
            int m = static_cast<int>(std::max(0.0, v - 90.0) * strength * 255.0 / 165.0);
            if (m <= 0) continue;
            m = std::min(255, m);
            uint8_t* p = rgb.px(x, y);
            static const uint8_t col[3] = {6, 4, 10};
            for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>((p[c] * (255 - m) + col[c] * m + 127) / 255);
        }
    }
}

void ApplyBottomShade(Img& rgb, int startY, double strength) {
    if (startY >= rgb.h) return;
    int span = std::max(1, rgb.h - 1 - startY);
    static const uint8_t col[3] = {8, 5, 14};
    for (int y = startY; y < rgb.h; ++y) {
        int m = static_cast<int>(255.0 * strength * (y - startY) / span);
        uint8_t* row = rgb.px(0, y);
        for (int x = 0; x < rgb.w; ++x)
            for (int c = 0; c < 3; ++c) row[3 * x + c] = static_cast<uint8_t>((row[3 * x + c] * (255 - m) + col[c] * m + 127) / 255);
    }
}

// -------------------------------------------------------------------------------------------------
// Game heading font (Art/fe/fonts/engfontheading36.res) -> flame_label("START", 70)
// -------------------------------------------------------------------------------------------------
struct GlyphInfo { int x, y, w, adv, off; };

Img RenderFlameStart(const std::vector<uint8_t>& fontBlob, int targetH) {
    if (fontBlob.size() < 16) return {};
    uint32_t sector = Be32(fontBlob.data()), count = Be32(fontBlob.data() + 4), dir = Be32(fontBlob.data() + 8);
    if (sector < 16 || sector > 0x10000 || count == 0 || count > 10000) return {};
    uint64_t dirOff = uint64_t(dir) * sector;
    if (dirOff + uint64_t(count) * 12 > fontBlob.size()) return {};
    std::string desc;
    uint32_t pageHash = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = fontBlob.data() + dirOff + i * 12;
        uint32_t h = Be32(e), block = Be32(e + 4), length = Be32(e + 8);
        uint64_t off = uint64_t(block) * sector;
        if (off + length > fontBlob.size()) continue;
        if (length >= 3 && std::memcmp(fontBlob.data() + off, "NLG", 3) == 0)
            desc.assign(reinterpret_cast<const char*>(fontBlob.data() + off), length);
        else if (pageHash == 0)
            pageHash = h;
    }
    if (desc.empty() || pageHash == 0) return {};
    Img page = LoadBundleTexture(fontBlob, pageHash);
    if (page.empty()) return {};

    int pageSize = 512, renderH = 0, gx = 0, gy = 0;
    GlyphInfo glyphs[128] = {};
    bool have[128] = {};
    size_t pos = 0;
    while (pos < desc.size()) {
        size_t eol = desc.find('\n', pos);
        if (eol == std::string::npos) eol = desc.size();
        std::string line = desc.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 8, "PageSize") == 0) {
            std::sscanf(line.c_str() + 8, "%d", &pageSize);
        } else if (line.compare(0, 6, "Height") == 0) {
            const char* rh = std::strstr(line.c_str(), "RenderHeight");
            if (rh) std::sscanf(rh + 12, "%d", &renderH);
        } else if (line.compare(0, 5, "Glyph") == 0) {
            char tok[32] = {}, wtok[32] = {};
            int adv = 0, gw = 0, goff = 0;
            if (std::sscanf(line.c_str() + 5, "%31s %31s %d %d %d", tok, wtok, &adv, &gw, &goff) == 5) {
                int ch = (tok[1] == 0) ? static_cast<unsigned char>(tok[0]) : std::atoi(tok);
                if (gx + gw > pageSize) { gx = 0; gy += renderH; }
                if (ch >= 0 && ch < 128) {
                    glyphs[ch] = {gx, gy, gw, adv, goff};
                    have[ch] = true;
                }
                gx += gw;
            }
        }
    }
    if (renderH <= 0 || renderH > page.h) return {};

    const char* label = "START";
    int pen = 0;
    struct Placed { int x; GlyphInfo g; };
    std::vector<Placed> placed;
    for (const char* s = label; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        if (c < 128 && have[c]) {
            placed.push_back({pen + glyphs[c].off, glyphs[c]});
            pen += glyphs[c].adv;
        } else {
            pen += 14;
        }
    }
    Img strip(pen + 24, renderH, 4, 0);
    for (const auto& pl : placed) {
        Img g = Crop(page, pl.g.x, pl.g.y, pl.g.x + pl.g.w, pl.g.y + renderH);
        AlphaComposite(strip, g, std::max(pl.x, 0) + 8, 0);
    }
    strip = TrimAlpha(strip, 8);
    if (strip.empty()) return {};

    static const ColorStop kFlame[] = {{0.0, 255, 244, 120}, {0.45, 255, 178, 38}, {1.0, 232, 84, 18}};
    Img fill = VGradient(strip.w, strip.h, kFlame, 3);
    Img coloured(strip.w, strip.h, 4);
    for (int y = 0; y < strip.h; ++y) {
        for (int x = 0; x < strip.w; ++x) {
            const uint8_t* sp = strip.px(x, y);
            const uint8_t* fp = fill.px(x, y);
            double lum = (0.299 * sp[0] + 0.587 * sp[1] + 0.114 * sp[2]) * 1.25;
            int m = ClampF(lum);
            uint8_t* dp = coloured.px(x, y);
            static const uint8_t ink[3] = {28, 10, 12};
            for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((fp[c] * (255 - m) + ink[c] * m + 127) / 255);
            dp[3] = sp[3];
        }
    }
    Img scaled = FitHeight(coloured, targetH);
    int pad = 0;
    return WithShadow(scaled, std::max(1, targetH / 18), std::max(2, targetH / 12), static_cast<float>(std::max(1, targetH / 16)), 0.65, pad);
}

// -------------------------------------------------------------------------------------------------
// Art33 compositions (icon0, startup, bg, pic0)
// -------------------------------------------------------------------------------------------------
struct DiscAssets {
    Img art, logo, sky, ball, sunburst, iceburst, boltA, boltB, boltC, sparks, mario, bowser;
    std::vector<uint8_t> fontBlob;
};

Img MakeStadiumScene(const DiscAssets& a, int w, int h, int logoW, int logoY, int heroH, int ballCy, double ballScale, bool isLiveArea) {
    if (a.mario.empty() || a.bowser.empty()) {
        // Fallback matching brand-from-disc.py when portraits are missing
        Img im = ResizeLanczos(a.art, w, h);
        for (uint8_t& v : im.d) v = static_cast<uint8_t>(v * 0.45);
        int pad = 0;
        Img mark = WithShadow(FitWidth(a.logo, logoW), 4, 7, 6.0f, 0.65, pad);
        PasteRgbaOntoRgb(im, mark, isLiveArea ? (w - mark.w) / 2 : 40, logoY);
        ApplyVignette(im, 0.45);
        return im;
    }

    static const ColorStop kBgStops[] = {
        {0.00, 12, 22, 56}, {0.28, 30, 22, 66}, {0.56, 108, 38, 28}, {0.80, 176, 82, 22}, {1.00, 24, 16, 20},
    };
    Img bg = VGradient(w, h, kBgStops, 5);

    if (!a.sky.empty()) {
        int ch = static_cast<int>(h * 0.64);
        Img clouds = BoostRgb(ResizeLanczos(a.sky, w, ch), 1.35, 1.18, 0.98, 1.0);
        for (int y = 0; y < ch; ++y) {
            double t = double(y) / std::max(ch - 1, 1);
            int m = (t <= 0.62) ? static_cast<int>(220 - (25.0 * t / 0.62)) : static_cast<int>(195.0 * (1.0 - (t - 0.62) / 0.38));
            m = std::max(0, std::min(255, m));
            for (int x = 0; x < w; ++x) {
                uint8_t* dp = bg.px(x, y);
                const uint8_t* sp = clouds.px(x, y);
                for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((dp[c] * (255 - m) + sp[c] * m + 127) / 255);
            }
        }
    }

    if (!a.sunburst.empty() && a.sunburst.w >= 128 && a.sunburst.h >= 128) {
        int inset = std::min({44, a.sunburst.w / 10, a.sunburst.h / 10});
        Img sun = BoostRgb(ResizeLanczos(ToRgb(Crop(a.sunburst, inset, inset, a.sunburst.w - inset, a.sunburst.h - inset)), w, h), 1.22, 1.15, 0.72, 1.25);
        ScreenRgbInPlace(bg, sun, 0.22);
    }

    if (!a.iceburst.empty() && a.iceburst.w >= 64 && a.iceburst.h >= 64) {
        int inset = std::min({16, a.iceburst.w / 12, a.iceburst.h / 12});
        Img ice = BoostRgb(ResizeLanczos(ToRgb(Crop(a.iceburst, inset, inset, a.iceburst.w - inset, a.iceburst.h - inset)), w, h), 1.35, 1.22, 0.82, 1.25);
        for (int y = 0; y < h; ++y) {
            double ny = (y + 0.5) / h * 2.0 - 1.0;
            for (int x = 0; x < w; ++x) {
                double nx = (x + 0.5) / w * 2.0 - 1.0;
                double v = std::min(255.0, std::sqrt(nx * nx + ny * ny) * 180.312229);
                double aMask = std::max(0.0, 255.0 - v) / 255.0 * 0.34;
                uint8_t* dp = bg.px(x, y);
                const uint8_t* sp = ice.px(x, y);
                for (int c = 0; c < 3; ++c) {
                    int ov = ClampF(sp[c] * aMask);
                    dp[c] = static_cast<uint8_t>(255 - ((255 - dp[c]) * (255 - ov) + 127) / 255);
                }
            }
        }
    }

    const Img& ballSrc = a.ball.empty() ? a.art : a.ball;
    int bsz = static_cast<int>(h * ballScale);
    int binset = std::min({28, ballSrc.w / 16, ballSrc.h / 16});
    Img ballSq = BoostRgb(ResizeLanczos(ToRgb(Crop(ballSrc, binset, binset, ballSrc.w - binset, ballSrc.h - binset)), bsz, bsz),
                          1.28, 1.20, isLiveArea ? 1.06 : 0.92, 1.20);
    int bx0 = (w - bsz) / 2, by0 = ballCy - bsz / 2;
    for (int y = 0; y < bsz; ++y) {
        int dy = by0 + y;
        if (dy < 0 || dy >= h) continue;
        double ny = (y + 0.5) / bsz * 2.0 - 1.0;
        for (int x = 0; x < bsz; ++x) {
            int dx = bx0 + x;
            if (dx < 0 || dx >= w) continue;
            double nx = (x + 0.5) / bsz * 2.0 - 1.0;
            double v = std::min(255.0, std::sqrt(nx * nx + ny * ny) * 180.312229);
            int m = (v < 75.0) ? 255 : static_cast<int>(255.0 * std::pow(std::max(0.0, 1.0 - (v - 75.0) / 99.0), 1.6));
            if (m <= 0) continue;
            uint8_t* dp = bg.px(dx, dy);
            const uint8_t* sp = ballSq.px(x, y);
            for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((dp[c] * (255 - m) + sp[c] * m + 127) / 255);
        }
    }

    AddRadialGlow(bg, w * 0.16, h * 0.65, h * 0.62, 255, 115, 25, 0.58);
    AddRadialGlow(bg, w * 0.84, h * 0.65, h * 0.62, 30, 185, 255, 0.58);
    AddRadialGlow(bg, w * 0.50, h * 0.28, h * 0.68, 255, 195, 55, 0.38);

    if (!a.boltA.empty()) {
        Img b1 = RotateRgbaExpand(ResizeLanczos(a.boltA, static_cast<int>(w * 0.34), static_cast<int>(h * 0.28)), 14.0);
        AddRgbaScreenInPlace(bg, b1, static_cast<int>(w * 0.03), static_cast<int>(h * 0.04), 0.82);
    }
    if (!a.boltB.empty()) {
        Img b2 = RotateRgbaExpand(ResizeLanczos(a.boltB, static_cast<int>(w * 0.34), static_cast<int>(h * 0.28)), -16.0);
        AddRgbaScreenInPlace(bg, b2, w - b2.w - static_cast<int>(w * 0.02), static_cast<int>(h * 0.05), 0.82);
    }
    if (!a.boltC.empty()) {
        int s3 = static_cast<int>(h * 0.44);
        Img b3 = RotateRgbaExpand(ResizeLanczos(a.boltC, s3, s3), 20.0);
        AddRgbaScreenInPlace(bg, b3, w / 2 - b3.w / 2, static_cast<int>(h * 0.22), 0.42);
    }

    // Mario (left) & Bowser (right) hero clash
    {
        Img mHero = BoostRgba(FitHeight(a.mario, heroH), 1.18, 1.12, 1.06, 1.35);
        int mPad = 36;
        Img mGlow = GlowLayer(mHero, 255, 165, 35, 14.0f, 0.72, mPad);
        int mx = isLiveArea ? -26 : -18;
        PasteRgbaOntoRgb(bg, mGlow, mx - mPad, h - mHero.h + 12 - mPad);
    }
    {
        Img bHero = BoostRgba(FitHeight(a.bowser, static_cast<int>(heroH * 1.02)), 1.16, 1.12, 1.05, 1.35);
        FeatherLeft(bHero, 26);
        int bPad = 36;
        Img bGlow = GlowLayer(bHero, 50, 205, 255, 14.0f, 0.75, bPad);
        int bx = isLiveArea ? (w - bHero.w + 32) : (w - bHero.w + 24);
        PasteRgbaOntoRgb(bg, bGlow, bx - bPad, h - bHero.h + 14 - bPad);
    }

    ApplyBottomShade(bg, static_cast<int>(h * 0.78), 0.50);
    ApplyVignette(bg, 0.28);

    if (!a.sparks.empty()) {
        Img sp = ResizeLanczos(a.sparks, static_cast<int>(w * 0.54), static_cast<int>(h * 0.22));
        AddRgbaScreenInPlace(bg, sp, (w - sp.w) / 2, logoY + static_cast<int>(logoW * 0.34), 0.68);
    }

    Img heroLogo = BoostRgba(FitWidth(a.logo, logoW), 1.12, 1.08, 1.04, 1.22);
    int lpad = 26, shPad = 0;
    Img logoGlow = GlowLayer(heroLogo, 45, 195, 255, 10.0f, 0.48, lpad);
    Img logoSh = WithShadow(logoGlow, 4, 7, 7.0f, 0.80, shPad);
    PasteRgbaOntoRgb(bg, logoSh, (w - logoSh.w) / 2, logoY - lpad - shPad);
    return bg;
}

Img MakeIcon(const DiscAssets& a) {
    const int s = 128;
    if (a.mario.empty()) {
        Img im = ResizeLanczos(a.art, s, s);
        for (uint8_t& v : im.d) v = static_cast<uint8_t>(v * 0.78);
        ApplyBottomShade(im, 52, 0.85);
        int pad = 0;
        Img mk = WithShadow(FitWidth(a.logo, 96), 1, 2, 2.0f, 0.85, pad);
        PasteRgbaOntoRgb(im, mk, (s - mk.w) / 2, 117 - mk.h);
        return im;
    }

    static const ColorStop kStops[] = {{0.00, 14, 92, 198}, {0.48, 30, 48, 124}, {0.78, 205, 82, 18}, {1.00, 82, 22, 14}};
    Img bg = VGradient(s, s, kStops, 4);

    if (!a.iceburst.empty() && a.iceburst.w >= 64 && a.iceburst.h >= 64) {
        int inset = std::min({16, a.iceburst.w / 12, a.iceburst.h / 12});
        Img ice = BoostRgb(ResizeLanczos(ToRgb(Crop(a.iceburst, inset, inset, a.iceburst.w - inset, a.iceburst.h - inset)), s, s), 1.38, 1.22, 0.88, 1.25);
        ScreenRgbInPlace(bg, ice, 0.40);
    }
    AddRadialGlow(bg, 64.0, 48.0, 72.0, 255, 210, 65, 0.58);
    if (!a.boltB.empty()) {
        Img bSm = RotateRgbaExpand(ResizeLanczos(a.boltB, 120, 58), -16.0);
        AddRgbaScreenInPlace(bg, bSm, 4, 14, 0.72);
    }

    Img m = BoostRgba(FitHeight(a.mario, 80), 1.22, 1.15, 1.08, 1.45);
    int mpad = 14;
    Img mg = GlowLayer(m, 255, 215, 75, 5.0f, 0.78, mpad);
    PasteRgbaOntoRgb(bg, mg, (s - m.w) / 2 - mpad + 2, 6 - mpad);

    // Dark elliptical cradle behind bottom logo
    std::vector<uint8_t> cradle(s * s, 0);
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            double dx = (x - 64.0) / 50.0, dy = (y - 92.0) / 30.0;
            if (dx * dx + dy * dy <= 1.0) cradle[y * s + x] = 155;
        }
    GaussianBlurInPlace(cradle.data(), s, s, 1, 10.0f);
    static const uint8_t cradleCol[3] = {10, 6, 18};
    for (int i = 0; i < s * s; ++i) {
        int k = cradle[i];
        if (!k) continue;
        for (int c = 0; c < 3; ++c) bg.d[3 * i + c] = static_cast<uint8_t>((bg.d[3 * i + c] * (255 - k) + cradleCol[c] * k + 127) / 255);
    }

    // Electric cyan -> gold -> orange circular ring (outer r=56, width=4)
    std::vector<uint8_t> ring(s * s, 0), ringGlow(s * s, 0);
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            double d = std::hypot(x + 0.5 - 64.0, y + 0.5 - 64.0);
            // band from 52.0 to 56.0
            double dist = std::max(52.0 - d, d - 56.0);
            double cov = std::max(0.0, std::min(1.0, 0.5 - dist));
            uint8_t v = ClampF(cov * 210.0);
            ring[y * s + x] = v;
            ringGlow[y * s + x] = v;
        }
    GaussianBlurInPlace(ringGlow.data(), s, s, 1, 2.0f);
    static const ColorStop kRStops[] = {{0.0, 115, 238, 255}, {0.48, 255, 230, 100}, {1.0, 255, 112, 22}};
    Img rgrad = VGradient(s, s, kRStops, 3);
    for (int i = 0; i < s * s; ++i) {
        int kg = ringGlow[i];
        if (kg) for (int c = 0; c < 3; ++c) bg.d[3 * i + c] = static_cast<uint8_t>((bg.d[3 * i + c] * (255 - kg) + rgrad.d[3 * i + c] * kg + 127) / 255);
        int kr = static_cast<int>(ring[i] * 0.92);
        if (kr) for (int c = 0; c < 3; ++c) bg.d[3 * i + c] = static_cast<uint8_t>((bg.d[3 * i + c] * (255 - kr) + rgrad.d[3 * i + c] * kr + 127) / 255);
    }

    Img mk = BoostRgba(FitWidth(a.logo, 92), 1.16, 1.12, 1.06, 1.32);
    int sp = 0;
    Img mkS = WithShadow(mk, 1, 2, 2.0f, 0.92, sp);
    PasteRgbaOntoRgb(bg, mkS, (s - mkS.w) / 2, 117 - mkS.h);
    return bg;
}

double RoundedRectSdf(double px, double py, double x0, double y0, double x1, double y1, double r) {
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double hx = (x1 - x0) * 0.5 - r, hy = (y1 - y0) * 0.5 - r;
    double qx = std::fabs(px - cx) - hx, qy = std::fabs(py - cy) - hy;
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
}

Img MakeStartup(const DiscAssets& a) {
    const int w = 280, h = 158;
    static const ColorStop kBaseStops[] = {{0.0, 20, 40, 82}, {0.45, 14, 22, 48}, {0.60, 12, 16, 32}, {1.0, 32, 16, 18}};
    Img base = VGradient(w, h, kBaseStops, 4);

    const Img& ballSrc = a.ball.empty() ? a.art : a.ball;
    int binset = std::min({24, ballSrc.w / 16, ballSrc.h / 16});
    Img b = BoostRgb(ResizeLanczos(ToRgb(Crop(ballSrc, binset, binset, ballSrc.w - binset, ballSrc.h - binset)), w, w), 1.30, 1.18, 0.68, 1.15);
    int by0 = (h - w) / 2 + 4;
    for (int y = 0; y < w; ++y) {
        int dy = by0 + y;
        if (dy < 0 || dy >= h) continue;
        double ny = (y + 0.5) / w * 2.0 - 1.0;
        for (int x = 0; x < w; ++x) {
            double nx = (x + 0.5) / w * 2.0 - 1.0;
            double v = std::min(255.0, std::sqrt(nx * nx + ny * ny) * 180.312229);
            int m = static_cast<int>(std::pow(std::max(0.0, 1.0 - v / 176.0), 1.35) * 215.0);
            if (m <= 0) continue;
            uint8_t* dp = base.px(x, dy);
            const uint8_t* sp = b.px(x, y);
            for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((dp[c] * (255 - m) + sp[c] * m + 127) / 255);
        }
    }

    AddRadialGlow(base, w * 0.5, 14.0, 175.0, 55, 205, 255, 0.48);
    AddRadialGlow(base, w * 0.5, h - 8.0, 185.0, 255, 135, 20, 0.54);
    if (!a.boltA.empty()) {
        Img ba = ResizeLanczos(a.boltA, 256, 94);
        AddRgbaScreenInPlace(base, ba, 12, 30, 0.58);
    }

    // Top subtle gloss ellipse
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double dx = (x + 0.5 - w * 0.5) / (w * 0.5 + 40.0);
            double dy = (y + 0.5 - (-95.0 + (h / 2 + 12.0)) * 0.5) / (((h / 2 + 12.0) - (-95.0)) * 0.5);
            if (dx * dx + dy * dy <= 1.0) {
                uint8_t* dp = base.px(x, y);
                for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((dp[c] * (255 - 28) + 255 * 28 + 127) / 255);
            }
        }
    }

    // Outer cyan -> gold -> orange rounded bevel rim + inner ice-white hairline
    static const ColorStop kRimStops[] = {{0.0, 125, 235, 255}, {0.45, 255, 228, 100}, {1.0, 245, 95, 20}};
    Img rimGrad = VGradient(w, h, kRimStops, 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double dOut = RoundedRectSdf(x + 0.5, y + 0.5, 3.0, 3.0, w - 4.0, h - 4.0, 14.0);
            double bandOut = std::max(dOut, -dOut - 4.0);
            double covOut = std::max(0.0, std::min(1.0, 0.5 - bandOut));
            uint8_t* dp = base.px(x, y);
            if (covOut > 0.0) {
                int m = ClampF(covOut * 255.0);
                const uint8_t* rp = rimGrad.px(x, y);
                for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((dp[c] * (255 - m) + rp[c] * m + 127) / 255);
            }
            double dIn = RoundedRectSdf(x + 0.5, y + 0.5, 8.0, 8.0, w - 9.0, h - 9.0, 10.0);
            double bandIn = std::max(dIn, -dIn - 1.0);
            double covIn = std::max(0.0, std::min(1.0, 0.5 - bandIn));
            if (covIn > 0.0) {
                int m = ClampF(covIn * 135.0);
                static const uint8_t iceCol[3] = {225, 248, 255};
                for (int c = 0; c < 3; ++c) dp[c] = static_cast<uint8_t>((dp[c] * (255 - m) + iceCol[c] * m + 127) / 255);
            }
        }
    }

    Img label = RenderFlameStart(a.fontBlob, 70);
    if (!label.empty()) {
        Img lg = GlowLayer(label, 255, 165, 25, 7.0f, 0.52, 18);
        PasteRgbaOntoRgb(base, lg, (w - lg.w) / 2, (h - lg.h) / 2 + 2);
    } else if (!a.logo.empty()) {
        int pad = 0;
        Img mk = WithShadow(FitWidth(a.logo, 210), 2, 4, 4.0f, 0.85, pad);
        PasteRgbaOntoRgb(base, mk, (w - mk.w) / 2, (h - mk.h) / 2);
    }
    return base;
}

// -------------------------------------------------------------------------------------------------
// Median-cut 256-color quantizer + Floyd-Steinberg dither + 8-bit indexed PNG encoder (type 3)
// -------------------------------------------------------------------------------------------------
struct HistEntry {
    uint8_t r, g, b;   // 5-bit quantized (0..31)
    uint32_t count;
    uint64_t sr, sg, sb;
};

struct ColorBox {
    size_t start, end;
    uint8_t rMin, rMax, gMin, gMax, bMin, bMax;
    uint64_t totalCount;
    int longestAxis() const {
        int dr = rMax - rMin, dg = gMax - gMin, db = bMax - bMin;
        if (dg >= dr && dg >= db) return 1;
        if (dr >= dg && dr >= db) return 0;
        return 2;
    }
    uint64_t score() const {
        int span = std::max({rMax - rMin, gMax - gMin, bMax - bMin});
        return static_cast<uint64_t>(span) * totalCount;
    }
};

void ComputeBoxBounds(const std::vector<HistEntry>& h, ColorBox& bx) {
    bx.rMin = bx.gMin = bx.bMin = 31;
    bx.rMax = bx.gMax = bx.bMax = 0;
    bx.totalCount = 0;
    for (size_t i = bx.start; i < bx.end; ++i) {
        bx.rMin = std::min(bx.rMin, h[i].r); bx.rMax = std::max(bx.rMax, h[i].r);
        bx.gMin = std::min(bx.gMin, h[i].g); bx.gMax = std::max(bx.gMax, h[i].g);
        bx.bMin = std::min(bx.bMin, h[i].b); bx.bMax = std::max(bx.bMax, h[i].b);
        bx.totalCount += h[i].count;
    }
}

void AppendBe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t((x >> 24) & 255));
    v.push_back(uint8_t((x >> 16) & 255));
    v.push_back(uint8_t((x >> 8) & 255));
    v.push_back(uint8_t(x & 255));
}

void AppendPngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* payload, size_t len) {
    AppendBe32(out, static_cast<uint32_t>(len));
    size_t start = out.size();
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(type), reinterpret_cast<const uint8_t*>(type) + 4);
    if (len) out.insert(out.end(), payload, payload + len);
    uint32_t crc = Crc32(out.data() + start, 4 + len);
    AppendBe32(out, crc);
}

bool EncodeIndexedPng(const Img& rgb, std::vector<uint8_t>& outPng) {
    outPng.clear();
    if (rgb.empty() || rgb.ch != 3) return false;
    const int w = rgb.w, h = rgb.h;
    const size_t npix = static_cast<size_t>(w) * h;

    // 1. Build 32x32x32 RGB555 histogram
    struct Acc { uint32_t cnt = 0; uint64_t sr = 0, sg = 0, sb = 0; };
    std::vector<Acc> grid(32768);
    for (size_t i = 0; i < npix; ++i) {
        uint8_t r = rgb.d[3 * i], g = rgb.d[3 * i + 1], b = rgb.d[3 * i + 2];
        size_t idx = (size_t(r >> 3) << 10) | (size_t(g >> 3) << 5) | size_t(b >> 3);
        grid[idx].cnt++;
        grid[idx].sr += r;
        grid[idx].sg += g;
        grid[idx].sb += b;
    }
    std::vector<HistEntry> hist;
    hist.reserve(4096);
    for (size_t idx = 0; idx < 32768; ++idx) {
        if (grid[idx].cnt) {
            hist.push_back({uint8_t((idx >> 10) & 31), uint8_t((idx >> 5) & 31), uint8_t(idx & 31),
                            grid[idx].cnt, grid[idx].sr, grid[idx].sg, grid[idx].sb});
        }
    }
    grid.clear();
    grid.shrink_to_fit();

    // 2. Median-cut into up to 256 boxes
    std::vector<ColorBox> boxes;
    boxes.reserve(256);
    if (!hist.empty()) {
        ColorBox root{0, hist.size(), 0, 0, 0, 0, 0, 0, 0};
        ComputeBoxBounds(hist, root);
        boxes.push_back(root);
        while (boxes.size() < 256) {
            int best = -1;
            uint64_t bestScore = 0;
            for (size_t i = 0; i < boxes.size(); ++i) {
                if (boxes[i].end - boxes[i].start < 2) continue;
                uint64_t sc = boxes[i].score();
                if (sc > bestScore) { bestScore = sc; best = static_cast<int>(i); }
            }
            if (best < 0) break;
            ColorBox bx = boxes[best];
            int axis = bx.longestAxis();
            std::sort(hist.begin() + bx.start, hist.begin() + bx.end, [axis](const HistEntry& a, const HistEntry& b) {
                if (axis == 0) return a.r < b.r;
                if (axis == 1) return a.g < b.g;
                return a.b < b.b;
            });
            uint64_t half = bx.totalCount / 2;
            uint64_t acc = 0;
            size_t mid = bx.start + 1;
            for (size_t i = bx.start; i + 1 < bx.end; ++i) {
                acc += hist[i].count;
                mid = i + 1;
                if (acc >= half) break;
            }
            ColorBox left{bx.start, mid, 0, 0, 0, 0, 0, 0, 0};
            ColorBox right{mid, bx.end, 0, 0, 0, 0, 0, 0, 0};
            ComputeBoxBounds(hist, left);
            ComputeBoxBounds(hist, right);
            boxes[best] = left;
            boxes.push_back(right);
        }
    }

    // 3. Palette table (256 entries, 768 bytes)
    uint8_t pal[256][3] = {};
    int nPal = static_cast<int>(boxes.size());
    for (int i = 0; i < nPal; ++i) {
        uint64_t sr = 0, sg = 0, sb = 0, cnt = 0;
        for (size_t k = boxes[i].start; k < boxes[i].end; ++k) {
            sr += hist[k].sr; sg += hist[k].sg; sb += hist[k].sb; cnt += hist[k].count;
        }
        if (cnt) {
            pal[i][0] = ClampU8(static_cast<int>((sr + cnt / 2) / cnt));
            pal[i][1] = ClampU8(static_cast<int>((sg + cnt / 2) / cnt));
            pal[i][2] = ClampU8(static_cast<int>((sb + cnt / 2) / cnt));
        }
    }
    if (nPal == 0) nPal = 1;

    // 4. Floyd-Steinberg error diffusion with lazy 32x32x32 nearest-color cache
    std::vector<int16_t> lut(32768, -1);
    auto nearest = [&](int r, int g, int b) -> uint8_t {
        r = std::max(0, std::min(255, r));
        g = std::max(0, std::min(255, g));
        b = std::max(0, std::min(255, b));
        size_t key = (size_t(r >> 3) << 10) | (size_t(g >> 3) << 5) | size_t(b >> 3);
        int16_t cached = lut[key];
        if (cached >= 0) return static_cast<uint8_t>(cached);
        int cr = ((r >> 3) << 3) | 4, cg = ((g >> 3) << 3) | 4, cb = ((b >> 3) << 3) | 4;
        int best = 0, bestD = 0x7FFFFFFF;
        for (int i = 0; i < nPal; ++i) {
            int dr = cr - pal[i][0], dg = cg - pal[i][1], db = cb - pal[i][2];
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bestD) { bestD = d; best = i; if (d == 0) break; }
        }
        lut[key] = static_cast<int16_t>(best);
        return static_cast<uint8_t>(best);
    };

    std::vector<uint8_t> raw(static_cast<size_t>(h) * (w + 1));
    std::vector<int16_t> errCur(static_cast<size_t>(w + 2) * 3, 0);
    std::vector<int16_t> errNxt(static_cast<size_t>(w + 2) * 3, 0);
    for (int y = 0; y < h; ++y) {
        std::fill(errNxt.begin(), errNxt.end(), 0);
        uint8_t* dstRow = raw.data() + static_cast<size_t>(y) * (w + 1);
        dstRow[0] = 0;   // PNG filter None
        const uint8_t* srcRow = rgb.px(0, y);
        for (int x = 0; x < w; ++x) {
            int r = srcRow[3 * x + 0] + (errCur[(x + 1) * 3 + 0] >> 4);
            int g = srcRow[3 * x + 1] + (errCur[(x + 1) * 3 + 1] >> 4);
            int b = srcRow[3 * x + 2] + (errCur[(x + 1) * 3 + 2] >> 4);
            uint8_t idx = nearest(r, g, b);
            dstRow[1 + x] = idx;
            int er = std::max(-64, std::min(64, r - pal[idx][0]));
            int eg = std::max(-64, std::min(64, g - pal[idx][1]));
            int eb = std::max(-64, std::min(64, b - pal[idx][2]));
            errCur[(x + 2) * 3 + 0] += static_cast<int16_t>(er * 7);
            errCur[(x + 2) * 3 + 1] += static_cast<int16_t>(eg * 7);
            errCur[(x + 2) * 3 + 2] += static_cast<int16_t>(eb * 7);
            errNxt[(x + 0) * 3 + 0] += static_cast<int16_t>(er * 3);
            errNxt[(x + 0) * 3 + 1] += static_cast<int16_t>(eg * 3);
            errNxt[(x + 0) * 3 + 2] += static_cast<int16_t>(eb * 3);
            errNxt[(x + 1) * 3 + 0] += static_cast<int16_t>(er * 5);
            errNxt[(x + 1) * 3 + 1] += static_cast<int16_t>(eg * 5);
            errNxt[(x + 1) * 3 + 2] += static_cast<int16_t>(eb * 5);
            errNxt[(x + 2) * 3 + 0] += static_cast<int16_t>(er * 1);
            errNxt[(x + 2) * 3 + 1] += static_cast<int16_t>(eg * 1);
            errNxt[(x + 2) * 3 + 2] += static_cast<int16_t>(eb * 1);
        }
        errCur.swap(errNxt);
    }

    // 5. Compress IDAT with stbi_zlib_compress (quality 1: fast on ARMv7, nearly identical size on dithered 8-bit PNG)
    int zlen = 0;
    unsigned char* zbuf = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &zlen, 1);
    if (!zbuf || zlen <= 0) {
        if (zbuf) STBIW_FREE(zbuf);
        return false;
    }

    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    outPng.reserve(static_cast<size_t>(zlen) + 1024);
    outPng.insert(outPng.end(), kSig, kSig + 8);

    uint8_t ihdr[13] = {
        uint8_t((w >> 24) & 255), uint8_t((w >> 16) & 255), uint8_t((w >> 8) & 255), uint8_t(w & 255),
        uint8_t((h >> 24) & 255), uint8_t((h >> 16) & 255), uint8_t((h >> 8) & 255), uint8_t(h & 255),
        8,   // bit depth 8
        3,   // color type 3 = indexed palette (required by Vita SceShell)
        0, 0, 0
    };
    AppendPngChunk(outPng, "IHDR", ihdr, 13);
    AppendPngChunk(outPng, "PLTE", &pal[0][0], static_cast<size_t>(nPal) * 3);
    AppendPngChunk(outPng, "IDAT", zbuf, static_cast<size_t>(zlen));
    STBIW_FREE(zbuf);
    AppendPngChunk(outPng, "IEND", nullptr, 0);
    return true;
}

#if defined(__vita__)
void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogLine(const char* fmt, ...) {
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fputs(buf, stderr);
    // Strip trailing newline for VitaLog::Note (reaches log.txt even in quiet player builds)
    size_t len = std::strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    if (len > 0) VitaLog::Note(buf);
}

bool AtomicWriteFile(const char* path, const uint8_t* data, size_t len, int* outErr = nullptr) {
    if (!path || !data || len < 16) return false;
    std::string tmp = std::string(path) + ".tmp";
    SceUID fd = sceIoOpen(tmp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        if (outErr) *outErr = static_cast<int>(fd);
        return false;
    }
    bool ok = true;
    for (size_t at = 0; at < len && ok;) {
        size_t n = std::min<size_t>(65536, len - at);
        int w = sceIoWrite(fd, data + at, static_cast<SceSize>(n));
        if (w != static_cast<int>(n)) {
            if (outErr) *outErr = w;
            ok = false;
        }
        at += n;
    }
    int cl = sceIoClose(fd);
    if (cl < 0) { if (outErr && ok) *outErr = cl; ok = false; }
    if (ok) {
        int rn = sceIoRename(tmp.c_str(), path);
        if (rn < 0) {
            sceIoRemove(path);
            rn = sceIoRename(tmp.c_str(), path);
            if (rn < 0) {
                if (outErr) *outErr = rn;
                ok = false;
            }
        }
    }
    if (!ok) sceIoRemove(tmp.c_str());
    return ok;
}

bool FileExistsVita(const char* path, uint64_t* outSize = nullptr) {
    SceIoStat st{};
    if (sceIoGetstat(path, &st) < 0) return false;
    if (outSize) *outSize = static_cast<uint64_t>(st.st_size);
    return true;
}

struct StampInfo {
    uint32_t ver = 0;
    char titleId[16] = {};
    uint64_t dmnSize = 0;
    uint64_t bnrSize = 0;
    uint32_t iconCrc = 0;
    bool valid = false;
};

StampInfo ReadStampFile(const char* path) {
    StampInfo info;
    std::vector<uint8_t> cur;
    if (!ReadFileBytes(path, cur, 512)) return info;
    std::string s(reinterpret_cast<const char*>(cur.data()), cur.size());
    unsigned ver = 0, crc = 0;
    unsigned long long dmn = 0, bnr = 0;
    if (std::sscanf(s.c_str(), "ART33_V%u %15s %llu %llu %x", &ver, info.titleId, &dmn, &bnr, &crc) == 5) {
        info.ver = ver;
        info.dmnSize = dmn;
        info.bnrSize = bnr;
        info.iconCrc = crc;
        info.valid = true;
    }
    return info;
}

std::string MakeStampText(const char* titleId, uint64_t dmnSize, uint64_t bnrSize, uint32_t iconCrc) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "ART33_V%u %s %llu %llu %08X\n", kFormatVersion, titleId,
                  static_cast<unsigned long long>(dmnSize), static_cast<unsigned long long>(bnrSize), iconCrc);
    return buf;
}

// Bump content-rev="N" -> content-rev="N+1" in a staged template.xml so SceShell treats it as a fresh revision.
std::vector<uint8_t> BumpTemplateContentRev(const std::vector<uint8_t>& tpl) {
    std::string xml(reinterpret_cast<const char*>(tpl.data()), tpl.size());
    const std::string key = "content-rev=\"";
    size_t pos = xml.find(key);
    if (pos != std::string::npos) {
        size_t valStart = pos + key.size();
        size_t valEnd = xml.find('"', valStart);
        if (valEnd != std::string::npos && valEnd > valStart && valEnd - valStart < 8) {
            int rev = std::atoi(xml.substr(valStart, valEnd - valStart).c_str());
            if (rev > 0 && rev < 9999) {
                xml.replace(valStart, valEnd - valStart, std::to_string(rev + 1));
            }
        }
    }
    return std::vector<uint8_t>(xml.begin(), xml.end());
}

const char* CurrentTitleId() {
#ifdef NP_TITLE_ID
    return NP_TITLE_ID;
#else
    return "MSCN00003";
#endif
}

bool LiveAreaModeForce() {
    const char* e = std::getenv("MSC_LIVEAREA_ART");
    return e && std::strcmp(e, "force") == 0;
}

bool LiveAreaModeOff() {
    const char* e = std::getenv("MSC_LIVEAREA_ART");
    return e && e[0] == '0';
}

void RemoveDirTree(const char* dir) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent ent{};
    while (sceIoDread(d, &ent) > 0) {
        if (ent.d_name[0] != '.' || (ent.d_name[1] != 0 && (ent.d_name[1] != '.' || ent.d_name[2] != 0))) {
            std::string child = std::string(dir) + "/" + ent.d_name;
            if (SCE_S_ISDIR(ent.d_stat.st_mode)) RemoveDirTree(child.c_str());
            else sceIoRemove(child.c_str());
        }
        std::memset(&ent, 0, sizeof ent);
    }
    sceIoDclose(d);
    sceIoRmdir(dir);
}

int InvokePromoterLA(const char* titleId, const char* path) {
    ScePromoterUtilityLAUpdate args{};
    std::snprintf(args.titleid, sizeof args.titleid, "%s", titleId);
    std::snprintf(args.path, sizeof args.path, "%s", path);
    int upd = scePromoterUtilityUpdateLiveArea(&args);
    if (upd == 0) {
        int state = 1;
        for (int i = 0; i < 100 && state != 0; ++i) {
            if (scePromoterUtilityGetState(&state) < 0) break;
            if (state != 0) sceKernelDelayThread(50000);
        }
        int res = 0;
        if (scePromoterUtilityGetResult(&res) == 0 && res < 0) upd = res;
    }
    return upd;
}

int TryPromoterUpdateLiveArea(const char* titleId, const char* stageDir, const char* contentsDir) {
    // Load PAF + ScePromoterUtil using the same VitaShell argument block proven in vita_lab.cpp.
    unsigned pafArgs[] = {0x180000, 0xffffffff, 0xffffffff, 1, 0xffffffff, 0xffffffff};
    int pafResult = -1;
    SceSysmoduleOpt opt = {sizeof(SceSysmoduleOpt), &pafResult, {-1, -1}};
    int paf = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(pafArgs), pafArgs, &opt);
    int mod = paf >= 0 ? sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL) : -1;
    int init = mod >= 0 ? scePromoterUtilityInit() : -1;
    int upd = -1;
    int upd2 = -1;
    if (init >= 0) {
        upd = InvokePromoterLA(titleId, stageDir);
        if (upd < 0 && contentsDir && *contentsDir) {
            upd2 = InvokePromoterLA(titleId, contentsDir);
            if (upd2 == 0) upd = 0;
        }
        scePromoterUtilityExit();
    }
    if (mod >= 0) sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (paf >= 0) {
        SceSysmoduleOpt uopt{};
        sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, nullptr, &uopt);
    }
    LogLine("[liveart] promoter update (%s): paf 0x%08X mod 0x%08X init 0x%08X upd 0x%08X upd2 0x%08X\n",
            titleId, static_cast<unsigned>(paf), static_cast<unsigned>(mod),
            static_cast<unsigned>(init), static_cast<unsigned>(upd), static_cast<unsigned>(upd2));
    return upd;
}
#endif

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool IsGenericIconBytes(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    uint32_t crc = Crc32(data, len);
    return (len == kGenericIconLen && crc == kGenericIconCrc) ||
           (len == kGenericTestIconLen && crc == kGenericTestIconCrc);
}

bool BuildFromFilesDir(const char* filesDir, BuiltArt& out, char* why, size_t whyBytes, const volatile bool* cancel) {
    if (why && whyBytes) why[0] = 0;
    out = BuiltArt{};
    if (!filesDir || !*filesDir) { SetWhy(why, whyBytes, "no files folder"); return false; }

    std::string bnrPath, dmnPath, supPath, fontPath;
    if (!FindFile(filesDir, "opening.bnr", bnrPath)) { SetWhy(why, whyBytes, "opening.bnr not found"); return false; }
    if (!FindFile(filesDir, "Art/fe/mainui.dmn", dmnPath)) { SetWhy(why, whyBytes, "Art/fe/mainui.dmn not found"); return false; }

    DiscAssets assets;
    {
        std::vector<uint8_t> bnr;
        if (!ReadFileBytes(bnrPath, bnr)) { SetWhy(why, whyBytes, "cannot read opening.bnr"); return false; }
        if (!LoadBannerAssets(bnr, assets.logo, assets.sky)) { SetWhy(why, whyBytes, "cannot decode opening.bnr logo"); return false; }
    }
    if (Cancelled(cancel)) { SetWhy(why, whyBytes, "cancelled"); return false; }
    {
        std::vector<uint8_t> dmn;
        if (!ReadFileBytes(dmnPath, dmn)) { SetWhy(why, whyBytes, "cannot read Art/fe/mainui.dmn"); return false; }
        Img key = LoadBundleTexture(dmn, kKeyArtHash);
        if (key.empty()) { SetWhy(why, whyBytes, "key art missing from mainui.dmn"); return false; }
        assets.art = ToRgb(key);
        assets.ball = LoadBundleTexture(dmn, kBallArtHash);
        assets.sunburst = LoadBundleTexture(dmn, kSunburstHash);
        assets.iceburst = LoadBundleTexture(dmn, kIceburstHash);
        assets.boltA = LoadBundleTexture(dmn, kBoltAHash);
        assets.boltB = LoadBundleTexture(dmn, kBoltBHash);
        assets.boltC = LoadBundleTexture(dmn, kBoltCHash);
        assets.sparks = LoadBundleTexture(dmn, kSparksHash);
    }
    if (Cancelled(cancel)) { SetWhy(why, whyBytes, "cancelled"); return false; }
    if (FindFile(filesDir, "Art/fe/superabilityui.res", supPath)) {
        std::vector<uint8_t> sup;
        if (ReadFileBytes(supPath, sup)) {
            assets.mario = LoadBundleTexture(sup, kMarioHash);
            assets.bowser = LoadBundleTexture(sup, kBowserHash);
        }
    }
    if (FindFile(filesDir, "Art/fe/fonts/engfontheading36.res", fontPath)) {
        ReadFileBytes(fontPath, assets.fontBlob);
    }
    if (Cancelled(cancel)) { SetWhy(why, whyBytes, "cancelled"); return false; }

    // Generate & encode sequentially so peak heap stays low (~5 MiB).
    {
        Img icon = MakeIcon(assets);
        if (!EncodeIndexedPng(icon, out.icon0)) { SetWhy(why, whyBytes, "icon0 encode failed"); return false; }
    }
    if (Cancelled(cancel)) { SetWhy(why, whyBytes, "cancelled"); return false; }
    {
        Img st = MakeStartup(assets);
        if (!EncodeIndexedPng(st, out.startup)) { SetWhy(why, whyBytes, "startup encode failed"); return false; }
    }
    assets.fontBlob.clear();
    assets.fontBlob.shrink_to_fit();
    if (Cancelled(cancel)) { SetWhy(why, whyBytes, "cancelled"); return false; }
    {
        Img bg = MakeStadiumScene(assets, 840, 500, 364, 24, 326, 365, 0.72, true);
        if (!EncodeIndexedPng(bg, out.bg)) { SetWhy(why, whyBytes, "bg encode failed"); return false; }
    }
    if (Cancelled(cancel)) { SetWhy(why, whyBytes, "cancelled"); return false; }
    {
        Img pic0 = MakeStadiumScene(assets, 960, 544, 460, 64, 356, 430, 0.60, false);
        if (!EncodeIndexedPng(pic0, out.pic0)) { SetWhy(why, whyBytes, "pic0 encode failed"); return false; }
    }
    return true;
}

#if defined(__vita__)
bool NeedsApply() {
    if (LiveAreaModeOff()) return false;
    const bool force = LiveAreaModeForce();
#if !defined(NP_PLAYER)
    if (!force) return false;
#endif
    const char* root = NpGameRootPath();
    if (!root || !*root) return false;
    std::string filesDir = std::string(root) + "/files";
    std::string dmnPath, bnrPath;
    if (!FindFile(filesDir.c_str(), "Art/fe/mainui.dmn", dmnPath) || !FindFile(filesDir.c_str(), "opening.bnr", bnrPath)) return false;
    uint64_t dmnSize = 0, bnrSize = 0;
    if (!FileExistsVita(dmnPath.c_str(), &dmnSize) || !FileExistsVita(bnrPath.c_str(), &bnrSize)) return false;

    const char* titleId = CurrentTitleId();
    if (!force) {
        char metaIcon[160], metaBg[160], stampPath[200];
        std::snprintf(metaIcon, sizeof metaIcon, "ur0:appmeta/%s/icon0.png", titleId);
        std::snprintf(metaBg, sizeof metaBg, "ur0:appmeta/%s/livearea/contents/bg.png", titleId);
        std::snprintf(stampPath, sizeof stampPath, "%s/cache/livearea_art.stamp", VitaPlatform::kDataDevicePath);

        std::vector<uint8_t> curIcon;
        if (ReadFileBytes(metaIcon, curIcon, 512u * 1024u)) {
            // Fresh unbranded install or unbranded VPK upgrade: ur0:appmeta has the generic CBOC icon -> always brand!
            if (IsGenericIconBytes(curIcon.data(), curIcon.size())) return true;
            // Already non-generic: only regenerate if WE created it (CRC matches our stamp) and version/disc/bg changed.
            uint32_t curCrc = Crc32(curIcon.data(), curIcon.size());
            StampInfo st = ReadStampFile(stampPath);
            if (!st.valid || st.iconCrc != curCrc) return false;   // custom user/installer artwork: never overwrite
            if (st.ver == kFormatVersion && std::strcmp(st.titleId, titleId) == 0 &&
                st.dmnSize == dmnSize && st.bnrSize == bnrSize && FileExistsVita(metaBg)) {
                return false;   // already up to date
            }
            return true;
        }
        // Fallback if ur0:appmeta/<TITLE_ID>/icon0.png is absent/unreadable: check ux0:app or app0 shipped generic icon.
        char appIconPath[160];
        std::snprintf(appIconPath, sizeof appIconPath, "ux0:app/%s/sce_sys/icon0.png", titleId);
        std::vector<uint8_t> appIcon;
        if (!ReadFileBytes(appIconPath, appIcon, 512u * 1024u) && !ReadFileBytes("app0:sce_sys/icon0.png", appIcon, 512u * 1024u)) {
            LogLine("[liveart] skip: cannot read %s or %s or app0:sce_sys/icon0.png\n", metaIcon, appIconPath);
            return false;
        }
        if (!IsGenericIconBytes(appIcon.data(), appIcon.size())) return false;
    }
    return true;
}

void ApplyIfNeeded(const volatile bool* cancel) {
    if (!NeedsApply() || Cancelled(cancel)) return;
    const char* root = NpGameRootPath();
    const char* titleId = CurrentTitleId();
    std::string filesDir = std::string(root) + "/files";
    std::string dmnPath, bnrPath;
    uint64_t dmnSize = 0, bnrSize = 0;
    if (!FindFile(filesDir.c_str(), "Art/fe/mainui.dmn", dmnPath) || !FindFile(filesDir.c_str(), "opening.bnr", bnrPath)) return;
    FileExistsVita(dmnPath.c_str(), &dmnSize);
    FileExistsVita(bnrPath.c_str(), &bnrSize);

    const uint64_t t0Us = sceKernelGetProcessTimeWide();
    LogLine("[liveart] starting first-boot branding for %s\n", titleId);
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    char why[128] = "";
    BuiltArt art;
    if (!BuildFromFilesDir(filesDir.c_str(), art, why, sizeof why, cancel)) {
        LogLine("[liveart] skipped: %s\n", why[0] ? why : "build failed");
        return;
    }
    const unsigned buildMs = static_cast<unsigned>((sceKernelGetProcessTimeWide() - t0Us) / 1000u);
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

    // 1. Write atomically to ur0:appmeta/<TITLE_ID>/ (where SceShell loads bubble & LiveArea)
    char metaRoot[128], metaLa[160], metaC[180], p[220];
    std::snprintf(metaRoot, sizeof metaRoot, "ur0:appmeta/%s", titleId);
    std::snprintf(metaLa, sizeof metaLa, "%s/livearea", metaRoot);
    std::snprintf(metaC, sizeof metaC, "%s/contents", metaLa);
    sceIoMkdir(metaRoot, 0777);
    sceIoMkdir(metaLa, 0777);
    sceIoMkdir(metaC, 0777);

    int metaErr = 0, appErr = 0;
    auto writeMetaAll = [&]() -> int {
        int ok = 0;
        std::snprintf(p, sizeof p, "%s/icon0.png", metaRoot);
        if (AtomicWriteFile(p, art.icon0.data(), art.icon0.size(), &metaErr)) ++ok;
        std::snprintf(p, sizeof p, "%s/pic0.png", metaRoot);
        if (AtomicWriteFile(p, art.pic0.data(), art.pic0.size(), &metaErr)) ++ok;
        std::snprintf(p, sizeof p, "%s/bg.png", metaC);
        if (AtomicWriteFile(p, art.bg.data(), art.bg.size(), &metaErr)) ++ok;
        std::snprintf(p, sizeof p, "%s/startup.png", metaC);
        if (AtomicWriteFile(p, art.startup.data(), art.startup.size(), &metaErr)) ++ok;
        return ok;
    };
    int metaOk = writeMetaAll();

    // 2. Also update ux0:app/<TITLE_ID>/sce_sys/ (best-effort; preserves art across VitaShell Refresh LiveArea)
    int appOk = 0;
    char appSys[128], appC[180];
    std::snprintf(appSys, sizeof appSys, "ux0:app/%s/sce_sys", titleId);
    std::snprintf(appC, sizeof appC, "%s/livearea/contents", appSys);
    std::snprintf(p, sizeof p, "%s/icon0.png", appSys);
    if (AtomicWriteFile(p, art.icon0.data(), art.icon0.size(), &appErr)) ++appOk;
    std::snprintf(p, sizeof p, "%s/pic0.png", appSys);
    if (AtomicWriteFile(p, art.pic0.data(), art.pic0.size(), &appErr)) ++appOk;
    std::snprintf(p, sizeof p, "%s/bg.png", appC);
    if (AtomicWriteFile(p, art.bg.data(), art.bg.size(), &appErr)) ++appOk;
    std::snprintf(p, sizeof p, "%s/startup.png", appC);
    if (AtomicWriteFile(p, art.startup.data(), art.startup.size(), &appErr)) ++appOk;

    // 3. Ask ScePromoterUtil to notify SceShell so LiveArea updates immediately
    char stage[180], stageLa[200], stageC[220];
    std::snprintf(stage, sizeof stage, "%s/cache/la_update", VitaPlatform::kDataDevicePath);
    std::snprintf(stageLa, sizeof stageLa, "%s/livearea", stage);
    std::snprintf(stageC, sizeof stageC, "%s/contents", stageLa);
    sceIoMkdir("ux0:/data", 0777);
    sceIoMkdir(VitaPlatform::kDataDevicePath, 0777);
    char cacheDir[160];
    std::snprintf(cacheDir, sizeof cacheDir, "%s/cache", VitaPlatform::kDataDevicePath);
    sceIoMkdir(cacheDir, 0777);
    sceIoMkdir(stage, 0777);
    sceIoMkdir(stageLa, 0777);
    sceIoMkdir(stageC, 0777);

    std::vector<uint8_t> tpl;
    bool staged = ReadFileBytes("app0:sce_sys/livearea/contents/template.xml", tpl, 65536);
    if (staged) {
        std::vector<uint8_t> bumped = BumpTemplateContentRev(tpl);
        std::snprintf(p, sizeof p, "%s/template.xml", stageC);
        staged = AtomicWriteFile(p, bumped.data(), bumped.size());
        std::snprintf(p, sizeof p, "%s/bg.png", stageC);
        staged = staged && AtomicWriteFile(p, art.bg.data(), art.bg.size());
        std::snprintf(p, sizeof p, "%s/startup.png", stageC);
        staged = staged && AtomicWriteFile(p, art.startup.data(), art.startup.size());
    }
    int promRc = -1;
    if (staged && !Cancelled(cancel)) promRc = TryPromoterUpdateLiveArea(titleId, stage, stageC);
    RemoveDirTree(stage);

    // Ensure ur0:appmeta still holds all 4 generated PNGs even if Promoter moved/rewrote contents.
    if (promRc == 0) metaOk = writeMetaAll();

    const unsigned ms = static_cast<unsigned>((sceKernelGetProcessTimeWide() - t0Us) / 1000u);
    LogLine("[liveart] generated in %u ms (build %u ms): icon0=%u bg=%u startup=%u pic0=%u B; ur0:appmeta=%d/4 (err=0x%08X) ux0:app=%d/4 (err=0x%08X) promoter=0x%08X\n",
            ms, buildMs, static_cast<unsigned>(art.icon0.size()), static_cast<unsigned>(art.bg.size()),
            static_cast<unsigned>(art.startup.size()), static_cast<unsigned>(art.pic0.size()),
            metaOk, static_cast<unsigned>(metaErr), appOk, static_cast<unsigned>(appErr), static_cast<unsigned>(promRc));

    if (metaOk == 4) {
        char stampPath[200];
        std::snprintf(stampPath, sizeof stampPath, "%s/cache/livearea_art.stamp", VitaPlatform::kDataDevicePath);
        uint32_t iconCrc = Crc32(art.icon0.data(), art.icon0.size());
        std::string stamp = MakeStampText(titleId, dmnSize, bnrSize, iconCrc);
        AtomicWriteFile(stampPath, reinterpret_cast<const uint8_t*>(stamp.data()), stamp.size());
    }
}
#endif

}  // namespace VitaLiveAreaArt
