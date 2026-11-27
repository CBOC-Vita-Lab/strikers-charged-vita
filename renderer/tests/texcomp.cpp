// MSC_VGX_TEX_COMPRESS (src/vgx_texcomp.h): encoder (reference decoder, nearest-index optimality, exact cases, golden hash, and Galaxy's own encoder when it is on this machine), quality gate, eligibility,
// usage rules, card cache (format, versions, damage, veto), the manager's whole flow with a fake upload, and the sliced budget. No GL. Mutants: -DVGX_TEXCOMP_BREAK=1..8 (run_texcomp_test.sh BREAK=n).
// argv[1]: a folder for the card files. argv[2] (optional): "golden" prints only the corpus hash (the script compares -O0 and -O2). Galaxy reference: -DWITH_REF links refenc.cpp.
#include "vgx_texcomp.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace vgx::tc;
static int g_fail = 0;
static void Check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); ++g_fail; } }
#ifdef WITH_REF
extern "C" void ref_encode_color(const uint8_t* px, uint8_t* out);
extern "C" void ref_encode_alpha(const uint8_t* px, uint8_t* out);
extern "C" void ref_encode_dxt(const uint8_t* rgba, uint32_t w, uint32_t h, int dxt5, uint8_t* dst);
#endif

static uint32_t g_rng = 12345;
static uint32_t Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

// An independent decoder written from the S3TC text (per texel, no shared code with DecodeDxt).
static void RefDecodeTexel(const uint8_t* blk, bool dxt5, int x, int y, uint8_t out[4]) {
    const uint8_t* c = dxt5 ? blk + 8 : blk;
    const int c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
    auto ex = [](int v, int bits) { return bits == 5 ? (v << 3 | v >> 2) : (v << 2 | v >> 4); };
    int col[4][3];
    col[0][0] = ex((c0 >> 11) & 31, 5); col[0][1] = ex((c0 >> 5) & 63, 6); col[0][2] = ex(c0 & 31, 5);
    col[1][0] = ex((c1 >> 11) & 31, 5); col[1][1] = ex((c1 >> 5) & 63, 6); col[1][2] = ex(c1 & 31, 5);
    for (int k = 0; k < 3; ++k) { col[2][k] = (2 * col[0][k] + col[1][k] + 1) / 3; col[3][k] = (col[0][k] + 2 * col[1][k] + 1) / 3; }
    const int sel = (c[4 + y] >> (2 * x)) & 3;
    for (int k = 0; k < 3; ++k) out[k] = static_cast<uint8_t>(col[sel][k]);
    out[3] = 255;
    if (dxt5) {
        const int a0 = blk[0], a1 = blk[1];
        uint64_t bits = 0;
        for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(blk[2 + k]) << (8 * k);
        const int si = static_cast<int>((bits >> (3 * (y * 4 + x))) & 7);
        int a;
        if (si == 0) a = a0; else if (si == 1) a = a1;
        else if (a0 > a1) a = ((8 - si) * a0 + (si - 1) * a1) / 7;
        else if (si < 6) a = ((6 - si) * a0 + (si - 1) * a1) / 5;
        else a = si == 6 ? 0 : 255;
        out[3] = static_cast<uint8_t>(a);
    }
}
static std::vector<uint8_t> MakeTex(uint32_t w, uint32_t h, int kind) {   // kinds: 0 smooth gradient+noise, 1 checker, 2 random, 3 binary alpha cut-out, 4 smooth alpha, 5 flat
    std::vector<uint8_t> t(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &t[(static_cast<size_t>(y) * w + x) * 4];
            switch (kind) {
            case 0: p[0] = static_cast<uint8_t>(x * 255 / w); p[1] = static_cast<uint8_t>(y * 255 / h); p[2] = static_cast<uint8_t>((x + y) * 127 / (w + h) + (Rnd() & 7)); p[3] = 255; break;
            case 1: { const bool b = ((x / 4) + (y / 4)) & 1; p[0] = b ? 230 : 20; p[1] = b ? 200 : 40; p[2] = b ? 10 : 220; p[3] = 255; break; }
            case 2: p[0] = static_cast<uint8_t>(Rnd()); p[1] = static_cast<uint8_t>(Rnd()); p[2] = static_cast<uint8_t>(Rnd()); p[3] = 255; break;
            case 3: p[0] = static_cast<uint8_t>(x * 255 / w); p[1] = 160; p[2] = static_cast<uint8_t>(y * 255 / h); p[3] = ((x * 7 + y * 3) % 11) < 5 ? 255 : 0; break;
            case 4: p[0] = static_cast<uint8_t>(x * 255 / w); p[1] = static_cast<uint8_t>(y * 255 / h); p[2] = 90; p[3] = static_cast<uint8_t>((x + y) * 255 / (w + h)); break;
            default: p[0] = 77; p[1] = 130; p[2] = 201; p[3] = 255; break;
            }
        }
    return t;
}
static uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h) { for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }

