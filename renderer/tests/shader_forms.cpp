// 1.3 MSC_VGX_FS_HALF and MSC_VGX_TEX2D: the generator's text with the forms off and on, over keys of every generator feature (a fixed pseudo-random set in the shapes of
// the other generator tests: indirect, compare stages, alpha tests, fog, Z-freeze, swaps, konst selects; palettes, buckets, skinning, lights, post matrices, colour texgens).
//   shader_forms           checks: the off text is the 1.2 text (hash below); the half text keeps coordinates, depth, fog and the alpha test in float and writes every type change
//                          out; half arithmetic, emulated operation by operation, stays inside FsHalfBound and inside one 8-bit step for an admitted key
//   shader_forms dump DIR  writes the texts of every form for shader_forms_gl.py (Mesa, vitaGL's preprocessor)
//   shader_forms golden    prints the hash of the off texts. Built with -DGOLDEN_ONLY against a tree without the forms it gives the constant below (the proof that nothing moved).
#include "vgx_shadergen.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vgx;

constexpr uint64_t kGolden = 0xa7a22790c8f9df07ull; // GOLDEN (the 1.2 generator's texts over these keys)
constexpr int kKeys = 3000;

static int g_fail = 0;
static void Check(bool ok, const char* what, int i = -1) {
    if (!ok) { if (g_fail < 20) std::printf("FAIL: %s (%d)\n", what, i); ++g_fail; }
}

struct Rng {
    uint64_t s;
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>(s >> 33); }
    uint32_t n(uint32_t m) { return next() % m; }
    bool chance(uint32_t pct) { return n(100) < pct; }
    double unit() { return next() / 2147483648.0; }
};

// wild: anything the registers can say. Otherwise the shapes materials are made of.
static uint32_t ColourReg(Rng& r, bool wild, int stage, int stages) {
    uint32_t a, b, c, d, bias = 0, op = 0, cl = 1, scale = 0, dest = 0;
    if (wild) {
        a = r.n(16); b = r.n(16); c = r.n(16); d = r.n(16); bias = r.n(4); op = r.n(2); cl = r.n(2); scale = r.n(4); dest = r.n(4);
    } else {
        static const uint32_t arg[] = {15, 8, 10, 14, 0, 2, 4, 6, 8, 10, 15, 12};
        a = arg[r.n(12)]; b = arg[r.n(12)]; c = arg[r.n(12)]; d = r.chance(70) ? 15 : arg[r.n(12)];
        scale = r.chance(10) ? 1 : 0;
        dest = stage == stages - 1 ? 0 : r.n(2);
    }
    return d | c << 4 | b << 8 | a << 12 | bias << 16 | op << 18 | cl << 19 | scale << 20 | dest << 22;
}
static uint32_t AlphaReg(Rng& r, bool wild, int stage, int stages) {
    uint32_t a, b, c, d, bias = 0, op = 0, cl = 1, scale = 0, dest = 0;
    if (wild) {
        a = r.n(8); b = r.n(8); c = r.n(8); d = r.n(8); bias = r.n(4); op = r.n(2); cl = r.n(2); scale = r.n(4); dest = r.n(4);
    } else {
        static const uint32_t arg[] = {7, 4, 5, 6, 0, 1, 2, 4, 5, 7};
        a = arg[r.n(10)]; b = arg[r.n(10)]; c = arg[r.n(10)]; d = r.chance(70) ? 7 : arg[r.n(10)];
        dest = stage == stages - 1 ? 0 : r.n(2);
    }
    return d << 4 | c << 7 | b << 10 | a << 13 | bias << 16 | op << 18 | cl << 19 | scale << 20 | dest << 22;
}
static uint8_t Swap(Rng& r) { return r.chance(80) ? 0xE4 : static_cast<uint8_t>(r.n(256)); }

