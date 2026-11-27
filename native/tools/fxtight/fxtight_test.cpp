// Host test of np/fxtight.h: brute force through a model of the sampler. For textures made here and, with a folder of dumps, every effect texture on the disc:
// no sample outside a sprite's polygon can be nonzero, at any level of its class, with the disc's levels or generated ones; polygons are convex, inside their frame, on the lattice.
// Counts the share of the quad each class keeps. A deliberate break: FXTIGHT_BREAK=1 shrinks a corner and the test must fail.
#include "np/fxtight.h"
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Lvl { int w, h; std::vector<uint8_t> px; };   // two bytes a texel, as NpTightDecode

// the texture as the sampler can see it: both chains
static void Chains(int fmt, int W, int H, int levels, const uint8_t* d, std::vector<Lvl>* disc, std::vector<Lvl>* gen)
{
    int w = W, h = H;
    for (int L = 0; L < levels; ++L) {
        Lvl l; l.w = w; l.h = h; l.px.resize((size_t)w * h * 2);
        NpTightDecode(fmt, w, h, d, l.px.data());
        d += NpTightLevelBytes(fmt, w, h);
        disc->push_back(l);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    gen->push_back((*disc)[0]);
    while (gen->back().w > 1 || gen->back().h > 1) {
        const Lvl& s = gen->back();
        Lvl l; l.w = s.w > 1 ? s.w / 2 : 1; l.h = s.h > 1 ? s.h / 2 : 1; l.px.resize((size_t)l.w * l.h * 2);
        for (int y = 0; y < l.h; ++y) for (int x = 0; x < l.w; ++x) for (int c = 0; c < 2; ++c) {   // a box filter that rounds up: the worst a generated level can be
            int sum = 0;
            for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) sum += s.px[((size_t)(s.h > 1 ? y * 2 + j : 0) * s.w + (s.w > 1 ? x * 2 + i : 0)) * 2 + c];
            l.px[((size_t)y * l.w + x) * 2 + c] = (uint8_t)((sum + 3) / 4);
        }
        gen->push_back(l);
    }
}

