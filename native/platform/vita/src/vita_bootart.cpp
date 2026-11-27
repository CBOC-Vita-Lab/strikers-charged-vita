// The boot loading screen's own picture, made from the player's game files. See vita_bootart.h.
//
// Where Pillow's arithmetic matters (the result must be byte for byte the Python tool's) each function says which of Pillow's it follows:
//   Resample.c  precompute_coeffs / normalize_coeffs_8bpc / ImagingResampleHorizontal_8bpc / Vertical_8bpc / ImagingResampleInner
//   BoxBlur.c   ImagingLineBoxBlur8 / 32, ImagingGaussianBlur (three box passes, the radius of _gaussian_blur_radius)
//   Paste.c     paste_mask_L (BLEND), AlphaComposite.c (7 bit coefficients), Convert.c rgbA2rgba / rgba2rgbA (premultiply and back)
//   Fill.c      ImagingFillRadialGradient
#include "vita_bootart.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace VitaBootArt {
namespace {

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Bookkeeping: the reason of a failure, the cancel flag, the heap held at one moment
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct Ctx {
    char* why;
    size_t whyBytes;
    const volatile bool* cancel;
    size_t current = 0, peak = 0;
    bool failed = false;

    bool Fail(const char* message) {
        if (!failed) {
            failed = true;
            if (why && whyBytes) std::snprintf(why, whyBytes, "%s", message);
        }
        return false;
    }
    bool Stop() {   // true (and failed) when the caller asked to stop
        if (cancel && *cancel) { Fail("cancelled"); return true; }
        return failed;
    }
};

// A heap block that is counted and given back when it goes out of scope.
struct Buf {
    Ctx* ctx = nullptr;
    uint8_t* p = nullptr;
    size_t n = 0;
    Buf() = default;
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
    ~Buf() { Free(); }
    bool Alloc(Ctx& c, size_t bytes, bool zero = false) {
        Free();
        if (bytes == 0) bytes = 1;
        p = static_cast<uint8_t*>(zero ? std::calloc(bytes, 1) : std::malloc(bytes));
        if (!p) return c.Fail("no memory");
        ctx = &c;
        n = bytes;
        c.current += bytes;
        if (c.current > c.peak) c.peak = c.current;
        return true;
    }
    void Free() {
        if (p) { std::free(p); ctx->current -= n; }
        p = nullptr;
        n = 0;
    }
};

inline uint32_t Be32(const uint8_t* b) { return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]; }
inline uint32_t Be16(const uint8_t* b) { return (uint32_t(b[0]) << 8) | b[1]; }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// GX textures (big-endian, tiled): the formats the art uses (brand-from-disc.py decode_gx)
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct Px { uint8_t r, g, b, a; };

inline int Expand(int v, int bits) {
    if (bits == 3) return (v << 5) | (v << 2) | (v >> 1);
    if (bits == 4) return v * 17;
    if (bits == 5) return (v << 3) | (v >> 2);
    return (v << 2) | (v >> 4);
}
inline Px Rgb565(uint32_t v) { return {uint8_t(Expand((v >> 11) & 31, 5)), uint8_t(Expand((v >> 5) & 63, 6)), uint8_t(Expand(v & 31, 5)), 255}; }
inline Px Rgb5a3(uint32_t v) {
    if (v & 0x8000) return {uint8_t(Expand((v >> 10) & 31, 5)), uint8_t(Expand((v >> 5) & 31, 5)), uint8_t(Expand(v & 31, 5)), 255};
    return {uint8_t(Expand((v >> 8) & 15, 4)), uint8_t(Expand((v >> 4) & 15, 4)), uint8_t(Expand(v & 15, 4)), uint8_t(Expand((v >> 12) & 7, 3))};
}

struct BlockSize { int bw, bh, bpp; };
bool BlockOf(int fmt, BlockSize& b) {
    switch (fmt) {
        case 0: b = {8, 8, 4}; return true;
        case 1: b = {8, 4, 8}; return true;
        case 2: b = {8, 4, 8}; return true;
        case 3: b = {4, 4, 16}; return true;
        case 4: b = {4, 4, 16}; return true;
        case 5: b = {4, 4, 16}; return true;
        case 6: b = {4, 4, 32}; return true;
        case 8: b = {8, 8, 4}; return true;
        case 9: b = {8, 4, 8}; return true;
        case 10: b = {4, 4, 16}; return true;
        case 14: b = {8, 8, 4}; return true;
        default: return false;
    }
}
uint64_t LevelSize(int w, int h, int fmt) {
    BlockSize b;
    if (!BlockOf(fmt, b)) return 0;
    return uint64_t((w + b.bw - 1) / b.bw) * ((h + b.bh - 1) / b.bh) * b.bw * b.bh * b.bpp / 8;
}

struct Palette {
    Px e[256];
    int count = 0;   // entries the file has (capped at 256: an index is a byte)
};