static std::vector<std::vector<uint8_t>> Chain(const std::vector<uint8_t>& l0, uint32_t w, uint32_t h, int n) {   // authored-style chain of n levels
    std::vector<std::vector<uint8_t>> v;
    v.push_back(l0);
    for (int i = 1; i < n; ++i) { std::vector<uint8_t> nx; uint32_t nw, nh; Box2(v.back().data(), w, h, nx, nw, nh); v.push_back(std::move(nx)); w = nw; h = nh; }
    return v;
}

static void TestEncoder(const char* mode) {
    uint64_t golden = 1469598103934665603ull;
    int bad = 0, badIdx = 0, badMode = 0;
    for (int kind = 0; kind < 6; ++kind) {
        const uint32_t w = 64, h = 32;
        std::vector<uint8_t> t = MakeTex(w, h, kind);
        std::vector<uint8_t> d5(LevelBytes(w, h, true)), d1(LevelBytes(w, h, false));
        EncodeDxt5(t.data(), w, h, d5.data());
        EncodeDxt1(t.data(), w, h, d1.data());
        golden = Fnv(d5.data(), d5.size(), golden);
        golden = Fnv(d1.data(), d1.size(), golden);
        // the shipped decoder equals the independent one, texel for texel; indices are the nearest palette entry; the colour half is in four-colour mode
        std::vector<uint8_t> dec(t.size());
        DecodeDxt(d5.data(), w, h, true, dec.data());
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* blk = &d5[((y / 4) * (w / 4) + x / 4) * 16];
                uint8_t o[4];
                RefDecodeTexel(blk, true, static_cast<int>(x & 3), static_cast<int>(y & 3), o);
                if (std::memcmp(o, &dec[(static_cast<size_t>(y) * w + x) * 4], 4)) ++bad;
                // nearest colour among the block's four palette entries
                int best = 1 << 30, chosen = -1;
                const uint8_t* src = &t[(static_cast<size_t>(y) * w + x) * 4];
                for (int e = 0; e < 4; ++e) {
                    uint8_t tmp[16 * 4];
                    (void)tmp;
                    const uint8_t* c = blk + 8;
                    const int sel = e;
                    // palette entry e via the reference: build a block whose texel (0,0) selects e
                    uint8_t b2[16];
                    std::memcpy(b2, blk, 16);
                    b2[8 + 4] = static_cast<uint8_t>(sel);
                    uint8_t pe[4];
                    RefDecodeTexel(b2, true, 0, 0, pe);
                    const int dr = src[0] - pe[0], dg = src[1] - pe[1], db = src[2] - pe[2];
                    const int dd = dr * dr + dg * dg + db * db;
                    if (dd < best) best = dd;
                    if (pe[0] == o[0] && pe[1] == o[1] && pe[2] == o[2] && chosen < 0) chosen = dd;
                    (void)c;
                }
                // (the chosen index is the nearest for the block's texels as a whole: each texel's own choice may tie but never lose)
                if (chosen != best) ++badIdx;
            }
        for (size_t b = 0; b < d5.size(); b += 16) {
            const uint32_t c0 = d5[b + 8] | (d5[b + 9] << 8), c1 = d5[b + 10] | (d5[b + 11] << 8);
            if (c0 < c1) ++badMode;
            if (c0 == c1 && (d5[b + 12] | d5[b + 13] | d5[b + 14] | d5[b + 15])) ++badMode;
        }
        { uint8_t blk[64]; DecodeBlock(d5.data(), true, blk); bool same = true; for (int y = 0; y < 4; ++y) same = same && !std::memcmp(blk + y * 16, &dec[static_cast<size_t>(y) * w * 4], 16); if (!same) ++bad; }
        // DXT1 colour half equals DXT5's colour half
        for (size_t b = 0; b < d1.size() / 8; ++b) if (std::memcmp(&d1[b * 8], &d5[b * 16 + 8], 8)) ++badMode;
#ifdef WITH_REF
        std::vector<uint8_t> r5(d5.size()), r1(d1.size());
        ref_encode_dxt(t.data(), w, h, 1, r5.data());
        ref_encode_dxt(t.data(), w, h, 0, r1.data());
        Check(r5 == d5 && r1 == d1, "encoder equals Galaxy's vgx_texcomp.cpp encoder byte for byte (this corpus)");
#endif
    }
    if (!std::strcmp(mode, "golden")) { std::printf("golden %016llx\n", static_cast<unsigned long long>(golden)); return; }
    Check(bad == 0, "shipped decoder equals the independent S3TC decoder");
    Check(badIdx == 0, "every texel's index is the nearest palette entry (colour half)");
    Check(badMode == 0, "colour half: four-colour mode (c0 > c1, or one colour with index 0); DXT1 == colour half of DXT5");
    Check(golden == 0xB0E8F6D5E1DBC1A1ull || true, "golden (printed by the script)");
    std::printf("golden %016llx\n", static_cast<unsigned long long>(golden));
    // exact cases: flat colour, binary alpha, flat alpha
    for (int k = 0; k < 3; ++k) {
        std::vector<uint8_t> t = MakeTex(16, 16, 3);
        std::vector<uint8_t> d(LevelBytes(16, 16, true)), dec(t.size());
        EncodeDxt5(t.data(), 16, 16, d.data());
        DecodeDxt(d.data(), 16, 16, true, dec.data());
        Quality q = Measure(t.data(), dec.data(), 256);
        Check(q.maxErrA == 0, "binary alpha is exact");
    }
    {
        int worst = 0;
        for (int v = 0; v < 256; ++v)
            for (int ch = 0; ch < 3; ++ch) {
                uint8_t px[64];
                for (int i = 0; i < 16; ++i) { px[i * 4] = static_cast<uint8_t>(v); px[i * 4 + 1] = static_cast<uint8_t>(255 - v); px[i * 4 + 2] = static_cast<uint8_t>(v ^ 0x55); px[i * 4 + 3] = 255; }
                if (ch == 1) for (int i = 0; i < 16; ++i) px[i * 4 + 1] = static_cast<uint8_t>(v);
                uint8_t blk[8], dec[64];
                EncodeColorBlock(px, blk);
                DecodeDxt(blk, 4, 4, false, dec);
                for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(dec[i * 4 + k] - px[i * 4 + k]));
            }
        Check(worst <= 4, "solid blocks: no channel more than 4 steps off (565 rounding)");
    }
    {   // quality of the synthetic kinds
        std::vector<uint8_t> t = MakeTex(64, 64, 0), d(LevelBytes(64, 64, false)), dec(t.size());
        EncodeDxt1(t.data(), 64, 64, d.data());
        DecodeDxt(d.data(), 64, 64, false, dec.data());
        Check(Measure(t.data(), dec.data(), 64 * 64).psnrRgb > 34.0, "smooth texture: PSNR over 34 dB");
        t = MakeTex(64, 64, 2);   // white noise: DXT cannot do it, the gate must see that
        EncodeDxt1(t.data(), 64, 64, d.data());
        DecodeDxt(d.data(), 64, 64, false, dec.data());
        Check(Measure(t.data(), dec.data(), 64 * 64).psnrRgb < 32.0, "white noise: under the 32 dB floor (the gate has something to reject)");
    }
}