static FsKey MakeFs(Rng& r, uint8_t texGens, uint8_t proj) {
    FsKey k;
    std::memset(static_cast<void*>(&k), 0, sizeof k);
    const bool wild = r.chance(35);
    k.numStages = static_cast<uint8_t>(r.chance(70) ? 1 + r.n(4) : 1 + r.n(16));
    k.numTexGens = texGens;
    k.texProjMask = proj;
    if (r.chance(30)) { k.alphaTestOn = 1; k.alphaTest = static_cast<uint8_t>(r.n(256)); }
    if (r.chance(25)) k.fog = static_cast<uint8_t>((2 + r.n(6)) | (r.n(2) << 3));
    k.dstAlpha = r.chance(10);
    k.zfreeze = r.chance(10);
    if (texGens && r.chance(10)) {
        k.numIndStages = static_cast<uint8_t>(1 + r.n(2));
        for (int i = 0; i < k.numIndStages; ++i) { k.ind[i].texMap = static_cast<uint8_t>(r.n(8)); k.ind[i].texCoord = static_cast<uint8_t>(r.n(texGens)); }
    }
    for (int s = 0; s < k.numStages; ++s) {
        FsKey::Stage& st = k.stage[s];
        st.color = ColourReg(r, wild, s, k.numStages);
        st.alpha = AlphaReg(r, wild, s, k.numStages);
        st.texEnable = texGens && r.chance(70);
        st.texMap = static_cast<uint8_t>(r.n(8));
        st.texCoord = static_cast<uint8_t>(texGens ? r.n(texGens) : 0);
        static const uint8_t ras[] = {0, 0, 0, 1, 5, 6, 7};
        st.rasChan = ras[r.n(7)];
        st.kcsel = static_cast<uint8_t>(r.n(32));
        st.kasel = static_cast<uint8_t>(r.n(32));
        st.rasSwap = Swap(r);
        st.texSwap = Swap(r);
        if (k.numIndStages && r.chance(40)) {
            const uint32_t mid = r.n(12), sw = r.n(7), tw = r.n(7);
            st.ind = r.n(k.numIndStages) | r.n(4) << 2 | r.n(8) << 4 | mid << 9 | sw << 13 | tw << 16 | r.n(2) << 20;
        }
    }
    return k;
}
static VsKey MakeVs(Rng& r, uint8_t& texGens, uint8_t& proj) {
    VsKey k;
    std::memset(static_cast<void*>(&k), 0, sizeof k);
    texGens = static_cast<uint8_t>(r.n(9));
    proj = 0;
    k.numTexGens = texGens;
    k.numChans = static_cast<uint8_t>(r.n(3));
    const uint32_t pv = r.n(4);
    k.perVtxPos = static_cast<uint8_t>(pv == 3 ? 2 : pv);
    if (k.perVtxPos == 2 && r.chance(50)) k.palRows = static_cast<uint8_t>(6 + 3 * r.n(9));
    for (int i = 0; i < 4; ++i) {
        VsKey::Chan& c = k.chan[i];
        c.enable = r.chance(60); c.matSrcVtx = r.n(2); c.ambSrcVtx = r.n(2); c.diffuse = static_cast<uint8_t>(r.n(3)); c.attn = static_cast<uint8_t>(r.n(3));
        c.lightMask = static_cast<uint8_t>(r.chance(50) ? 0 : r.n(256));
    }
    if (k.perVtxPos == 0 && r.chance(10)) k.chan[0].skin = 1;
    for (int i = 0; i < texGens; ++i) {
        VsKey::TexGen& t = k.tg[i];
        t.type = static_cast<uint8_t>(r.chance(10) ? 2 + r.n(2) : 0);
        if (t.type >= 2) { t.src = 2; continue; }
        t.src = static_cast<uint8_t>(r.chance(70) ? 5 + r.n(8) : r.n(5));
        t.proj = r.chance(25);
        t.inputAbc1 = r.n(2);
        t.post = static_cast<uint8_t>(r.chance(70) ? 0 : 1 + r.n(2));
        t.perVtxMtx = r.chance(15);
        if (t.proj) proj |= static_cast<uint8_t>(1u << i);
    }
    return k;
}