// One level of texel data -> RGBA, w x h (the texels past the edge of a tile that sticks out are dropped, as the tool's crop does). The caller
// has checked that data holds LevelSize bytes.
bool DecodeGx(Ctx& c, int fmt, int w, int h, const uint8_t* data, const Palette* pal, uint8_t* out) {
    BlockSize bs;
    if (!BlockOf(fmt, bs)) return c.Fail("texture format not handled");
    const int pw = (w + bs.bw - 1) / bs.bw * bs.bw, ph = (h + bs.bh - 1) / bs.bh * bs.bh;
    auto put = [&](int x, int y, Px px) {
        if (x < w && y < h) {
            uint8_t* o = out + (static_cast<size_t>(y) * w + x) * 4;
            o[0] = px.r; o[1] = px.g; o[2] = px.b; o[3] = px.a;
        }
    };
    size_t o = 0;
    for (int ty = 0; ty < ph; ty += bs.bh) {
        for (int tx = 0; tx < pw; tx += bs.bw) {
            if (fmt == 14) {
                for (int sy = 0; sy < 8; sy += 4) {
                    for (int sx = 0; sx < 8; sx += 4) {
                        const uint32_t c0 = Be16(data + o), c1 = Be16(data + o + 2);
                        const uint32_t bits = Be32(data + o + 4);
                        o += 8;
                        const Px a = Rgb565(c0), b = Rgb565(c1);
                        Px cs[4];
                        cs[0] = a;
                        cs[1] = b;
                        if (c0 > c1) {
                            cs[2] = {uint8_t((2 * a.r + b.r) / 3), uint8_t((2 * a.g + b.g) / 3), uint8_t((2 * a.b + b.b) / 3), 255};
                            cs[3] = {uint8_t((a.r + 2 * b.r) / 3), uint8_t((a.g + 2 * b.g) / 3), uint8_t((a.b + 2 * b.b) / 3), 255};
                        } else {
                            cs[2] = {uint8_t((a.r + b.r) / 2), uint8_t((a.g + b.g) / 2), uint8_t((a.b + b.b) / 2), 255};
                            cs[3] = {0, 0, 0, 0};
                        }
                        for (int i = 0; i < 16; ++i) put(tx + sx + i % 4, ty + sy + i / 4, cs[(bits >> (30 - 2 * i)) & 3]);
                    }
                }
            } else if (fmt == 6) {
                for (int i = 0; i < 16; ++i) put(tx + i % 4, ty + i / 4, {data[o + 2 * i + 1], data[o + 32 + 2 * i], data[o + 33 + 2 * i], data[o + 2 * i]});
                o += 64;
            } else if (fmt == 4 || fmt == 5) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const uint32_t v = Be16(data + o);
                        put(tx + x, ty + y, fmt == 4 ? Rgb565(v) : Rgb5a3(v));
                        o += 2;
                    }
                }
            } else if (fmt == 3) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const uint8_t a = data[o], i = data[o + 1];
                        o += 2;
                        put(tx + x, ty + y, {i, i, i, a});
                    }
                }
            } else if (fmt == 0 || fmt == 8) {
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; x += 2) {
                        const uint8_t v = data[o++];
                        const int nib[2] = {v >> 4, v & 15};
                        for (int k = 0; k < 2; ++k) {
                            if (fmt == 0) {
                                const uint8_t g = uint8_t(nib[k] * 17);
                                put(tx + x + k, ty + y, {g, g, g, g});
                            } else {
                                if (!pal || nib[k] >= pal->count) return c.Fail("texture palette too short");
                                put(tx + x + k, ty + y, pal->e[nib[k]]);
                            }
                        }
                    }
                }
            } else if (fmt == 1 || fmt == 2 || fmt == 9) {
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        const uint8_t n = data[o++];
                        if (fmt == 1) put(tx + x, ty + y, {n, n, n, n});
                        else if (fmt == 2) { const uint8_t g = uint8_t((n & 15) * 17); put(tx + x, ty + y, {g, g, g, uint8_t((n >> 4) * 17)}); }
                        else {
                            if (!pal) return c.Fail("texture needs a palette");
                            put(tx + x, ty + y, n < pal->count ? pal->e[n] : Px{255, 0, 255, 255});
                        }
                    }
                }
            } else {
                return c.Fail("texture format not handled");
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kArtHash = 0x57399903;                 // fe/screens/images: the start screen's 512 x 512 picture
constexpr const char* kLogoMember = "arc/timg/logo_UK_Banner.tpl";
constexpr uint64_t kMaxBanner = 16u << 20;

bool ReadAt(Ctx& c, const Source& s, uint64_t off, void* dst, size_t len) {
    if (len == 0) return true;
    if (off > s.size || len > s.size - off || !s.read || !s.read(s.context, off, dst, len)) return c.Fail("a game file could not be read");
    return true;
}

// The start-screen picture of a BundleFile: RGB, w x h (alpha dropped as .convert("RGB") does).
bool BundlePicture(Ctx& c, const Source& bundle, uint32_t wanted, Buf& rgb, int& w, int& h) {
    uint8_t head[16];
    if (!ReadAt(c, bundle, 0, head, 16)) return false;
    const uint64_t sector = Be32(head), count = Be32(head + 4), directory = Be32(head + 8);
    if (sector == 0 || sector > (1u << 20) || count > (1u << 20)) return c.Fail("mainui.dmn: not a bundle");
    uint64_t block = 0;
    bool found = false;
    for (uint64_t i = 0; i < count && !found;) {
        uint8_t rows[12 * 32];
        const size_t n = static_cast<size_t>(std::min<uint64_t>(32, count - i));
        if (!ReadAt(c, bundle, directory * sector + i * 12, rows, n * 12)) return false;
        for (size_t k = 0; k < n; ++k, ++i) {
            if (Be32(rows + 12 * k) == wanted) { block = Be32(rows + 12 * k + 4); found = true; break; }
        }
    }
    if (!found) return c.Fail("mainui.dmn: the start-screen picture is not in it");
    const uint64_t off = block * sector;
    uint8_t th[0x20];
    if (!ReadAt(c, bundle, off, th, sizeof th)) return false;
    const uint32_t levels = Be32(th), gameFmt = Be32(th + 4), entries = Be32(th + 0x14);
    w = static_cast<int>(Be16(th + 0x0E));
    h = static_cast<int>(Be16(th + 0x10));
    static const int kGameToGx[9] = {4, 5, 14, 6, 1, 0, 1, 3, 9};
    if (gameFmt > 8) return c.Fail("mainui.dmn: unknown texture format");
    const int fmt = kGameToGx[gameFmt];
    if (w < 1 || h < 1 || w > 2048 || h > 2048 || levels > 32) return c.Fail("mainui.dmn: odd texture size");
    // the art needs the crop (0, 40) .. (w * 2 / 5, 40 + h * 3 / 5)
    if (w < 8 || 40 + h * 3 / 5 > h) return c.Fail("mainui.dmn: the picture is too small");
    const uint64_t bytes = LevelSize(w, h, fmt);
    Buf data;
    if (!data.Alloc(c, static_cast<size_t>(bytes))) return false;
    if (!ReadAt(c, bundle, off + 0x20, data.p, static_cast<size_t>(bytes))) return false;
    Palette pal;
    const Palette* palette = nullptr;
    if (gameFmt == 8) {
        uint64_t total = 0;
        int lw = w, lh = h;
        for (uint32_t i = 0; i < levels; ++i) { total += static_cast<uint64_t>(lw) * lh; lw = std::max(lw / 2, 1); lh = std::max(lh / 2, 1); }
        const int n = static_cast<int>(std::min<uint32_t>(entries, 256));
        uint8_t raw[512];
        if (!ReadAt(c, bundle, off + 0x20 + total, raw, static_cast<size_t>(n) * 2)) return false;
        for (int k = 0; k < n; ++k) pal.e[k] = Rgb5a3(Be16(raw + 2 * k));
        pal.count = n;
        palette = &pal;
    }
    Buf rgba;
    if (!rgba.Alloc(c, static_cast<size_t>(w) * h * 4)) return false;
    if (!DecodeGx(c, fmt, w, h, data.p, palette, rgba.p)) return false;
    data.Free();
    // RGBA -> RGB in place (the write index never passes the read index)
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) { rgba.p[3 * i] = rgba.p[4 * i]; rgba.p[3 * i + 1] = rgba.p[4 * i + 1]; rgba.p[3 * i + 2] = rgba.p[4 * i + 2]; }
    rgb.ctx = &c;   // hand the block over (the counted size is the RGBA block's: a little more than held, which is the safe side)
    rgb.p = rgba.p;
    rgb.n = rgba.n;
    rgba.p = nullptr;
    rgba.n = 0;
    return true;
}