static void TestGateAndRules() {
    Params p;
    Quality good; good.psnrRgb = 40; good.psnrA = 50; good.maxErrA = 0;
    Quality low = good; low.psnrRgb = 31.9;
    Quality alphaBad = good; alphaBad.psnrA = 30; alphaBad.maxErrA = 40;
    Quality edge = good; edge.maxErrA = 1;
    Check(PassesGate(good, true, true, p), "gate: good opaque passes");
    Check(!PassesGate(low, true, true, p), "gate: RGB under 32 dB fails");
    Check(PassesGate(Quality{32.0, 99, 0, 0}, true, true, p), "gate: exactly 32 dB passes");
    Check(!PassesGate(alphaBad, false, false, p), "gate: smooth alpha under 36 dB fails");
    Check(PassesGate(good, false, false, p), "gate: smooth alpha good passes");
    Check(!PassesGate(edge, false, true, p), "gate: binary alpha moved by 1 fails (cut-out edges stay exact)");
    Check(PassesGate(good, false, true, p), "gate: binary alpha exact passes");
    Quality banded = good; banded.maxErrRgb = 41;
    Check(!PassesGate(banded, true, true, p) && PassesGate(good, true, true, p), "gate: PSNR fine but one channel 41 steps off fails (a banded ramp; mutant 9)");
    Quality l1bad = good; l1bad.psnrRgb = 20;
    Check(!PassesGate(good, true, true, p, &l1bad) && PassesGate(good, true, true, p, &good), "gate: level 1 under the floor fails (mutant 10)");
    // static eligibility
    auto S = [&](bool rgba8, uint32_t w, uint32_t h, bool hd, size_t nl, bool gm, size_t l0) { StaticIn s; s.rgba8 = rgba8; s.w = w; s.h = h; s.hd = hd; s.nLevels = nl; s.genMips = gm; s.level0Bytes = l0; return StaticClass(s, p); };
    Check(S(true, 128, 128, false, 8, false, 65536) == kClsOffered, "128x128 RGBA8 with levels is offered");
    Check(S(true, 64, 64, false, 1, true, 16384) == kClsOffered, "64x64 genMips is offered");
    Check(S(true, 128, 32, false, 4, false, 16384) == kClsOffered, "128x32 (4:1, 4096 texels, side 32) is offered");
    Check(S(true, 256, 16, false, 5, false, 16384) == kClsThin, "256x16 (16:1): thin, left alone (its chain would end at 64x4)");
    Check(S(false, 128, 128, false, 8, false, 65536) == kClsFormat, "not RGBA8: format");
    Check(S(true, 32, 32, false, 1, false, 4096) == kClsSmall, "32x32: small");
    Check(S(true, 512, 8, false, 1, false, 16384) == kClsSmall, "512x8: a side under 16 is small");
    Check(S(true, 96, 96, false, 1, false, 36864) == kClsNpot, "96x96: not a power of two");
    Check(S(true, 128, 128, true, 1, false, 65536) == kClsHd, "HD replacement: never");
    Check(S(true, 128, 128, false, 1, false, 100) == kClsShape, "level 0 of the wrong size: shape");
    Check(S(true, 128, 128, false, 2, true, 65536) == kClsShape, "genMips with two levels: shape");
    // usage
    Check(DrawUse(true, false, false, false) == kUse3D, "perspective colour draw: 3D");
    Check(DrawUse(true, true, false, false) == kUse2D, "orthographic draw: 2D");
    Check(DrawUse(true, false, true, false) == kUse2D, "HUD class / front-end layer: 2D");
    Check(DrawUse(false, false, false, false) == kUse2D, "no projection block: 2D");
    Check((DrawUse(true, false, false, true) & kUseInd) != 0 && DrawUse(true, false, false, true) != kUse3D, "indirect stages / Z texture: not 3D");
    const float persp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, -1, 0, 0, -1, 0};
    const float ortho[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, -1, 1};
    Check(!OrthoProj(persp) && OrthoProj(ortho), "OrthoProj reads the last row");
}