static uint64_t GoldenHash() {
    Rng r{12345};
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    for (int i = 0; i < kKeys; ++i) {
        uint8_t tg, pm;
        const VsKey v = MakeVs(r, tg, pm);
        const FsKey f = MakeFs(r, tg, pm);
        ShaderSource sv, sf;
        GenerateVertexShader(v, sv);
        GenerateFragmentShader(f, sf);
        XXH3_64bits_update(st, sv.text.data(), sv.text.size());
        XXH3_64bits_update(st, sf.text.data(), sf.text.size());
    }
    const uint64_t h = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    return h;
}

#ifndef GOLDEN_ONLY
static double RoundHalf(double x) {     // to half's 11 significant bits, nearest
    if (x == 0.0) return 0.0;
    int e;
    const double m = std::frexp(x, &e);
    return std::ldexp(std::nearbyint(m * 2048.0), e - 11);
}
// One 4-component TEV run as the generator writes it. half = false: every value exact. half = true: every value taken in and every operation rounded. order: which of the
// two shapes of mix the compiler may choose.
struct Inputs { double reg[4][4], konst[4][4], tex[16][4], ras[2][4]; };
struct Tev {
    bool half;
    int order;
    double R(double x) const { return half ? RoundHalf(x) : x; }
    double Mix(double a, double b, double c) const {
        return order ? R(a + R(c * R(b - a))) : R(R(a * R(1.0 - c)) + R(b * c));
    }
    double Stage(double a, double b, double c, double d, uint32_t reg) const {
        const uint32_t bias = bits(reg, 16, 2), op = bits(reg, 18, 1), cl = bits(reg, 19, 1), scale = bits(reg, 20, 2);
        double v = R(op ? d - Mix(a, b, c) : d + Mix(a, b, c));
        if (bias == 1) v = R(v + 0.5);
        if (bias == 2) v = R(v - 0.5);
        static const double s[4] = {1, 2, 4, 0.5};
        v = R(v * s[scale]);
        const double lo = cl ? 0.0 : -4.0157, hi = cl ? 1.0 : 4.0118;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    void Run(const FsKey& k, const Inputs& in, double out[4]) const {
        double reg[4][4], tex[4] = {1, 1, 1, 1}, ras[4] = {0, 0, 0, 0}, kc[3] = {1, 1, 1}, ka = 1;
        for (int i = 0; i < 4; ++i)
            for (int c = 0; c < 4; ++c) reg[i][c] = R(in.reg[i][c]);
        for (int s = 0; s < k.numStages; ++s) {
            const FsKey::Stage& st = k.stage[s];
            if (st.texEnable)
                for (int c = 0; c < 4; ++c) tex[c] = R(in.tex[s][c]);
            for (int c = 0; c < 4; ++c) ras[c] = st.rasChan < 2 ? R(in.ras[st.rasChan][c]) : 0.0;
            const uint32_t tc = st.color, ta = st.alpha;
            for (int c = 0; c < 3; ++c) {
                if (st.kcsel < 8) kc[c] = (8 - st.kcsel) / 8.0;
                else if (st.kcsel < 12) kc[c] = 0;
                else { static const int comp[5][3] = {{0, 1, 2}, {0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}}; kc[c] = R(in.konst[st.kcsel & 3][comp[(st.kcsel - 12) >> 2][c]]); }
            }
            ka = st.kasel < 8 ? (8 - st.kasel) / 8.0 : (st.kasel < 16 ? 0.0 : R(in.konst[st.kasel & 3][(st.kasel - 16) >> 2]));
            auto carg = [&](uint32_t a, int c) -> double {
                switch (a & 15) {
                case 0: case 2: case 4: case 6: return reg[(a & 15) / 2][c];
                case 1: case 3: case 5: case 7: return reg[(a & 15) / 2][3];
                case 8: return tex[c];
                case 9: return tex[3];
                case 10: return ras[c];
                case 11: return ras[3];
                case 12: return 1.0;
                case 13: return 0.5;
                case 14: return kc[c];
                default: return 0.0;
                }
            };
            auto aarg = [&](uint32_t a) -> double {
                switch (a & 7) {
                case 0: case 1: case 2: case 3: return reg[a & 7][3];
                case 4: return tex[3];
                case 5: return ras[3];
                case 6: return ka;
                default: return 0.0;
                }
            };
            double rc[3];
            for (int c = 0; c < 3; ++c) rc[c] = Stage(carg(bits(tc, 12, 4), c), carg(bits(tc, 8, 4), c), carg(bits(tc, 4, 4), c), carg(bits(tc, 0, 4), c), tc);
            const double ra = Stage(aarg(bits(ta, 13, 3)), aarg(bits(ta, 10, 3)), aarg(bits(ta, 7, 3)), aarg(bits(ta, 4, 3)), ta);
            for (int c = 0; c < 3; ++c) reg[bits(tc, 22, 2)][c] = rc[c];
            reg[bits(ta, 22, 2)][3] = ra;
        }
        const FsKey::Stage& last = k.stage[k.numStages - 1];
        for (int c = 0; c < 4; ++c) {
            const double v = reg[c < 3 ? bits(last.color, 22, 2) : bits(last.alpha, 22, 2)][c];
            out[c] = v < 0 ? 0 : (v > 1 ? 1 : v);
        }
    }
};
static void MakeInputs(Rng& r, Inputs& in, bool wide) {
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c) {
            in.reg[i][c] = wide ? (r.unit() * 2 - 1) * 4.0118 : r.n(256) / 255.0;
            in.konst[i][c] = r.n(256) / 255.0;
            in.ras[i & 1][c] = r.unit();
        }
    for (int s = 0; s < 16; ++s)
        for (int c = 0; c < 4; ++c) in.tex[s][c] = r.n(256) / 255.0;
}
// Half must not reach a line that holds a coordinate, the depth, fog or the alpha test's value; and no bare float literal meets a half operand.
static void CheckHalfText(const std::string& t, const FsKey& k, int i) {
    size_t p = 0;
    while (p < t.size()) {
        const size_t e = t.find('\n', p);
        const std::string l = t.substr(p, e - p);
        p = e + 1;
        const bool halfLine = l.find("HV") != std::string::npos || l.find("HF") != std::string::npos;
        if (l[0] == '#') continue;
        if (halfLine) {
            if (l.find('*') != std::string::npos && l.find("u_tev[") == std::string::npos) Check(false, ("`*` on half (vitaGL's vglMul has no half form): " + l).c_str(), i);
            for (const char* no : {"uv", "itc", "fg", "ai ", "gl_FragCoord", "VGX_FRAGDEPTH"})
                if (l.find(no) != std::string::npos && l.find("texture2D") == std::string::npos) Check(false, ("half on a float-only line: " + l).c_str(), i);
            if (l.find("kc = ") != std::string::npos || l.find("ka = ") != std::string::npos || l.find("u_tev[") != std::string::npos || l.find("texture2D") != std::string::npos) continue;
            // (a literal only as the argument of a cast)
            std::string s = l;
            for (const char* c : {"HV3(", "HV4(", "HF("})
                for (size_t q = 0; (q = s.find(c, q)) != std::string::npos;) { const size_t close = s.find(')', q); s.erase(q, close - q + 1); }
            for (size_t q = 0; q + 2 < s.size(); ++q)
                if (std::isdigit(static_cast<unsigned char>(s[q])) && s[q + 1] == '.' && std::isdigit(static_cast<unsigned char>(s[q + 2]))) Check(false, ("bare literal: " + l).c_str(), i);
        }
    }
    Check(t.find("vec3 a =") == std::string::npos && t.find("float aa =") == std::string::npos, "float TEV arguments in a half text", i);
    for (int g = 0; g < k.numTexGens; ++g) Check(t.find("vec2 uv" + std::to_string(g)) != std::string::npos, "a coordinate left float", i);
    if (k.alphaTestOn) Check(t.find("float ai = ") != std::string::npos, "alpha test value float", i);
    if (k.fog) Check(t.find("float fg = ") != std::string::npos, "fog float", i);
    Check(t.find("vec4 fin = vec4(clamp(HV4(") != std::string::npos, "final value", i);
    // A scale has no `*` here: each of the stage's scales must be there as additions or as the half lerp.
    auto count = [&](const char* what) { int n = 0; for (size_t q = 0; (q = t.find(what, q)) != std::string::npos; ++q) ++n; return n; };
    int dc = 0, da = 0, hc = 0, ha = 0;
    for (int s = 0; s < k.numStages; ++s) {
        const uint32_t sc = bits(k.stage[s].color, 20, 2), sa = bits(k.stage[s].alpha, 20, 2);
        dc += sc == 1 ? 1 : (sc == 2 ? 2 : 0);
        da += sa == 1 ? 1 : (sa == 2 ? 2 : 0);
        hc += sc == 3;
        ha += sa == 3;
    }
    Check(count("rc = rc + rc;") == dc && count("ra = ra + ra;") == da, "scales by doubling", i);
    Check(count("rc = mix(HV3(0.0), rc, HV3(0.5));") == hc && count("ra = mix(HF(0.0), ra, HF(0.5));") == ha, "scales by a half", i);
}
static void Save(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::perror(path.c_str()); std::exit(2); }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}
static void Dump(const char* dir, int n) {
    Rng r{12345};
    static const struct { const char* name; uint8_t fs, vs; } kForms[] = {{"off", 0, 0}, {"half", FSP_HALF, 0}, {"halfvar", FSP_HALF | FSP_HALFVAR, 0}, {"tex2", FSP_TEX2, VSP_TEX2},
                                                                         {"halfvar_tex2", FSP_HALF | FSP_HALFVAR | FSP_TEX2, VSP_TEX2}, {"trim", FSP_TRIM, 0}, {"half_trim", FSP_HALF | FSP_TRIM, 0}};
    for (int i = 0; i < n; ++i) {
        uint8_t tg, pm;
        VsKey v = MakeVs(r, tg, pm);
        FsKey f = MakeFs(r, tg, pm);
        const bool eligible = FsHalfEligible(f);
        bool colourGen = false;
        for (int g = 0; g < tg; ++g) colourGen |= v.tg[g].type >= 2;
        // MSC_VGX_VS_TRIM: the same key with every listed light's attenuations 1 (voff: the key's text, with uniforms that hold those facts; vtrim: the text that relies on them)
        {
            uint8_t mask = 0;
            for (const auto& c : v.chan) mask |= c.lightMask;
            VsKey vt = v;
            vt.chan[1].pad = vt.chan[2].pad = mask;
            VsKey vu = vt;   // (colour 1 unread: its fragment shader leaves it out; links, and draws what the same shader draws with colour 1 written)
            vu.tg[7].pad[0] = 1;
            VsKey vc = vt;   // (spot term only: the distance divisor stays in the text)
            vc.chan[2].pad = 0;
            ShaderSource s0, s1, s2, s3;
            GenerateVertexShader(vu, s3);
            // (a fragment shader that shows the lit colours directly: a random TEV chain mostly hides them)
            const std::string both = "precision mediump float;\nvarying vec4 v_col0;\nvarying vec4 v_col1;\nvoid main() { gl_FragColor = vec4(v_col0.r, v_col0.a, v_col0.g * 0.5 + v_col1.r * 0.5, 1.0); }\n";
            const std::string one = "precision mediump float;\nvarying vec4 v_col0;\nvoid main() { gl_FragColor = vec4(v_col0.r, v_col0.a, v_col0.b, 1.0); }\n";
            GenerateVertexShader(v, s0);
            GenerateVertexShader(vt, s1);
            GenerateVertexShader(vc, s2);
            auto put = [&](const char* name, const std::string& text, const char* ext) {
                const std::string p = std::string(dir) + "/" + std::to_string(i) + "." + name + ext;
                if (FILE* o = std::fopen(p.c_str(), "wb")) { std::fwrite(text.data(), 1, text.size(), o); std::fclose(o); }
            };
            if (!colourGen) {
                put("voff", s0.text, ".vs"); put("voff", both, ".fs");
                put("vtrim", s1.text, ".vs"); put("vtrim", both, ".fs");
                put("vcos", s2.text, ".vs"); put("vcos", both, ".fs");
                put("vunread", s3.text, ".vs"); put("vunread", one, ".fs");
            }
        }
        for (const auto& form : kForms) {
            if ((form.fs & FSP_HALF) && !eligible) continue;
            FsKey f2 = f;
            f2.prec = form.fs;
            VsKey v2 = v;
            v2.chan[0].pad = form.vs;
            ShaderSource sv, sf;
            GenerateVertexShader(v2, sv);
            GenerateFragmentShader(f2, sf);
            const std::string base = std::string(dir) + "/" + std::to_string(i) + "." + form.name + ((form.fs & FSP_TEX2) && colourGen ? ".nolink" : "");
            Save(base + ".vs", sv.text);
            Save(base + ".fs", sf.text);
        }
        if (v.perVtxPos == 2) { // 1.3 MSC_VGX_PALMERGE: the same key with 60 palette position rows (a palette run's record: u_pal[120])
            VsKey v6 = v;
            v6.palRows = 60;
            ShaderSource sv, sf;
            GenerateVertexShader(v6, sv);
            GenerateFragmentShader(f, sf);
            const std::string base = std::string(dir) + "/" + std::to_string(i) + ".pal60";
            Save(base + ".vs", sv.text);
            Save(base + ".fs", sf.text);
        }
    }
}
#endif

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "golden")) { std::printf("%016llx\n", static_cast<unsigned long long>(GoldenHash())); return 0; }
#ifndef GOLDEN_ONLY
    if (argc > 2 && !std::strcmp(argv[1], "dump")) { Dump(argv[2], argc > 3 ? std::atoi(argv[3]) : 300); return 0; }
    if (argc > 1 && !std::strcmp(argv[1], "switch")) {
        // (run with MSC_VGX_FS_HALF / MSC_VGX_TEX2D set: the bits come from the switches and the eligibility alone)
        Rng r{777};
        unsigned on = 0;
        for (int i = 0; i < kKeys; ++i) {
            uint8_t tg, pm;
            const VsKey v = MakeVs(r, tg, pm);
            const FsKey f = MakeFs(r, tg, pm);
            const FsKey a = ShaderSwitchKey(f);
            const VsKey b = ShaderSwitchKey(v);
            const int want = (FsHalfMode() && FsHalfEligible(f) ? FSP_HALF : 0) | (FsHalfMode() == 2 && FsHalfEligible(f) ? FSP_HALFVAR : 0) | (Tex2dOn() ? FSP_TEX2 : 0);
            Check(a.prec == want, "key bits", i);
            Check((b.chan[0].pad != 0) == Tex2dOn(), "vertex key bit", i);
            FsKey c = a;
            c.prec = 0;
            Check(!std::memcmp(&c, &f, sizeof f), "only the bits change", i);
            ShaderSource x, y;
            GenerateFragmentShader(f, x);
            GenerateFragmentShader(a, y);
            if (a.prec) ++on;
            bool usesTex2 = (a.prec & FSP_TEX2) && tg && pm != ((1u << tg) - 1u);
            Check((x.text != y.text) == ((a.prec & FSP_HALF) != 0 || usesTex2), "the text differs exactly when the form does", i);
        }
        std::printf("switches: %u of %d keys take a form bit\n", on, kKeys);
        return g_fail ? 1 : 0;
    }
    const uint64_t g = GoldenHash();
    if (g != kGolden) { std::printf("FAIL: the off texts moved: %016llx\n", static_cast<unsigned long long>(g)); ++g_fail; }
    Rng r{12345}, inp{99};
    unsigned eligible = 0, plainShapes = 0, plainEligible = 0, refused = 0;
    double worstRatio = 0, worstLevels = 0, worstWide = 0;
    for (int i = 0; i < kKeys; ++i) {
        uint8_t tg, pm;
        const VsKey v = MakeVs(r, tg, pm);
        const FsKey f = MakeFs(r, tg, pm);
        (void)v;
        bool special = f.numIndStages != 0;
        for (int s = 0; s < f.numStages; ++s) special |= f.stage[s].ind || bits(f.stage[s].color, 16, 2) == 3 || bits(f.stage[s].alpha, 16, 2) == 3;
        const float bound1 = FsHalfBound(f, 0.0f, 1.0f), bound4 = FsHalfBound(f, -4.0157f, 4.0118f);
        Check(special == (bound1 < 0), "indirect and compare stages are refused", i);
        Check(!special || !FsHalfEligible(f), "refused key not eligible", i);
        refused += special;
        if (f.numStages <= 4 && !special) ++plainShapes;
        ShaderSource sf;
        if (FsHalfEligible(f)) {
            ++eligible;
            plainEligible += f.numStages <= 4;
            FsKey h = f;
            h.prec = FSP_HALF;
            GenerateFragmentShader(h, sf);
            CheckHalfText(sf.text, f, i);
            Check(sf.text.find("#define HV4 half4") != std::string::npos, "the half types", i);
            FsKey h2 = h;
            h2.prec |= FSP_HALFVAR;
            ShaderSource sf2;
            GenerateFragmentShader(h2, sf2);
            for (int c = 0; c < 2; ++c) if (sf.text.find("v_col" + std::to_string(c)) != std::string::npos)
                Check(sf2.text.find("varying HV4 v_col" + std::to_string(c)) != std::string::npos, "half varying", i);
        }
        if (special) continue;
        // Emulated half against exact values.
        for (int pass = 0; pass < 2; ++pass) {
            const double bound = pass ? bound4 : bound1;
            for (int n = 0; n < 200; ++n) {
                Inputs in;
                MakeInputs(inp, in, pass != 0);
                double a[4], b[4];
                Tev{false, 0}.Run(f, in, a);
                Tev{true, n & 1}.Run(f, in, b);
                for (int c = 0; c < 4; ++c) {
                    const double e = std::fabs(a[c] - b[c]);
                    if (bound > 0 && e / bound > worstRatio) worstRatio = e / bound;
                    Check(e <= bound * (1 + 1e-6) + 1e-9, "emulated error beyond FsHalfBound", i);
                    if (FsHalfEligible(f)) {
                        if (!pass && e * 255 > worstLevels) worstLevels = e * 255;
                        if (pass && e * 255 > worstWide) worstWide = e * 255;
                        Check(e * 255 <= (pass ? 4.0 : 1.0), "emulated error of an admitted key beyond its steps", i);
                    }
                }
            }
        }
    }
    // Pinned shapes: the everyday ones are admitted, what half cannot carry is not.
    {
        FsKey k;
        std::memset(static_cast<void*>(&k), 0, sizeof k);
        k.numStages = 1; k.numTexGens = 1;
        k.stage[0].texEnable = 1; k.stage[0].rasChan = 0; k.stage[0].rasSwap = k.stage[0].texSwap = 0xE4;
        k.stage[0].color = 15u | 10u << 4 | 8u << 8 | 15u << 12 | 1u << 19;     // tex x ras
        k.stage[0].alpha = 7u << 4 | 5u << 7 | 4u << 10 | 7u << 13 | 1u << 19;
        Check(FsHalfEligible(k), "tex x ras admitted");
        FsKey c = k;
        c.stage[0].color |= 3u << 16;
        Check(FsHalfBound(c, 0.0f, 1.0f) < 0, "compare stage refused");
        FsKey d = k;
        d.numIndStages = 1;
        Check(FsHalfBound(d, 0.0f, 1.0f) < 0, "indirect refused");
        FsKey s = k;
        s.numStages = 16;
        for (int i = 1; i < 16; ++i) s.stage[i] = k.stage[0];
        for (int i = 0; i < 16; ++i) s.stage[i].color = 15u | 8u << 4 | 0u << 8 | 15u << 12 | 1u << 19;   // sixteen multiplies by the same texture
        Check(!FsHalfEligible(s), "a long chain is not admitted");
    }
    std::printf("shader forms: %d keys, %u admitted, %u with indirect or compare; of the %u plain keys of up to 4 stages %u admitted; emulated error at most %.2f of the bound, %.3f steps (registers 0..1), %.3f (+-4)\n",
                kKeys, eligible, refused, plainShapes, plainEligible, worstRatio, worstLevels, worstWide);
    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::printf("shader forms: passed\n");
#endif
    return 0;
}