const uint8_t* FindMagic(const uint8_t* b, size_t n, size_t from = 0) {   // blob.find(b"U\xaa8-")
    static const uint8_t m[4] = {'U', 0xAA, '8', '-'};
    for (size_t i = from; i + 4 <= n; ++i)
        if (b[i] == m[0] && b[i + 1] == m[1] && b[i + 2] == m[2] && b[i + 3] == m[3]) return b + i;
    return nullptr;
}

// The member `want` of the U8 archive that starts at b + base (the last one of that name, as a dict would keep it).
bool U8Find(Ctx& c, const uint8_t* b, size_t n, size_t base, const char* want, size_t& off, size_t& size) {
    if (base + 12 > n || std::memcmp(b + base, "U\xaa" "8-", 4) != 0) return c.Fail("not a U8 archive");
    const size_t root = Be32(b + base + 4);
    if (base + root + 12 > n) return c.Fail("U8: cut short");
    const size_t count = Be32(b + base + root + 8);
    if (count == 0 || count > 100000 || base + root + count * 12 > n) return c.Fail("U8: cut short");
    const size_t names = base + root + count * 12;
    char path[512];
    size_t lens[40];
    size_t ends[41];
    int depth = 0, nends = 1;
    ends[0] = count;
    path[0] = 0;
    bool found = false;
    for (size_t i = 1; i < count; ++i) {
        const uint8_t* e = b + base + root + i * 12;
        const uint32_t word = Be32(e), noff = Be32(e + 4), nsize = Be32(e + 8);
        const size_t at = names + (word & 0xFFFFFF);
        if (at >= n) return c.Fail("U8: a name is outside the file");
        const void* nul = std::memchr(b + at, 0, n - at);
        if (!nul) return c.Fail("U8: a name has no end");
        const size_t nlen = static_cast<const uint8_t*>(nul) - (b + at);
        while (nends > 0 && i >= ends[nends - 1]) {
            --nends;
            if (depth > 0) --depth;
        }
        const size_t cur = depth ? lens[depth - 1] : 0;   // length of the joined path of the open directories
        if (cur + 1 + nlen + 1 > sizeof path || depth >= 39 || nends >= 40) return c.Fail("U8: too deep");
        if ((word >> 24) == 1) {
            if (depth) path[cur] = '/';
            const size_t start = depth ? cur + 1 : 0;
            std::memcpy(path + start, b + at, nlen);
            path[start + nlen] = 0;
            lens[depth] = start + nlen;
            ++depth;
            ends[nends++] = nsize;
        } else {
            char full[512];
            size_t k = 0;
            if (depth) { std::memcpy(full, path, cur); k = cur; full[k++] = '/'; }
            std::memcpy(full + k, b + at, nlen);
            full[k + nlen] = 0;
            if (std::strcmp(full, want) == 0) {
                if (uint64_t(base) + noff + nsize > n) return c.Fail("U8: a member is cut short");
                off = base + noff;
                size = nsize;
                found = true;
            }
        }
    }
    return found ? true : c.Fail("U8: the member is not in the archive");
}

// Nintendo LZ77 type 0x10 ('LZ77' + 0x10 + a 24 bit size, optional prefix).
bool Lz77(Ctx& c, const uint8_t* d, size_t n, Buf& out, size_t& outSize) {
    if (n >= 4 && std::memcmp(d, "LZ77", 4) == 0) { d += 4; n -= 4; }
    if (n < 4 || d[0] != 0x10) return c.Fail("opening.bnr: unsupported compression");
    const size_t size = size_t(d[1]) | (size_t(d[2]) << 8) | (size_t(d[3]) << 16);
    if (!out.Alloc(c, size)) return false;
    size_t len = 0, p = 4;
    while (len < size) {
        if (p >= n) return c.Fail("opening.bnr: compressed data cut short");
        const unsigned flags = d[p++];
        for (int k = 0; k < 8 && len < size; ++k) {
            if (flags & (0x80u >> k)) {
                if (p + 2 > n) return c.Fail("opening.bnr: compressed data cut short");
                const unsigned a = d[p], b = d[p + 1];
                p += 2;
                const size_t length = (a >> 4) + 3, disp = (((a & 15) << 8) | b) + 1;
                if (disp > len) return c.Fail("opening.bnr: bad back reference");
                for (size_t i = 0; i < length && len < size; ++i, ++len) out.p[len] = out.p[len - disp];
            } else {
                if (p >= n) return c.Fail("opening.bnr: compressed data cut short");
                out.p[len++] = d[p++];
            }
        }
    }
    outSize = size;
    return true;
}

// The logo: RGBA, trimmed to the pixels with alpha above 12 (brand-from-disc.py banner_member, tpl_image, trim). The pixels stay in the full
// decoded image: `pixels` points at the trimmed corner and `stride` is the full row.
struct Logo {
    Buf full;
    const uint8_t* pixels = nullptr;
    int w = 0, h = 0;
    size_t stride = 0;
};