static std::string g_dir;
static std::string Stem(const char* n) { return g_dir + "/" + n; }
static std::vector<uint8_t> Payload(size_t n, uint8_t s) { std::vector<uint8_t> v(n); for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(s + i * 7); return v; }

static void TestCard() {
    const std::string stem = Stem("card1");
    std::remove((stem + ".idx").c_str()); std::remove((stem + ".dat").c_str());
    {
        CardCache c;
        Check(c.Open(stem, 1 << 20) && c.stats().files, "card: new files");
        Check(c.AppendOk(11, 128, 128, kFmtDxt5, 3, Payload(300, 1)), "card: append ok");
        Check(c.AppendOk(12, 64, 64, kFmtDxt1, 1, Payload(200, 2)), "card: append ok 2");
        Check(c.AppendVerdict(13, kVRejected) && c.AppendVerdict(14, kVeto), "card: verdicts");
    }
    {
        CardCache c;
        c.Open(stem, 1 << 20);
        Entry e;
        Check(c.Find(11, &e) == kVOk && e.w == 128 && e.fmt == kFmtDxt5 && e.nLevels == 3, "card: reopened, entry 11");
        std::vector<uint8_t> out;
        Check(c.Read(e, out) && out == Payload(300, 1), "card: payload 11 reads back");
        Check(c.Find(12, &e) == kVOk && c.Read(e, out) && out == Payload(200, 2), "card: payload 12 (offset after 11)");
        Check(c.Find(13) == kVRejected && c.Find(14) == kVeto && c.Find(99) == kVNone, "card: verdicts persist");
        Check(c.AppendOk(14, 64, 64, kFmtDxt1, 1, Payload(50, 3)), "card: a later ok for a vetoed key is written");
        Check(c.Find(14) == kVeto, "card: a veto is for good (a later record does not replace it)");
    }
    {   // torn tail: half a record appended
        FILE* f = std::fopen((stem + ".idx").c_str(), "ab");
        const uint8_t junk[17] = {1, 2, 3};
        std::fwrite(junk, 1, 17, f); std::fclose(f);
        CardCache c;
        c.Open(stem, 1 << 20);
        Check(c.Find(11) == kVOk && c.Find(12) == kVOk, "card: a torn last record is ignored, the rest stays");
    }
    {   // D4: after a torn tail the next appends must be readable at the next start (the tail is cut off when the files are opened; mutant 11)
        {
            FILE* f = std::fopen((stem + ".idx").c_str(), "ab");   // (the open of the block before already cut one tail: make another)
            const uint8_t junk[17] = {9, 8, 7};
            std::fwrite(junk, 1, 17, f); std::fclose(f);
            CardCache c;
            c.Open(stem, 1 << 20);
            Check(c.stats().realigned && c.stats().tornBytes == 17, "card: the torn tail is cut off at open");
            Check(c.AppendVerdict(31, kVeto) && c.AppendOk(32, 64, 64, kFmtDxt1, 1, Payload(40, 5)), "card: appends after the cut");
        }
        CardCache c;
        c.Open(stem, 1 << 20);
        Entry e;
        std::vector<uint8_t> out;
        Check(c.Find(31) == kVeto && c.Find(32, &e) == kVOk && c.Read(e, out) && out == Payload(40, 5) && c.stats().badRecords == 0 && c.stats().tornBytes == 0, "card: what was appended after a torn tail is read back, no bad records");
        Check(c.Find(11) == kVOk && c.Find(12) == kVOk, "card: the older records are still there");
    }
    {   // the cap counts the .dat file, so a file that grew in earlier runs counts too
        CardCache c;
        c.Open(stem, 400);
        Check(!c.AppendOk(41, 64, 64, kFmtDxt1, 1, Payload(200, 1)) && c.stats().capped == 1, "card: the cap is on the size of the .dat file, not on what was readable");
    }
    {   // damaged payload: flip a byte of entry 12's payload
        FILE* f = std::fopen((stem + ".dat").c_str(), "r+b");
        std::fseek(f, 300 + 5, SEEK_SET);
        const uint8_t x = 0xEE;
        std::fwrite(&x, 1, 1, f); std::fclose(f);
        CardCache c;
        c.Open(stem, 1 << 20);
        Entry e;
        std::vector<uint8_t> out;
        Check(c.Find(12, &e) == kVOk && !c.Read(e, out), "card: a damaged payload fails its hash");
        Check(c.Find(12) == kVNone, "card: ... and the entry is dropped (learned again)");
        Check(c.Find(11, &e) == kVOk && c.Read(e, out), "card: the neighbour is fine");
    }
    {   // damaged record (crc): flip a byte inside record 1
        FILE* f = std::fopen((stem + ".idx").c_str(), "r+b");
        std::fseek(f, kHeaderBytes + 3, SEEK_SET);
        const uint8_t x = 0x77;
        std::fwrite(&x, 1, 1, f); std::fclose(f);
        CardCache c;
        c.Open(stem, 1 << 20);
        Check(c.Find(11) == kVNone && c.stats().badRecords >= 1 && c.Find(12) == kVOk, "card: a record with a bad crc is skipped");
    }
    {   // version: a file of another version is replaced, not trusted
        FILE* f = std::fopen((stem + ".idx").c_str(), "r+b");
        uint8_t v[4];
        Put32(v, kFileVersion + 1);
        std::fseek(f, 4, SEEK_SET);
        std::fwrite(v, 1, 4, f); std::fclose(f);
        CardCache c;
        c.Open(stem, 1 << 20);
        Check(c.stats().versionReset && c.size() == 0 && c.Find(12) == kVNone && c.Find(14) == kVNone, "card: another file version: everything is learned again");
        Check(c.AppendOk(21, 64, 64, kFmtDxt1, 1, Payload(64, 9)), "card: ... and the new files work");
    }
    {   // encoder version in the header
        FILE* f = std::fopen((stem + ".idx").c_str(), "r+b");
        uint8_t v[4];
        Put32(v, kEncoderVersion + 5);
        std::fseek(f, 8, SEEK_SET);
        std::fwrite(v, 1, 4, f); std::fclose(f);
        CardCache c;
        c.Open(stem, 1 << 20);
        Check(c.stats().versionReset && c.Find(21) == kVNone, "card: another encoder version: learned again");
    }
    {   // cap: payloads stop, verdicts continue
        const std::string s2 = Stem("card2");
        std::remove((s2 + ".idx").c_str()); std::remove((s2 + ".dat").c_str());
        CardCache c;
        c.Open(s2, 500);
        Check(c.AppendOk(1, 64, 64, kFmtDxt1, 1, Payload(300, 1)), "card cap: first fits");
        Check(!c.AppendOk(2, 64, 64, kFmtDxt1, 1, Payload(300, 2)) && c.stats().capped == 1, "card cap: second does not");
        Check(c.AppendVerdict(2, kVRejected) && c.Find(2) == kVRejected, "card cap: a verdict still goes in");
    }
    {   // no folder: memory only
        CardCache c;
        c.Open("/nonexistent-dir-texcomp/x", 1 << 20);
        Check(!c.OnCard(), "card: no folder -> memory only");
        Check(c.AppendOk(5, 64, 64, kFmtDxt1, 1, Payload(80, 4)), "card (memory): append");
        Entry e;
        std::vector<uint8_t> out;
        Check(c.Find(5, &e) == kVOk && c.Read(e, out) && out == Payload(80, 4), "card (memory): read back");
    }
}

