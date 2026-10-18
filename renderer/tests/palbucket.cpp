// W5 LOOK2: the vertex shader of a skinned draw in its palette buckets (vgx_shadergen.cpp).
//   palbucket dump     prints the generated vertex shaders (sizes and text) of a set of keys: run it against the tree before the buckets and after, and diff:
//                      the keys that existed before (perVtxPos 0, 1, 2 with palRows 0) must come out byte for byte the same (the seeds hold the compiled shaders by source)
//   palbucket          checks the bucket shaders themselves (sizes, row clamp, the normal rows' offset)
#include "vgx_shadergen.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace vgx;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}
static bool Has(const ShaderSource& s, const char* t) { return s.text.find(t) != std::string::npos; }

static VsKey MakeKey(int perVtxPos, int palRows, int lit) {
    VsKey k;
    std::memset(&k, 0, sizeof k);
    k.numTexGens = 1;
    k.numChans = 1;
    k.perVtxPos = static_cast<uint8_t>(perVtxPos);
#ifdef HAVE_PALROWS
    k.palRows = static_cast<uint8_t>(palRows);
#else
    (void)palRows;
#endif
    k.chan[0].enable = lit ? 1 : 0;
    k.chan[0].lightMask = lit ? 3 : 0;
    k.chan[0].diffuse = lit ? 2 : 0;
    k.chan[2].enable = 0;
    k.tg[0].src = 5;
    return k;
}

int main(int argc, char** argv) {
    const bool dump = argc > 1 && !std::strcmp(argv[1], "dump");
    for (int lit = 0; lit < 2; ++lit)
        for (int pv = 0; pv <= 2; ++pv) {
            ShaderSource s;
            GenerateVertexShader(MakeKey(pv, 0, lit), s);
            if (dump) std::printf("=== perVtxPos %d lit %d: nPal %d usesPalette %d keyhash-free text %zu bytes\n%s\n", pv, lit, s.nPal, s.usesPalette, s.text.size(), s.text.c_str());
        }
#ifdef HAVE_PALROWS
    if (dump) return 0;
    for (int lit = 0; lit < 2; ++lit) {
        const int sizes[] = {4, 8, 16};
        for (int P : sizes) {
            ShaderSource s;
            GenerateVertexShader(MakeKey(2, P, lit), s);
            char buf[96];
            Check(s.nPal == 2 * P, "nPal is twice the position rows");
            std::snprintf(buf, sizeof buf, "uniform vec4 u_pal[%d];", 2 * P);
            Check(Has(s, buf), buf);
            std::snprintf(buf, sizeof buf, "if (pi > %d) pi = %d;", P - 3, P - 3);
            Check(Has(s, buf), buf);
            if (lit) {
                std::snprintf(buf, sizeof buf, "int ni = %d + pi;", P);
                Check(Has(s, buf), buf);
            }
            Check(!Has(s, "u_pal[60]") && !Has(s, "u_pal[96]"), "no full-size array in a bucket");
        }
        ShaderSource s30;
        GenerateVertexShader(MakeKey(2, 0, lit), s30);
        Check(s30.nPal == 60 && Has(s30, "uniform vec4 u_pal[60];") && Has(s30, "if (pi > 27) pi = 27;"), "palRows 0 is the whole short palette (60 rows, row <= 27)");
        ShaderSource sf;
        GenerateVertexShader(MakeKey(1, 0, lit), sf);
        Check(sf.nPal == 96 && Has(sf, "uniform vec4 u_pal[96];"), "perVtxPos 1 is the full palette");
        Check(PalPosRows(MakeKey(2, 0, 1)) == 30 && PalPosRows(MakeKey(2, 8, 1)) == 8 && PalPosRows(MakeKey(1, 0, 1)) == 0, "PalPosRows");
    }
    std::printf("%s\n", g_fail ? "palbucket: FAILED" : "palbucket: ok");
    return g_fail != 0;
#else
    return 0;
#endif
}