// the bilinear value of channel c at a lattice point, clamped
static double Sample(const Lvl& l, double u, double v, int c)
{
    const double x = u / 1024.0 * l.w - 0.5, y = v / 1024.0 * l.h - 0.5;
    const int x0 = (int)floor(x), y0 = (int)floor(y);
    const double fx = x - x0, fy = y - y0;
    auto at = [&](int i, int j) { i = i < 0 ? 0 : i >= l.w ? l.w - 1 : i; j = j < 0 ? 0 : j >= l.h ? l.h - 1 : j; return (double)l.px[((size_t)j * l.w + i) * 2 + c]; };
    return (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
}

static bool Inside(const NpTightPoly& p, double u, double v)
{
    for (int i = 0; i < p.n; ++i) {
        const int j = i + 1 == p.n ? 0 : i + 1;
        const double ax = p.uv[i][0], ay = p.uv[i][1], bx = p.uv[j][0], by = p.uv[j][1];
        if ((bx - ax) * (v - ay) - (by - ay) * (u - ax) < 0) return false;
    }
    return true;
}

struct Tot { double keep[3][3]; int n; };

static void Check(const char* name, int fmt, int W, int H, int levels, const uint8_t* d, int nFrames, int k, bool black, Tot* t)
{
    int side = 1;
    while (side * side < nFrames) ++side;
    std::vector<NpTightRect> fr;
    const float inc = 1.0f / side;
    float u = 0, v = 0;
    for (int i = 0; i < nFrames; ++i) {
        NpTightRect r = { (short)(u * 1024.0f), (short)(v * 1024.0f), (short)((u + inc) * 1024.0f), (short)((v + inc) * 1024.0f) };
        fr.push_back(r);
        u += inc;
        if (u >= 0.999f) { u = 0; v += inc; }
    }
    std::vector<NpTightPoly> polys((size_t)nFrames * kNpTightClasses);
    const bool ok = NpTightSolve(fmt, W, H, levels, d, fr.data(), nFrames, k, black, polys.data());
    CHECK(ok, "%s: no result", name);
    if (!ok) return;
    if (getenv("FXTIGHT_BREAK")) for (auto& p : polys) if (p.n != kNpTightQuad && p.n >= 3) p.uv[0][0] = (int16_t)((p.uv[0][0] + p.uv[1 % p.n][0] + p.uv[2][0]) / 3), p.uv[0][1] = (int16_t)((p.uv[0][1] + p.uv[1][1] + p.uv[2][1]) / 3);
    std::vector<Lvl> disc, gen;
    Chains(fmt, W, H, levels, d, &disc, &gen);
    const int step = (1024 / W >= 8 ? 4 : 2) * (W >= 256 ? 2 : 1);   // about two samples a texel
    for (int f = 0; f < nFrames; ++f) {
        const NpTightRect& r = fr[(size_t)f];
        for (int c = 0; c < kNpTightClasses; ++c) {
            const NpTightPoly& p = polys[(size_t)f * kNpTightClasses + c];
            const double quad = (double)(r.u1 - r.u0) * (r.v1 - r.v0);
            double area = quad;
            if (p.n == 0) area = 0;
            else if (p.n != kNpTightQuad) {
                CHECK(p.n >= 3 && p.n <= k, "%s: %d corners", name, p.n);
                double a2 = 0;
                for (int i = 0; i < p.n; ++i) {
                    const int j = i + 1 == p.n ? 0 : i + 1;
                    const int m = i + 2 >= p.n ? i + 2 - p.n : i + 2;
                    a2 += (double)p.uv[i][0] * p.uv[j][1] - (double)p.uv[j][0] * p.uv[i][1];
                    CHECK(p.uv[i][0] >= r.u0 && p.uv[i][0] <= r.u1 && p.uv[i][1] >= r.v0 && p.uv[i][1] <= r.v1, "%s: corner out of frame", name);
                    const long cr = (long)(p.uv[j][0] - p.uv[i][0]) * (p.uv[m][1] - p.uv[i][1]) - (long)(p.uv[j][1] - p.uv[i][1]) * (p.uv[m][0] - p.uv[i][0]);
                    if (!getenv("FXTIGHT_BREAK")) CHECK(cr >= 0, "%s: not convex", name);
                }
                area = a2 / 2;
                CHECK(a2 > 0, "%s: winding", name);
            } else continue;
            t->keep[c][0] += area / quad;
            t->keep[c][1] += 1;
            // every level the class reaches, trilinear: level c + 1 is not read in class c (the class is the deepest level)
            for (double su = r.u0 + 0.25; su < r.u1; su += step)
                for (double sv = r.v0 + 0.25; sv < r.v1; sv += step) {
                    if (p.n != 0 && Inside(p, su, sv)) continue;
                    double a = 0, cc = 0;
                    for (int L = 0; L <= c; ++L) {
                        if (L < (int)disc.size()) { a = std::max(a, Sample(disc[(size_t)L], su, sv, 0)); cc = std::max(cc, Sample(disc[(size_t)L], su, sv, 1)); }
                        if (L < (int)gen.size()) { a = std::max(a, Sample(gen[(size_t)L], su, sv, 0)); cc = std::max(cc, Sample(gen[(size_t)L], su, sv, 1)); }
                    }
                    const bool shows = black ? (a > 0 && cc > 0) : a > 0;
                    CHECK(!shows, "%s: frame %d class %d: sample (%.2f, %.2f) outside the polygon has alpha %.3f, brightness %.3f", name, f, c, su, sv, a, cc);
                    if (shows) return;
                }
        }
    }
    ++t->n;
}

static void MakeBlob(std::vector<uint8_t>* out, int W, int H, int levels, int seed)
{
    // RGB5A3 tiles, big-endian: a soft blob of alpha at a place that depends on the seed, with holes
    unsigned s = 1234567u * (unsigned)(seed + 1);
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 16) & 0xFFFF; };
    int w = W, h = H;
    const double cx = 0.25 + (rnd() % 100) / 200.0, cy = 0.25 + (rnd() % 100) / 200.0, rx = 0.05 + (rnd() % 100) / 300.0, ry = 0.05 + (rnd() % 100) / 300.0;
    for (int L = 0; L < levels; ++L) {
        for (int ty = 0; ty < h; ty += 4) for (int tx = 0; tx < w; tx += 4) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            const double dx = ((tx + x + 0.5) / w - cx) / rx, dy = ((ty + y + 0.5) / h - cy) / ry;
            const double e = dx * dx + dy * dy;
            int a3 = e < 1 ? (int)((1 - e) * 7) + 1 : 0;
            if (rnd() % 5 == 0) a3 = 0;
            if (a3 > 7) a3 = 7;
            const uint16_t v = (uint16_t)((a3 << 12) | 0xFFF);
            out->push_back((uint8_t)(v >> 8));
            out->push_back((uint8_t)v);
        }
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
}