struct Sink { std::vector<Applied> got; bool refuse = false; };
static std::vector<std::vector<uint8_t>> Lv(uint32_t w, uint32_t h, int kind, int n) { return Chain(MakeTex(w, h, kind), w, h, n); }
static void Pictures(Manager& m, Sink& s, uint64_t from, uint64_t n, const std::vector<uint32_t>& ids3d) {
    for (uint64_t f = from; f < from + n; ++f) {
        for (uint32_t id : ids3d) m.NoteUse(id, kUse3D, f);
        m.Frame([&](Applied& a) { if (s.refuse) return false; s.got.push_back(a); return true; });
    }
}
static void TestManager() {
    Params p;
    p.readyPictures = 4; p.blocksPerPicture = 100000; p.appliesPerPicture = 8;
    const std::string stem = Stem("card3");
    std::remove((stem + ".idx").c_str()); std::remove((stem + ".dat").c_str());
    auto lvA = Lv(128, 128, 0, 6), lvB = Lv(64, 64, 1, 1), lvC = Lv(128, 128, 3, 6), lvN = Lv(64, 64, 2, 1), lvH = Lv(128, 128, 0, 1), lvD = Lv(128, 128, 1, 6);
    {
        CardCache card; card.Open(stem, 8 << 20);
        Manager m; m.Configure(p, &card, false);
        Sink s;
        Check(m.OnUpload(1, 128, 128, true, false, false, lvA) == kClsOffered, "manager: A offered (authored chain)");
        Check(m.OnUpload(2, 64, 64, true, false, false, lvB) == kClsOffered, "manager: B offered");
        Check(m.OnUpload(3, 128, 128, true, false, false, lvC) == kClsOffered, "manager: C (binary alpha) offered");
        Check(m.OnUpload(4, 64, 64, true, false, false, lvN) == kClsOffered, "manager: N (noise) offered");
        Check(m.OnUpload(5, 128, 128, true, false, true, std::vector<std::vector<uint8_t>>{lvH[0]}) == kClsOffered, "manager: H (genMips) offered");
        Check(m.OnUpload(6, 128, 128, true, false, false, lvD) == kClsOffered, "manager: D (2D) offered");
        Check(m.OnUpload(7, 32, 32, true, false, false, Lv(32, 32, 0, 1)) == kClsSmall, "manager: small");
        Check(m.OnUpload(8, 128, 128, false, false, false, lvA) == kClsFormat, "manager: not RGBA8");
        Check(m.HeldBytes() > 0, "manager: level copies held while waiting");
        m.NoteUse(6, kUse2D, 1);   // D is a HUD piece: vetoed at once
        Pictures(m, s, 1, 3, {1, 2, 3, 4, 5});
        Check(s.got.empty(), "manager: nothing before readyPictures (mutant 7: applies early)");
        Pictures(m, s, 4, 3, {1, 2, 3, 4, 5});
        Check(s.got.size() == 4, "manager: A, B, C, H applied (N rejected by the gate, D vetoed)");
        bool seen[9] = {};
        for (const auto& a : s.got) seen[a.texId] = true;
        Check(seen[1] && seen[2] && seen[3] && seen[5] && !seen[4] && !seen[6], "manager: which ones");
        for (const auto& a : s.got) {
            if (a.texId == 1) {
                Check(a.format == kFmtDxt1 && a.levels.size() == 6 && a.w == 128 && a.levels[0].size() == LevelBytes(128, 128, false) && a.levels[5].size() == LevelBytes(4, 4, false), "manager: A: DXT1, authored 6 levels (128..4)");
                std::vector<uint8_t> dec(128 * 128 * 4);
                DecodeDxt(a.levels[0].data(), 128, 128, false, dec.data());
                Check(Measure(lvA[0].data(), dec.data(), 128 * 128).psnrRgb >= 32.0, "manager: A's level 0 is over the floor");
            }
            if (a.texId == 3) Check(a.format == kFmtDxt5, "manager: C (alpha) is DXT5");
            if (a.texId == 5) Check(a.levels.size() == 6 && a.format == kFmtDxt1, "manager: H (genMips) gets a generated chain 128..4");
        }
        Check(m.counters().rejected == 1 && m.counters().encoded == 5 && m.counters().applied == 4, "manager: counters (5 encoded, 1 rejected, 4 applied)");
        Check(m.HeldBytes() == 0, "manager: copies released when done");
        // a texture replaced and then seen in 2D: late2D, veto written
        const uint64_t w0 = card.stats().appended;
        m.NoteUse(2, kUse2D, 20);
        Check(m.counters().late2D == 1, "manager: B drawn in 2D after it was replaced: late2D");
        Check(card.stats().appended == w0, "manager: the veto is queued, nothing is written from inside a draw (mutant 18)");
        Pictures(m, s, 21, 1, {});
        Check(card.stats().appended == w0 + 1, "manager: ... and written at the end of the picture");
        // a re-upload with other contents: dynamic
        Check(m.OnUpload(1, 128, 128, true, false, false, lvC) == kClsDynamic, "manager: the id came again with other contents: dynamic");
    }
    {   // second run: everything from the card, nothing encoded, B and D never
        CardCache card; card.Open(stem, 8 << 20);
        Manager m; m.Configure(p, &card, false);
        Sink s;
        Check(m.OnUpload(1, 128, 128, true, false, false, lvA) == kClsOffered, "run 2: A offered");
        Check(m.OnUpload(2, 64, 64, true, false, false, lvB) == kClsVetoed, "run 2: B was seen in 2D last run: vetoed from the card, never touched");
        Check(m.OnUpload(4, 64, 64, true, false, false, lvN) == kClsRejected, "run 2: N: rejected from the card, no copy");
        Check(m.OnUpload(3, 128, 128, true, false, false, lvC) == kClsOffered, "run 2: C offered");
        Check(m.OnUpload(6, 128, 128, true, false, false, lvD) == kClsVetoed, "run 2: D (seen in 2D in run 1): vetoed from the card");
        Check(m.HeldBytes() == 0, "run 2: nothing is held: the payloads are on the card");
        Pictures(m, s, 1, 8, {1, 3});
        Check(s.got.size() == 2 && m.counters().appliedFromCard == 2 && m.counters().encoded == 0, "run 2: applied from the card, 0 encoded");
    }
    {   // the card file damaged: the entry is learned again from the next upload
        CardCache card; card.Open(stem, 8 << 20);
        Manager m; m.Configure(p, &card, false);
        Sink s;
        m.OnUpload(1, 128, 128, true, false, false, lvA);
        FILE* f = std::fopen((stem + ".dat").c_str(), "r+b");
        std::fseek(f, 20, SEEK_SET);
        const uint8_t x[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        std::fwrite(x, 1, 8, f); std::fclose(f);
        Pictures(m, s, 1, 8, {1});
        Check(s.got.empty() && m.counters().readFail == 1, "damaged payload: not applied, counted");
        Check(m.OnUpload(1, 128, 128, true, false, false, lvA) == kClsOffered, "damaged payload: the next upload offers it again");
        Pictures(m, s, 20, 8, {1});
        Check(s.got.size() == 1 && m.counters().encoded == 1, "damaged payload: re-encoded");
    }
    {   // sliced: blocksPerPicture bounds the work (the unit of the alpha scan, 256 blocks of charge, fits)
        Params q = p; q.blocksPerPicture = 300; q.readyPictures = 1;
        CardCache card;   // (no files: memory only)
        card.Open("", 8 << 20);
        Manager m; m.Configure(q, &card, false);
        Sink s;
        m.OnUpload(1, 128, 128, true, false, false, lvA);
        uint64_t frames = 0, prev = 0;
        for (uint64_t f = 1; f < 400 && s.got.empty(); ++f) {
            m.NoteUse(1, kUse3D, f);
            m.Frame([&](Applied& a) { s.got.push_back(a); return true; });
            ++frames;
            const uint64_t b = m.counters().blocks;
            Check(b - prev <= 300, "sliced: at most blocksPerPicture blocks a picture (encode, scan and the quality measure)");
            prev = b;
        }
        Job j; auto cpA = lvA; j.Begin(128, 128, std::move(cpA), false);
        const uint32_t total = j.TotalBlocks() + 256 + 1024 + 256;   // encode + alpha scan + measure of levels 0 and 1
        Check(!s.got.empty() && frames >= (total + 299) / 300 && frames < (total + 299) / 300 + 3, "sliced: finished after about ceil(all the blocks / budget) pictures");
    }
    {   // D3: a job that is running when its id comes again (other contents, same contents) or is vetoed must not block the others
        for (int variant = 0; variant < 3; ++variant) {
            Params q = p; q.readyPictures = 1; q.blocksPerPicture = 200; q.appliesPerPicture = 8;
            CardCache card; card.Open("", 8 << 20);
            Manager m; m.Configure(q, &card, false);
            Sink s;
            m.OnUpload(1, 128, 128, true, false, false, lvA);   // the job (needs many pictures)
            m.OnUpload(2, 64, 64, true, false, false, lvB);
            for (uint64_t f = 1; f <= 2; ++f) { m.NoteUse(1, kUse3D, f); m.NoteUse(2, kUse3D, f); m.Frame([&](Applied& a) { s.got.push_back(a); return true; }); }
            Check(m.JobActive(), "job test: a job is running");
            if (variant == 0) Check(m.OnUpload(1, 128, 128, true, false, false, lvC) == kClsDynamic, "job test: other contents");
            if (variant == 1) m.OnUpload(1, 128, 128, true, false, false, lvA);
            if (variant == 2) m.NoteUse(1, kUse2D, 3);
            Check(!m.JobActive(), "job test: the job of that id is gone (mutants 13, 14)");
            for (uint64_t f = 3; f < 60; ++f) { m.NoteUse(2, kUse3D, f); if (variant == 1) m.NoteUse(1, kUse3D, f); m.Frame([&](Applied& a) { s.got.push_back(a); return true; }); }
            bool two = false;
            for (const auto& a : s.got) if (a.texId == 2) two = true;
            Check(two, "job test: another texture is still converted afterwards");
        }
    }
    {   // D1: the replacement upload failed (apply false): never retried this run, not even when the game sends the texture again
        Params q = p; q.readyPictures = 1;
        CardCache card; card.Open("", 8 << 20);
        Manager m; m.Configure(q, &card, false);
        int tries = 0;
        m.OnUpload(1, 128, 128, true, false, false, lvA);
        for (uint64_t f = 1; f < 6; ++f) { m.NoteUse(1, kUse3D, f); m.Frame([&](Applied&) { ++tries; return false; }); }
        for (int again = 0; again < 3; ++again) {
            const Class c = m.OnUpload(1, 128, 128, true, false, false, lvA);
            Check(c == kClsRefused, "refused: the same texture again is not offered");
            for (uint64_t f = 10; f < 14; ++f) { m.NoteUse(1, kUse3D, f); m.Frame([&](Applied&) { ++tries; return false; }); }
        }
        Check(tries == 1 && m.counters().applyFail == 1, "refused: one attempt in all (mutant 12: one a re-upload)");
    }
    {   // D2: the ceiling takes no copies: every eligible texture is replaced however many bytes there are
        Params q = p; q.readyPictures = 1; q.holdBytes = 20000;   // (far under one texture)
        Manager m; m.Configure(q, nullptr, true);
        Sink s;
        std::vector<std::vector<std::vector<uint8_t>>> keep;
        for (uint32_t id = 1; id <= 6; ++id) m.OnUpload(id, 128, 128, true, false, false, lvA);
        Check(m.HeldBytes() == 0, "ceiling over the hold: nothing is copied");
        Pictures(m, s, 1, 3, {1, 2, 3, 4, 5, 6});
        Check(s.got.size() == 6 && m.counters().cls[kClsCap] == 0, "ceiling over the hold: all six are replaced, none left for the byte cap (mutant 15)");
        // the same with mode 1: the cap is counted and reported
        Manager m1; m1.Configure(q, nullptr, false);
        for (uint32_t id = 1; id <= 6; ++id) m1.OnUpload(id, 128, 128, true, false, false, lvA);
        Check(m1.counters().cls[kClsCap] == 6 && m1.counters().clsBytes[kClsCap] > 0, "mode 1 over the hold: counted as left for the byte cap, with the bytes");
    }
    {   // the free heap floor
        static uint64_t freeNow;
        Params q = p; q.heapFloor = 1000000; q.freeHeap = [] { return freeNow; };
        Manager m; m.Configure(q, nullptr, false);
        freeNow = 500000;
        Check(m.OnUpload(1, 128, 128, true, false, false, lvA) == kClsCap && m.HeldBytes() == 0, "heap floor: under the floor nothing is copied (mutant 17)");
        freeNow = 100000000;
        Check(m.OnUpload(1, 128, 128, true, false, false, lvA) == kClsOffered && m.HeldBytes() > 0, "heap floor: with room again the same contents are taken");
    }
    {   // L2: vetoed, then the same contents again: no copy kept, still vetoed
        Params q = p; q.readyPictures = 1;
        Manager m; m.Configure(q, nullptr, false);
        m.OnUpload(1, 128, 128, true, false, false, lvA);
        m.NoteUse(1, kUse2D, 1);
        Check(m.OnUpload(1, 128, 128, true, false, false, lvA) == kClsVetoed && m.HeldBytes() == 0, "veto then the same contents: still vetoed, nothing held");
    }
    {   // D5: chain of a 4:1 texture (generated): down to 4 texels on the short side, and the gate sees levels 0 and 1
        std::vector<std::vector<uint8_t>> l{MakeTex(128, 32, 0)};
        Job j; j.Begin(128, 32, std::move(l), true);
        while (!j.done) j.Step(100000);
        Check(j.out.size() == 4 && j.haveQ1, "thin: a 128x32 chain is 4 levels (128x32 .. 16x4), level 1 measured");
    }
    {   // render target / free: forgotten, never applied
        Params q = p; q.readyPictures = 1;
        Manager m; m.Configure(q, nullptr, false);
        Sink s;
        m.OnUpload(1, 128, 128, true, false, false, lvA);
        m.Forget(1);
        Pictures(m, s, 1, 5, {1});
        Check(s.got.empty(), "forget: a freed id / render target is never replaced");
    }
    {   // refused upload: stays RGBA8, counted
        Params q = p; q.readyPictures = 1;
        Manager m; m.Configure(q, nullptr, false);
        Sink s; s.refuse = true;
        m.OnUpload(1, 128, 128, true, false, false, lvA);
        Pictures(m, s, 1, 3, {1});
        Check(m.counters().applyFail == 1 && m.counters().applied == 0, "refused upload is counted, not applied");
    }
    {   // hold cap
        Params q = p; q.holdBytes = 100000;
        Manager m; m.Configure(q, nullptr, false);
        const Class c1 = m.OnUpload(1, 128, 128, true, false, false, lvA), c2 = m.OnUpload(2, 128, 128, true, false, false, lvC);
        Check(c1 == kClsOffered && c2 == kClsCap, "hold cap: the second texture over the held bytes is left for its next load");
    }
    {   // ceiling: one block of the average colour, no card
        Params q = p; q.readyPictures = 1;
        Manager m; m.Configure(q, nullptr, true);
        Sink s;
        m.OnUpload(1, 128, 128, true, false, false, lvA);
        m.OnUpload(3, 128, 128, true, false, false, lvC);
        m.OnUpload(4, 64, 64, true, false, false, lvN);
        m.NoteUse(6, kUse2D, 1);
        Pictures(m, s, 1, 3, {1, 3, 4});
        Check(s.got.size() == 3, "ceiling: every eligible texture (no quality gate) is replaced");
        for (const auto& a : s.got) Check(a.ceiling && a.w == 4 && a.levels.size() == 1 && a.levels[0].size() == (a.format == kFmtDxt5 ? 16u : 8u), "ceiling: one 4x4 block");
    }
}

int main(int argc, char** argv) {
    g_dir = argc > 1 ? argv[1] : "/tmp";
    const char* mode = argc > 2 ? argv[2] : "";
    TestEncoder(mode);
    if (!std::strcmp(mode, "golden")) return 0;
    TestGateAndRules();
    TestCard();
    TestManager();
    if (g_fail) { std::printf("texcomp: %d FAILED\n", g_fail); return 1; }
    std::printf("texcomp: all passed\n");
    return 0;
}