bool LoadLogo(Ctx& c, const Source& banner, Logo& logo) {
    if (banner.size < 64 || banner.size > kMaxBanner) return c.Fail("opening.bnr: odd size");
    Buf blob;
    if (!blob.Alloc(c, static_cast<size_t>(banner.size))) return false;
    if (!ReadAt(c, banner, 0, blob.p, blob.n)) return false;
    const uint8_t* at = FindMagic(blob.p, blob.n);
    if (!at) return c.Fail("opening.bnr: no archive");
    size_t off = 0, size = 0;
    if (!U8Find(c, blob.p, blob.n, static_cast<size_t>(at - blob.p), "meta/banner.bin", off, size)) return false;
    const uint8_t* inner = blob.p + off;
    size_t innerSize = size;
    if (innerSize >= 4 && std::memcmp(inner, "IMD5", 4) == 0) {
        const size_t skip = std::min<size_t>(0x20, innerSize);
        inner += skip;
        innerSize -= skip;
    }
    Buf unpacked;
    if (innerSize >= 4 && std::memcmp(inner, "LZ77", 4) == 0) {
        size_t n = 0;
        if (!Lz77(c, inner, innerSize, unpacked, n)) return false;
        inner = unpacked.p;
        innerSize = n;
    }
    const uint8_t* at2 = FindMagic(inner, innerSize);
    if (!at2) return c.Fail("opening.bnr: no logo archive");
    if (!U8Find(c, inner, innerSize, static_cast<size_t>(at2 - inner), kLogoMember, off, size)) return false;
    const uint8_t* tpl = inner + off;
    const size_t tplSize = size;
    // TPL image 0
    if (tplSize < 16 || Be32(tpl) != 0x0020AF30 || Be32(tpl + 4) < 1) return c.Fail("opening.bnr: the logo is not a TPL image");
    const size_t table = Be32(tpl + 8);
    if (table + 8 > tplSize) return c.Fail("opening.bnr: the logo is cut short");
    const size_t imageAt = Be32(tpl + table), paletteAt = Be32(tpl + table + 4);
    if (imageAt + 12 > tplSize) return c.Fail("opening.bnr: the logo is cut short");
    const int h = static_cast<int>(Be16(tpl + imageAt)), w = static_cast<int>(Be16(tpl + imageAt + 2));
    const int fmt = static_cast<int>(Be32(tpl + imageAt + 4));
    const size_t dataAt = Be32(tpl + imageAt + 8);
    if (w < 1 || h < 1 || w > 2048 || h > 2048) return c.Fail("opening.bnr: odd logo size");
    BlockSize bs;
    if (!BlockOf(fmt, bs)) return c.Fail("opening.bnr: logo format not handled");
    const uint64_t bytes = LevelSize(w, h, fmt);
    if (uint64_t(dataAt) + bytes > tplSize) return c.Fail("opening.bnr: the logo is cut short");
    Palette pal;
    const Palette* palette = nullptr;
    if (paletteAt) {
        if (paletteAt + 12 > tplSize) return c.Fail("opening.bnr: the logo is cut short");
        const int entries = static_cast<int>(Be16(tpl + paletteAt));
        const uint32_t palFmt = Be32(tpl + paletteAt + 4), palData = Be32(tpl + paletteAt + 8);
        const int n = std::min(entries, 256);
        if (uint64_t(palData) + 2u * n > tplSize) return c.Fail("opening.bnr: the logo is cut short");
        for (int k = 0; k < n; ++k) {
            const uint32_t v = Be16(tpl + palData + 2 * k);
            pal.e[k] = palFmt == 1 ? Rgb565(v) : Rgb5a3(v);
        }
        pal.count = n;
        palette = &pal;
    }
    if (!logo.full.Alloc(c, static_cast<size_t>(w) * h * 4)) return false;
    if (!DecodeGx(c, fmt, w, h, tpl + dataAt, palette, logo.full.p)) return false;
    // trim: the box of the pixels with alpha > 12 (the whole image when there are none)
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = logo.full.p + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            if (row[4 * x + 3] > 12) { x0 = std::min(x0, x); x1 = std::max(x1, x + 1); y0 = std::min(y0, y); y1 = std::max(y1, y + 1); }
        }
    }
    if (x1 <= x0 || y1 <= y0) { x0 = 0; y0 = 0; x1 = w; y1 = h; }
    logo.pixels = logo.full.p + (static_cast<size_t>(y0) * w + x0) * 4;
    logo.w = x1 - x0;
    logo.h = y1 - y0;
    logo.stride = static_cast<size_t>(w) * 4;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Pillow's resampling (Resample.c), 8 bits per channel
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum class Filter { Bicubic, Lanczos };
constexpr double kPi = 3.14159265358979323846;
constexpr int kPrecisionBits = 32 - 8 - 2;