int main(int argc, char** argv)
{
    Tot t;
    memset(&t, 0, sizeof t);
    // decoder, by hand
    {
        uint8_t a, c;
        NpTightRgb5a3(0x8000, &a, &c); CHECK(a == 255 && c == 0, "opaque black");
        NpTightRgb5a3(0x7FFF, &a, &c); CHECK(a == 255 && c == 255, "alpha 7, white");
        NpTightRgb5a3(0x0FFF, &a, &c); CHECK(a == 0 && c == 255, "alpha 0, white");
        NpTightRgb5a3(0x1000, &a, &c); CHECK(a == 36 && c == 0, "alpha 1, black");
    }
    // a texture of nothing: every polygon empty, so it collapses; a full one: the quad
    {
        std::vector<uint8_t> z(128 * 128 * 2 * 2, 0);
        NpTightRect r = { 0, 0, 1024, 1024 };
        NpTightPoly p[kNpTightClasses];
        CHECK(NpTightSolve(kNpTightFmtRGB5A3, 64, 64, 2, z.data(), &r, 1, 8, false, p), "empty");
        CHECK(p[0].n == 0 && p[1].n == 0 && p[2].n == 0, "empty: %d", p[0].n);
        CHECK(p[0].peak == 0 && p[0].peakA == 0, "empty peak");
        std::fill(z.begin(), z.end(), (uint8_t)0xFF);
        CHECK(NpTightSolve(kNpTightFmtRGB5A3, 64, 64, 2, z.data(), &r, 1, 8, false, p), "full");
        CHECK(p[0].n == kNpTightQuad, "full: %d", p[0].n);
        CHECK(!NpTightSolve(0, 64, 64, 2, z.data(), &r, 1, 8, false, p), "format");
        CHECK(!NpTightSolve(kNpTightFmtRGB5A3, 512, 512, 2, z.data(), &r, 1, 8, false, p), "side");
    }
    // a floor on alpha x brightness: a smaller polygon, a wrong picture on purpose
    {
        std::vector<uint8_t> d;
        MakeBlob(&d, 64, 64, 1, 7);
        NpTightRect r = { 0, 0, 1024, 1024 };
        NpTightPoly a[kNpTightClasses], b[kNpTightClasses];
        CHECK(NpTightSolve(kNpTightFmtRGB5A3, 64, 64, 1, d.data(), &r, 1, 8, false, a) && NpTightSolve(kNpTightFmtRGB5A3, 64, 64, 1, d.data(), &r, 1, 8, false, b, 128), "floor");
        long aa = 0, ab = 0;
        NpTightPt pa[8], pb[8];
        for (int i = 0; i < a[0].n; ++i) { pa[i].x = a[0].uv[i][0]; pa[i].y = a[0].uv[i][1]; }
        for (int i = 0; i < b[0].n; ++i) { pb[i].x = b[0].uv[i][0]; pb[i].y = b[0].uv[i][1]; }
        aa = NpTightArea2(pa, a[0].n);
        ab = NpTightArea2(pb, b[0].n);
        CHECK(ab < aa, "floor: %ld, %ld", aa, ab);
        CHECK(a[0].peak == b[0].peak, "the peak comes before the floor");
    }
    // the class of a sprite
    {
        const float k2 = 150.0f * 150.0f, tex2 = 128.0f * 128.0f;
        CHECK(NpTightClass(100.0f, 10.0f, k2, tex2) == 1, "150 px, 128 texels: %d", NpTightClass(100.0f, 10.0f, k2, tex2));    // lod -0.2 + 0.5 > 0
        CHECK(NpTightClass(196.0f, 10.0f, k2, tex2) == 0, "210 px: %d", NpTightClass(196.0f, 10.0f, k2, tex2));
        CHECK(NpTightClass(16.0f, 10.0f, k2, tex2) == 2, "60 px: %d", NpTightClass(16.0f, 10.0f, k2, tex2));                  // lod 1.09 + 0.5 <= 2
        CHECK(NpTightClass(4.0f, 10.0f, k2, tex2) == -1, "30 px: %d", NpTightClass(4.0f, 10.0f, k2, tex2));                   // lod 2.09 + 0.5 > 2: a quad
        CHECK(NpTightClass(100.0f, 0.0f, k2, tex2) == -1, "on the camera plane");
    }
    for (int seed = 0; seed < 40; ++seed) {
        std::vector<uint8_t> d;
        const int W = 16 << (seed % 4), H = (seed & 8) ? W / 2 : W, levels = 1 + seed % 5;
        MakeBlob(&d, W, H, levels, seed);
        static const int fr[] = { 1, 4, 9, 16, 25, 36 };
        char name[32];
        snprintf(name, sizeof name, "blob%d", seed);
        for (int k = 4; k <= 8; k += 2)
            Check(name, kNpTightFmtRGB5A3, W, H, levels, d.data(), fr[seed % 6], k, false, &t);
    }
    // the disc's textures: name w h levels fmt, then the bytes: fxtight_dump.py
    if (argc > 1) {
        Tot s[2];
        memset(s, 0, sizeof s);
        DIR* dir = opendir(argv[1]);
        CHECK(dir, "no dump folder");
        int n = 0;
        for (dirent* e; dir && (e = readdir(dir));) {
            int W, H, levels, fmt;
            char hash[16];
            if (sscanf(e->d_name, "%15[^_]_%d_%d_%d_%d.bin", hash, &W, &H, &levels, &fmt) != 5) continue;
            FILE* f = fopen((std::string(argv[1]) + "/" + e->d_name).c_str(), "rb");
            std::vector<uint8_t> d(1 << 20);
            d.resize(fread(d.data(), 1, d.size(), f));
            fclose(f);
            if (W > kNpTightMaxSide || H > kNpTightMaxSide) continue;
            ++n;
            Check(e->d_name, fmt, W, H, levels, d.data(), 1, 4, false, &s[0]);
            Check(e->d_name, fmt, W, H, levels, d.data(), 1, 8, false, &s[1]);
            Tot x[2];
            memset(x, 0, sizeof x);
            Check(e->d_name, fmt, W, H, levels, d.data(), 4, 8, true, &x[0]);
            if (getenv("FXTIGHT_VERBOSE")) printf("%s\n", e->d_name);
        }
        if (dir) closedir(dir);
        printf("disc: %d textures\n", n);
        for (int i = 0; i < 2; ++i)
            printf("  %d corners, share of the quad kept (those that are not left as quads), class 0/1/2: %.3f (%d) %.3f (%d) %.3f (%d)\n", i ? 8 : 4, s[i].keep[0][0] / (s[i].keep[0][1] + 1e-9), (int)s[i].keep[0][1], s[i].keep[1][0] / (s[i].keep[1][1] + 1e-9), (int)s[i].keep[1][1], s[i].keep[2][0] / (s[i].keep[2][1] + 1e-9), (int)s[i].keep[2][1]);
    }
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