double SincFilter(double x) {
    if (x == 0.0) return 1.0;
    x = x * kPi;
    return std::sin(x) / x;
}
double FilterValue(Filter f, double x) {
    if (f == Filter::Lanczos) {
        if (-3.0 <= x && x < 3.0) return SincFilter(x) * SincFilter(x / 3);
        return 0.0;
    }
    const double a = -0.5;
    if (x < 0.0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
    if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}

struct Coeffs {
    int ksize = 0;
    Buf bounds;   // int pairs (xmin, count)
    Buf kk;       // int32 normalised coefficients, ksize per output
    const int* B() const { return reinterpret_cast<const int*>(bounds.p); }
    const int32_t* K() const { return reinterpret_cast<const int32_t*>(kk.p); }
};

// precompute_coeffs + normalize_coeffs_8bpc for the outputs [o0, o1) of an outSize wide result (box = the whole input).
bool Precompute(Ctx& c, int inSize, int outSize, Filter f, int o0, int o1, Coeffs& out) {
    const double support0 = f == Filter::Lanczos ? 3.0 : 2.0;
    double filterscale, scale;
    filterscale = scale = static_cast<double>(static_cast<float>(inSize) - 0.0f) / outSize;
    if (filterscale < 1.0) filterscale = 1.0;
    const double support = support0 * filterscale;
    const int ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
    const int count = o1 - o0;
    if (count < 1 || ksize > 4096) return c.Fail("resize: odd size");
    if (!out.bounds.Alloc(c, static_cast<size_t>(count) * 2 * sizeof(int))) return false;
    Buf tmp;   // the doubles first, as Pillow computes them
    if (!tmp.Alloc(c, static_cast<size_t>(ksize) * sizeof(double))) return false;
    if (!out.kk.Alloc(c, static_cast<size_t>(count) * ksize * sizeof(int32_t))) return false;
    double* k = reinterpret_cast<double*>(tmp.p);
    int* bounds = reinterpret_cast<int*>(out.bounds.p);
    int32_t* kk = reinterpret_cast<int32_t*>(out.kk.p);
    const double invFilterscale = 1.0 / filterscale;
    for (int xx = o0; xx < o1; ++xx) {
        const double center = 0.0f + (xx + 0.5) * scale;
        double ww = 0.0;
        int xmin = static_cast<int>(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = static_cast<int>(center + support + 0.5);
        if (xmax > inSize) xmax = inSize;
        xmax -= xmin;
        int x = 0;
        for (; x < xmax; ++x) {
            const double w = FilterValue(f, (x + xmin - center + 0.5) * invFilterscale);
            k[x] = w;
            ww += w;
        }
        if (ww != 0.0)
            for (x = 0; x < xmax; ++x) k[x] /= ww;
        for (; x < ksize; ++x) k[x] = 0;
        bounds[(xx - o0) * 2] = xmin;
        bounds[(xx - o0) * 2 + 1] = xmax;
        int32_t* row = kk + static_cast<size_t>(xx - o0) * ksize;
        for (x = 0; x < ksize; ++x) {
            row[x] = k[x] < 0 ? static_cast<int32_t>(-0.5 + k[x] * (1 << kPrecisionBits)) : static_cast<int32_t>(0.5 + k[x] * (1 << kPrecisionBits));
        }
    }
    out.ksize = ksize;
    return true;
}

inline uint8_t Clip8(int32_t v) {
    v >>= kPrecisionBits;
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}

// ImagingResampleHorizontal_8bpc: input rows [ya, yb) of src, dw pixels wide, into dst rows 0.. (dw * CH bytes each).
template <int CH>
void HPass(const uint8_t* src, size_t sstride, int ya, int yb, uint8_t* dst, int dw, const Coeffs& cf) {
    const int* bounds = cf.B();
    const int32_t* kk = cf.K();
    for (int y = ya; y < yb; ++y) {
        const uint8_t* in = src + static_cast<size_t>(y) * sstride;
        uint8_t* out = dst + static_cast<size_t>(y - ya) * dw * CH;
        for (int xx = 0; xx < dw; ++xx) {
            const int xmin = bounds[xx * 2], xmax = bounds[xx * 2 + 1];
            const int32_t* k = kk + static_cast<size_t>(xx) * cf.ksize;
            int32_t ss[CH];
            for (int ch = 0; ch < CH; ++ch) ss[ch] = 1 << (kPrecisionBits - 1);
            const uint8_t* p = in + static_cast<size_t>(xmin) * CH;
            for (int x = 0; x < xmax; ++x)
                for (int ch = 0; ch < CH; ++ch) ss[ch] += p[x * CH + ch] * k[x];
            for (int ch = 0; ch < CH; ++ch) out[xx * CH + ch] = Clip8(ss[ch]);
        }
    }
}

// ImagingResampleVertical_8bpc: output rows 0..rows of dst; the coefficients' bounds are input rows counted from `rowBase` of src.
bool VPass(Ctx& c, const uint8_t* src, size_t sstride, int rowBase, int widthBytes, uint8_t* dst, int rows, const Coeffs& cf) {
    Buf acc;
    if (!acc.Alloc(c, static_cast<size_t>(widthBytes) * sizeof(int32_t))) return false;
    int32_t* a = reinterpret_cast<int32_t*>(acc.p);
    const int* bounds = cf.B();
    const int32_t* kk = cf.K();
    for (int yy = 0; yy < rows; ++yy) {
        if ((yy & 31) == 0 && c.Stop()) return false;
        const int ymin = bounds[yy * 2] - rowBase, ymax = bounds[yy * 2 + 1];
        const int32_t* k = kk + static_cast<size_t>(yy) * cf.ksize;
        for (int i = 0; i < widthBytes; ++i) a[i] = 1 << (kPrecisionBits - 1);
        for (int y = 0; y < ymax; ++y) {
            const uint8_t* row = src + static_cast<size_t>(ymin + y) * sstride;
            const int32_t kv = k[y];
            for (int i = 0; i < widthBytes; ++i) a[i] += row[i] * kv;
        }
        uint8_t* out = dst + static_cast<size_t>(yy) * widthBytes;
        for (int i = 0; i < widthBytes; ++i) out[i] = Clip8(a[i]);
    }
    return true;
}

bool HPassAny(Ctx& c, int ch, const uint8_t* src, size_t sstride, int ya, int yb, uint8_t* dst, int dw, const Coeffs& cf) {
    if (c.Stop()) return false;
    if (ch == 1) HPass<1>(src, sstride, ya, yb, dst, dw, cf);
    else if (ch == 3) HPass<3>(src, sstride, ya, yb, dst, dw, cf);
    else HPass<4>(src, sstride, ya, yb, dst, dw, cf);
    return !c.Stop();
}

// Image.resize((dw, dh), filter) of the sw x sh picture at src (rows sstride bytes apart, ch bytes a pixel); only the result's rows [oy0, oy1)
// are made, into dst (rows dw * ch bytes). The same bytes as the rows of Pillow's whole result: the passes and their order are Pillow's
// (ImagingResampleInner), and every output row depends on its own coefficients only.
bool Resize(Ctx& c, const uint8_t* src, size_t sstride, int sw, int sh, int ch, uint8_t* dst, int dw, int dh, Filter f, int oy0, int oy1) {
    const bool needH = dw != sw, needV = dh != sh;
    const bool horizontalFirst = !((sh - dh) > 0 && (sh - dh) > (sw - dw) * 2);
    const size_t dstride = static_cast<size_t>(dw) * ch;
    if (!needH && !needV) {
        for (int y = oy0; y < oy1; ++y) std::memcpy(dst + static_cast<size_t>(y - oy0) * dstride, src + static_cast<size_t>(y) * sstride, dstride);
        return true;
    }
    Coeffs vert, horiz;
    if (needV && !Precompute(c, sh, dh, f, oy0, oy1, vert)) return false;
    if (needH && !Precompute(c, sw, dw, f, 0, dw, horiz)) return false;
    Buf tmp;
    if (needH && horizontalFirst) {
        int ya = oy0, yb = oy1;
        if (needV) {
            ya = sh;
            yb = 0;
            for (int i = 0; i < oy1 - oy0; ++i) {
                ya = std::min(ya, vert.B()[i * 2]);
                yb = std::max(yb, vert.B()[i * 2] + vert.B()[i * 2 + 1]);
            }
        }
        if (!needV) return HPassAny(c, ch, src, sstride, ya, yb, dst, dw, horiz);
        if (!tmp.Alloc(c, static_cast<size_t>(yb - ya) * dstride)) return false;
        if (!HPassAny(c, ch, src, sstride, ya, yb, tmp.p, dw, horiz)) return false;
        return VPass(c, tmp.p, dstride, ya, static_cast<int>(dstride), dst, oy1 - oy0, vert);
    }
    if (needV && needH) {   // vertical first (Pillow does so when the height shrinks more than twice what the width does)
        const size_t sbytes = static_cast<size_t>(sw) * ch;
        if (!tmp.Alloc(c, static_cast<size_t>(oy1 - oy0) * sbytes)) return false;
        if (!VPass(c, src, sstride, 0, static_cast<int>(sbytes), tmp.p, oy1 - oy0, vert)) return false;
        return HPassAny(c, ch, tmp.p, sbytes, 0, oy1 - oy0, dst, dw, horiz);
    }
    if (needV) return VPass(c, src, sstride, 0, static_cast<int>(dstride), dst, oy1 - oy0, vert);
    return HPassAny(c, ch, src, sstride, oy0, oy1, dst, dw, horiz);   // horizontal only
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Pillow's Gaussian blur (BoxBlur.c): three box passes along each axis
// ---------------------------------------------------------------------------------------------------------------------------------------------
template <int CH>
void LineBoxBlur(uint8_t* lineOut, const uint8_t* lineIn, int lastx, int radius, int edgeA, int edgeB, uint32_t ww, uint32_t fw) {
    uint32_t acc[CH], bulk[CH];
    auto in = [&](int x, int ch) -> uint32_t { return lineIn[x * CH + ch]; };
    auto moveAcc = [&](int subtract, int add) { for (int ch = 0; ch < CH; ++ch) acc[ch] += in(add, ch) - in(subtract, ch); };
    auto save = [&](int x, int left, int right) {
        for (int ch = 0; ch < CH; ++ch) {
            bulk[ch] = (acc[ch] * ww) + (in(left, ch) + in(right, ch)) * fw;
            lineOut[x * CH + ch] = static_cast<uint8_t>((bulk[ch] + (1u << 23)) >> 24);
        }
    };
    for (int ch = 0; ch < CH; ++ch) acc[ch] = in(0, ch) * (radius + 1);
    for (int x = 0; x < edgeA - 1; ++x)
        for (int ch = 0; ch < CH; ++ch) acc[ch] += in(x, ch);
    for (int ch = 0; ch < CH; ++ch) acc[ch] += in(lastx, ch) * (radius - edgeA + 1);
    if (edgeA <= edgeB) {
        for (int x = 0; x < edgeA; ++x) { moveAcc(0, x + radius); save(x, 0, x + radius + 1); }
        for (int x = edgeA; x < edgeB; ++x) { moveAcc(x - radius - 1, x + radius); save(x, x - radius - 1, x + radius + 1); }
        for (int x = edgeB; x <= lastx; ++x) { moveAcc(x - radius - 1, lastx); save(x, x - radius - 1, lastx); }
    } else {
        for (int x = 0; x < edgeB; ++x) { moveAcc(0, x + radius); save(x, 0, x + radius + 1); }
        for (int x = edgeB; x < edgeA; ++x) { moveAcc(0, lastx); save(x, 0, lastx); }
        for (int x = edgeA; x <= lastx; ++x) { moveAcc(x - radius - 1, lastx); save(x, x - radius - 1, lastx); }
    }
}

float GaussianBoxRadius(float radius, int passes) {   // _gaussian_blur_radius (float arithmetic as in C)
    float sigma2 = radius * radius / passes;
    float L = static_cast<float>(std::sqrt(12.0 * sigma2 + 1.0));
    float l = static_cast<float>(std::floor((L - 1.0) / 2.0));
    float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
    a /= 6 * (sigma2 - (l + 1) * (l + 1));
    return l + a;
}

// n passes over one line of `len` pixels (a, b are scratch lines): the result ends in `a`.
template <int CH>
void BlurLine(uint8_t* a, uint8_t* b, int len, float floatRadius, int passes) {
    const int radius = static_cast<int>(floatRadius);
    const uint32_t ww = static_cast<uint32_t>((1 << 24)) / (floatRadius * 2 + 1);
    const uint32_t fw = ((1 << 24) - (radius * 2 + 1) * ww) / 2;
    const int edgeA = std::min(radius + 1, len), edgeB = std::max(len - radius - 1, 0);
    uint8_t* from = a;
    uint8_t* to = b;
    for (int i = 0; i < passes; ++i) {
        LineBoxBlur<CH>(to, from, len - 1, radius, edgeA, edgeB, ww, fw);
        std::swap(from, to);
    }
    if (from != a) std::memcpy(a, from, static_cast<size_t>(len) * CH);
}

template <int CH>
bool GaussianBlurT(Ctx& c, uint8_t* img, int w, int h, float radius) {
    const int passes = 3;
    const float r = GaussianBoxRadius(radius, passes);
    const int longest = std::max(w, h);
    Buf lines;
    if (!lines.Alloc(c, static_cast<size_t>(longest) * CH * 2)) return false;
    uint8_t* a = lines.p;
    uint8_t* b = lines.p + static_cast<size_t>(longest) * CH;
    for (int y = 0; y < h; ++y) {
        if ((y & 31) == 0 && c.Stop()) return false;
        uint8_t* row = img + static_cast<size_t>(y) * w * CH;
        std::memcpy(a, row, static_cast<size_t>(w) * CH);
        BlurLine<CH>(a, b, w, r, passes);
        std::memcpy(row, a, static_cast<size_t>(w) * CH);
    }
    const size_t stride = static_cast<size_t>(w) * CH;
    for (int x = 0; x < w; ++x) {
        if ((x & 31) == 0 && c.Stop()) return false;
        for (int y = 0; y < h; ++y) std::memcpy(a + static_cast<size_t>(y) * CH, img + y * stride + static_cast<size_t>(x) * CH, CH);
        BlurLine<CH>(a, b, h, r, passes);
        for (int y = 0; y < h; ++y) std::memcpy(img + y * stride + static_cast<size_t>(x) * CH, a + static_cast<size_t>(y) * CH, CH);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Compositing (Paste.c, AlphaComposite.c, Convert.c)
// ---------------------------------------------------------------------------------------------------------------------------------------------
inline int Div255(int a) { const int t = a + 128; return ((t >> 8) + t) >> 8; }
inline uint8_t Blend(int mask, int dst, int src) { return static_cast<uint8_t>(Div255(dst * (255 - mask) + src * mask)); }   // paste_mask_L's BLEND

inline uint32_t ShiftDiv255(uint32_t a) { return ((a >> 8) + a) >> 8; }
// ImagingAlphaComposite for one pixel: d = s over d (RGBA, straight alpha).
inline void AlphaOver(uint8_t* d, const uint8_t* s) {
    if (s[3] == 0) return;
    const uint32_t blend = d[3] * (255u - s[3]);
    const uint32_t outa255 = s[3] * 255u + blend;
    const uint32_t coef1 = s[3] * 255u * 255u * (1u << 7) / outa255;
    const uint32_t coef2 = 255u * (1u << 7) - coef1;
    for (int i = 0; i < 3; ++i) {
        const uint32_t tmp = s[i] * coef1 + d[i] * coef2;
        d[i] = static_cast<uint8_t>(ShiftDiv255(tmp + (0x80u << 7)) >> 7);
    }
    d[3] = static_cast<uint8_t>(ShiftDiv255(outa255 + 0x80u));
}

struct Stop { double pos; int val; };
// ramp(): a table that changes along one axis; stops = [(position 0..1, value)]; entries no stop covers stay 0.
void Ramp(uint8_t* out, int length, const Stop* stops, int n) {
    for (int i = 0; i < length; ++i) {
        const double t = static_cast<double>(i) / std::max(length - 1, 1);
        out[i] = 0;
        for (int s = 0; s + 1 < n; ++s) {
            const double t0 = stops[s].pos, t1 = stops[s + 1].pos;
            const int v0 = stops[s].val, v1 = stops[s + 1].val;
            if (t0 <= t && t <= t1) {
                out[i] = static_cast<uint8_t>(static_cast<int>(v0 + (v1 - v0) * (t - t0) / std::max(t1 - t0, 1e-6)));
                break;
            }
        }
    }
}

// shade(): image.paste(solid colour, (0, 0), mask) where the mask is constant along one axis (mask[x] for a row ramp, mask[y] for a column ramp).
void ShadeByColumn(uint8_t* rgb, int w, int h, const uint8_t* maskX, const uint8_t colour[3]) {
    for (int y = 0; y < h; ++y) {
        uint8_t* row = rgb + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) {
            const int m = maskX[x];
            for (int ch = 0; ch < 3; ++ch) row[3 * x + ch] = Blend(m, row[3 * x + ch], colour[ch]);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The picture (brand-from-disc.py hero, vignette, with_shadow, fit_width, make_loading)
// ---------------------------------------------------------------------------------------------------------------------------------------------
// Image.radial_gradient("L") (256 x 256), Fill.c.
void RadialGradient(uint8_t* out) {
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            const int d = static_cast<int>(std::sqrt(static_cast<double>((x - 128) * (x - 128) + (y - 128) * (y - 128)) * 2.0));
            out[y * 256 + x] = static_cast<uint8_t>(d >= 255 ? 255 : d);
        }
    }
}

// cover(sky, 960, 544, (0.5, 0.5)) -> blur -> darken: the quiet background of hero(), into base (W x H RGB).
bool HeroBase(Ctx& c, const uint8_t* art, int aw, int ah, uint8_t* base, double baseDim) {
    const int W = kWidth, H = kHeight;
    const int sw = aw * 2 / 5, sh = ah * 3 / 5;   // sky = art.crop((0, 40, aw * 2 // 5, 40 + ah * 3 // 5))
    const uint8_t* sky = art + (static_cast<size_t>(40) * aw) * 3;
    const double scale = std::max(static_cast<double>(W) / sw, static_cast<double>(H) / sh);
    const int cw = std::max(W, static_cast<int>(std::nearbyint(sw * scale)));
    const int chh = std::max(H, static_cast<int>(std::nearbyint(sh * scale)));
    const int cx = std::min(std::max(static_cast<int>(std::nearbyint(cw * 0.5 - W / 2.0)), 0), cw - W);
    const int cy = std::min(std::max(static_cast<int>(std::nearbyint(chh * 0.5 - H / 2.0)), 0), chh - H);
    if (cw == W) {
        if (!Resize(c, sky, static_cast<size_t>(aw) * 3, sw, sh, 3, base, cw, chh, Filter::Lanczos, cy, cy + H)) return false;
    } else {
        Buf wide;
        if (!wide.Alloc(c, static_cast<size_t>(cw) * H * 3)) return false;
        if (!Resize(c, sky, static_cast<size_t>(aw) * 3, sw, sh, 3, wide.p, cw, chh, Filter::Lanczos, cy, cy + H)) return false;
        for (int y = 0; y < H; ++y) std::memcpy(base + static_cast<size_t>(y) * W * 3, wide.p + (static_cast<size_t>(y) * cw + cx) * 3, static_cast<size_t>(W) * 3);
    }
    if (!GaussianBlurT<3>(c, base, W, H, static_cast<float>(std::max(10, W / 40)))) return false;
    uint8_t table[256];
    for (int v = 0; v < 256; ++v) table[v] = static_cast<uint8_t>(static_cast<int>(v * baseDim));
    for (size_t i = 0, n = static_cast<size_t>(W) * H * 3; i < n; ++i) base[i] = table[base[i]];
    return !c.Stop();
}

// The picture at its own proportions on the right (a square of the page's height), faded in from the left; then the left shade.
bool HeroFront(Ctx& c, const uint8_t* art, int aw, int ah, uint8_t* base) {
    const int W = kWidth, H = kHeight;
    Buf front;
    if (!front.Alloc(c, static_cast<size_t>(H) * H * 3)) return false;
    if (!Resize(c, art, static_cast<size_t>(aw) * 3, aw, ah, 3, front.p, H, H, Filter::Lanczos, 0, H)) return false;
    uint8_t mask[kHeight];
    const Stop fade[] = {{0.0, 0}, {0.45, 255}, {1.0, 255}};
    Ramp(mask, H, fade, 3);
    for (int y = 0; y < H; ++y) {
        uint8_t* row = base + (static_cast<size_t>(y) * W + (W - H)) * 3;
        const uint8_t* in = front.p + static_cast<size_t>(y) * H * 3;
        for (int x = 0; x < H; ++x)
            for (int ch = 0; ch < 3; ++ch) row[3 * x + ch] = Blend(mask[x], row[3 * x + ch], in[3 * x + ch]);
    }
    return !c.Stop();
}

bool Vignette(Ctx& c, uint8_t* rgb, double strength) {
    const int W = kWidth, H = kHeight;
    Buf grad, mask;
    if (!grad.Alloc(c, 256 * 256) || !mask.Alloc(c, static_cast<size_t>(W) * H)) return false;
    RadialGradient(grad.p);
    if (!Resize(c, grad.p, 256, 256, 256, 1, mask.p, W, H, Filter::Bicubic, 0, H)) return false;
    uint8_t table[256];
    for (int v = 0; v < 256; ++v) table[v] = static_cast<uint8_t>(static_cast<int>(std::max(0, v - 90) * strength * 255 / 165));
    static const uint8_t colour[3] = {6, 4, 10};
    for (size_t i = 0, n = static_cast<size_t>(W) * H; i < n; ++i) {
        const int m = table[mask.p[i]];
        for (int ch = 0; ch < 3; ++ch) rgb[3 * i + ch] = Blend(m, rgb[3 * i + ch], colour[ch]);
    }
    return !c.Stop();
}

// with_shadow(fit_width(logo, width), (5, 9), 8) composited onto the picture at (40, 70).
bool PasteMark(Ctx& c, const Logo& logo, uint8_t* rgb) {
    const int W = kWidth, H = kHeight;
    const int width = 430, offsetX = 5, offsetY = 9, blur = 8;
    const double opacity = 0.65;
    const int h = std::max(1, static_cast<int>(std::nearbyint(static_cast<double>(static_cast<int64_t>(logo.h) * width) / logo.w)));
    if (h > 3000) return c.Fail("opening.bnr: the logo is odd");
    // premultiply (RGBA -> RGBa), resize, un-premultiply: Image.resize on an RGBA picture
    Buf pre;
    if (!pre.Alloc(c, static_cast<size_t>(logo.w) * logo.h * 4)) return false;
    for (int y = 0; y < logo.h; ++y) {
        const uint8_t* in = logo.pixels + static_cast<size_t>(y) * logo.stride;
        uint8_t* out = pre.p + static_cast<size_t>(y) * logo.w * 4;
        for (int x = 0; x < logo.w; ++x, in += 4, out += 4) {
            out[0] = static_cast<uint8_t>(Div255(in[0] * in[3]));
            out[1] = static_cast<uint8_t>(Div255(in[1] * in[3]));
            out[2] = static_cast<uint8_t>(Div255(in[2] * in[3]));
            out[3] = in[3];
        }
    }
    const int pad = blur * 3 + std::max(offsetX, offsetY);
    const int cw = width + 2 * pad, chh = h + 2 * pad;
    Buf scaled, canvas;
    if (!scaled.Alloc(c, static_cast<size_t>(width) * h * 4)) return false;
    if (!Resize(c, pre.p, static_cast<size_t>(logo.w) * 4, logo.w, logo.h, 4, scaled.p, width, h, Filter::Lanczos, 0, h)) return false;
    pre.Free();
    for (size_t i = 0, n = static_cast<size_t>(width) * h; i < n; ++i) {
        uint8_t* p = scaled.p + 4 * i;
        const unsigned a = p[3];
        if (a != 255 && a != 0) {
            for (int ch = 0; ch < 3; ++ch) { const unsigned v = (255 * p[ch]) / a; p[ch] = static_cast<uint8_t>(v > 255 ? 255 : v); }
        }
    }
    if (!canvas.Alloc(c, static_cast<size_t>(cw) * chh * 4, true)) return false;
    uint8_t alphaTable[256];
    for (int a = 0; a < 256; ++a) alphaTable[a] = static_cast<uint8_t>(static_cast<int>(a * opacity));
    for (int y = 0; y < h; ++y) {   // the shadow: black with the logo's alpha * opacity, composited at (pad + offset)
        for (int x = 0; x < width; ++x) {
            const uint8_t s[4] = {0, 0, 0, alphaTable[scaled.p[(static_cast<size_t>(y) * width + x) * 4 + 3]]};
            AlphaOver(canvas.p + (static_cast<size_t>(y + pad + offsetY) * cw + (x + pad + offsetX)) * 4, s);
        }
    }
    if (!GaussianBlurT<4>(c, canvas.p, cw, chh, static_cast<float>(blur))) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < width; ++x) AlphaOver(canvas.p + (static_cast<size_t>(y + pad) * cw + (x + pad)) * 4, scaled.p + (static_cast<size_t>(y) * width + x) * 4);
    scaled.Free();
    // paste_rgba(im, mark, (40, 70)): the picture as RGBA (alpha 255), the mark over it, back to RGB; what falls outside the picture is cut
    const int px = 40, py = 70;
    for (int y = 0; y < chh; ++y) {
        if (py + y < 0 || py + y >= H) continue;
        for (int x = 0; x < cw; ++x) {
            if (px + x < 0 || px + x >= W) continue;
            const uint8_t* s = canvas.p + (static_cast<size_t>(y) * cw + x) * 4;
            uint8_t d[4];
            uint8_t* o = rgb + (static_cast<size_t>(py + y) * W + (px + x)) * 3;
            d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; d[3] = 255;
            AlphaOver(d, s);
            o[0] = d[0]; o[1] = d[1]; o[2] = d[2];
        }
    }
    return !c.Stop();
}

bool Compose(Ctx& c, const uint8_t* art, int aw, int ah, const Logo& logo, uint8_t* out) {
    const int W = kWidth, H = kHeight;
    Buf image;
    if (!image.Alloc(c, static_cast<size_t>(W) * H * 3)) return false;
    // hero(art, 960, 544, base_dim = 0.45)
    if (!HeroBase(c, art, aw, ah, image.p, 0.45)) return false;
    if (!HeroFront(c, art, aw, ah, image.p)) return false;
    {
        static const uint8_t shadeColour[3] = {8, 5, 14};
        const double shadeLeft = 0.55;
        uint8_t mask[kWidth];
        const Stop stops[] = {{0.0, static_cast<int>(255 * shadeLeft)}, {0.55, static_cast<int>(255 * shadeLeft * 0.5)}, {1.0, 0}};
        Ramp(mask, W, stops, 3);
        ShadeByColumn(image.p, W, H, mask, shadeColour);
    }
    if (!Vignette(c, image.p, 0.5)) return false;
    if (!PasteMark(c, logo, image.p)) return false;
    {   // bottom_shade(image, 420, 0.75): rows from 420 take a ramp of 0 .. 191
        const int start = 420;
        uint8_t ramp[kHeight];
        const Stop stops[] = {{0.0, 0}, {1.0, static_cast<int>(255 * 0.75)}};
        Ramp(ramp, H - start, stops, 2);
        static const uint8_t shadeColour[3] = {8, 5, 14};
        for (int y = start; y < H; ++y) {
            uint8_t* row = image.p + static_cast<size_t>(y) * W * 3;
            const int m = ramp[y - start];
            for (int x = 0; x < W; ++x)
                for (int ch = 0; ch < 3; ++ch) row[3 * x + ch] = Blend(m, row[3 * x + ch], shadeColour[ch]);
        }
    }
    // loading_bin(): 'NPBS', u16 width, u16 height, u32 format 1, RGB565 little-endian, rows top first
    std::memcpy(out, "NPBS", 4);
    out[4] = W & 255; out[5] = W >> 8; out[6] = H & 255; out[7] = H >> 8;
    out[8] = 1; out[9] = out[10] = out[11] = 0;
    uint8_t* pixels = out + kHeaderBytes;
    for (size_t i = 0, n = static_cast<size_t>(W) * H; i < n; ++i) {
        const unsigned v = ((image.p[3 * i] >> 3) << 11) | ((image.p[3 * i + 1] >> 2) << 5) | (image.p[3 * i + 2] >> 3);
        pixels[2 * i] = static_cast<uint8_t>(v & 255);
        pixels[2 * i + 1] = static_cast<uint8_t>(v >> 8);
    }
    return true;
}

} // namespace

bool Build(const Source& bundle, const Source& banner, uint8_t* out, size_t outBytes, char* why, size_t whyBytes, Stats* stats, const volatile bool* cancel) {
    Ctx c{why, whyBytes, cancel};
    if (why && whyBytes) why[0] = 0;
    bool ok = false;
    {
        Buf art;
        int aw = 0, ah = 0;
        Logo logo;
        if (!out || outBytes < kPictureBytes) c.Fail("no room for the picture");
        else if (BundlePicture(c, bundle, kArtHash, art, aw, ah) && !c.Stop() && LoadLogo(c, banner, logo) && !c.Stop())
            ok = Compose(c, art.p, aw, ah, logo, out);
        if (stats) stats->peakBytes = c.peak;
    }
    if (!ok && why && whyBytes && !why[0]) std::snprintf(why, whyBytes, "failed");
    return ok;
}

void WriteStamp(uint8_t* out, uint64_t bundleSize, uint64_t bannerSize) {
    std::memcpy(out, "NPAS", 4);
    const uint32_t v[3] = {kFormatVersion, static_cast<uint32_t>(bundleSize), static_cast<uint32_t>(bannerSize)};
    for (int i = 0; i < 3; ++i) for (int k = 0; k < 4; ++k) out[4 + 4 * i + k] = static_cast<uint8_t>(v[i] >> (8 * k));
}

bool StampMatches(const uint8_t* trailer, uint64_t bundleSize, uint64_t bannerSize) {
    uint8_t want[kStampBytes];
    WriteStamp(want, bundleSize, bannerSize);
    return std::memcmp(trailer, want, kStampBytes) == 0;
}

} // namespace VitaBootArt
