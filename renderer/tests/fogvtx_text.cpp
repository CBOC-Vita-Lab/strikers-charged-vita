// MSC_VGX_FOGVTX: the generated texts. A fogged fragment key with FOG_LINEAR reads one varying and one multiply-add where the plain one reads gl_FragCoord.z and divides; a vertex key
// with VSK_FOGZ writes that varying and nothing else differs; keys without the new bits give the texts they gave before. Prints a static count of the fog part, as the 1.3 changelog counts
// the clamps of FS_TRIM (operations of the GLSL, not the console compiler's instructions: the compiler's own count needs the console's libshacccg, see native/tools/gxpcount).
#include "../src/vgx_shadergen.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace vgx;
static int g_fail = 0;
static void Check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; } }
static size_t Count(const std::string& s, const char* t) { size_t n = 0, p = 0; while ((p = s.find(t, p)) != std::string::npos) { ++n; p += std::strlen(t); } return n; }

static FsKey Fs(uint8_t fog) {
    FsKey k;
    std::memset(static_cast<void*>(&k), 0, sizeof k);
    k.numStages = 1; k.numTexGens = 1; k.fog = fog;
    k.stage[0].color = 0x0A0F0; // any: d = texture
    k.stage[0].texEnable = 1; k.stage[0].rasChan = 0;
    return k;
}
static VsKey Vs(uint8_t z) {
    VsKey k;
    std::memset(static_cast<void*>(&k), 0, sizeof k);
    k.numTexGens = 1; k.numChans = 1;
    k.tg[7].pad[1] = z;
    return k;
}
// the fog part: the lines from "float fg" to the mix
static std::string FogPart(const std::string& t) {
    const size_t a = t.find("float fg"), b = t.find("fin.rgb = mix(");
    return a == std::string::npos || b == std::string::npos ? std::string() : t.substr(a, b - a);
}

int main() {
    for (int fsel : {2, 4, 5, 6, 7}) {
        for (int ortho = 0; ortho < 2; ++ortho) {
            const uint8_t plain = static_cast<uint8_t>(fsel | (ortho ? 8 : 0));
            ShaderSource a, b;
            GenerateFragmentShader(Fs(plain), a);
            GenerateFragmentShader(Fs(static_cast<uint8_t>(plain | FOG_LINEAR)), b);
            const std::string pa = FogPart(a.text), pb = FogPart(b.text);
            Check(!pa.empty() && !pb.empty(), "fog part found");
            Check(a.text.find("v_fz") == std::string::npos, "the plain fog text has no v_fz");
            Check(a.text.find("gl_FragCoord.z") != std::string::npos, "the plain fog text reads gl_FragCoord.z");
            Check(b.text.find("varying float v_fz;") != std::string::npos, "the linear text declares v_fz");
            Check(b.text.find("gl_FragCoord") == std::string::npos, "the linear text does not read gl_FragCoord");
            Check(b.text.find(" / ") == std::string::npos || FogPart(b.text).find(" / ") == std::string::npos, "no division in the linear fog part");
            // everything outside the fog part and the declaration is the same
            std::string ra = a.text, rb = b.text;
            ra.erase(ra.find(pa), pa.size());
            rb.erase(rb.find(pb), pb.size());
            const size_t d = rb.find("varying float v_fz;\n");
            Check(d != std::string::npos, "declaration found");
            if (d != std::string::npos) rb.erase(d, 20);
            Check(ra == rb, "the rest of the text is the same");
            if (fsel == 2) {
                std::printf("fog type %d %s: plain fog part %zu lines: %zu '/', %zu 'max', %zu 'clamp', 1 gl_FragCoord read; linear: %zu '/', %zu 'max', %zu 'clamp', 1 varying read\n", fsel, ortho ? "ortho" : "persp",
                            Count(pa, "\n"), Count(pa, " / "), Count(pa, "max("), Count(pa, "clamp("), Count(pb, " / "), Count(pb, "max("), Count(pb, "clamp("));
                if (!ortho) std::printf("  plain : %s  linear: %s", pa.c_str(), pb.c_str());
            }
        }
    }
    ShaderSource v0, v1;
    GenerateVertexShader(Vs(0), v0);
    GenerateVertexShader(Vs(VSK_FOGZ), v1);
    Check(v0.text.find("v_fz") == std::string::npos, "a plain vertex key has no v_fz");
    Check(v1.text.find("varying float v_fz;") != std::string::npos && v1.text.find("  v_fz = eye.z;\n") != std::string::npos, "the VSK_FOGZ vertex text writes v_fz");
    std::string r0 = v0.text, r1 = v1.text;
    for (const char* t : {"varying float v_fz;\n", "  v_fz = eye.z;\n"}) { const size_t p = r1.find(t); if (p != std::string::npos) r1.erase(p, std::strlen(t)); }
    Check(r0 == r1, "the vertex texts differ in nothing else");
    std::printf("vertex: the flagged text adds %zu bytes (a declaration and one copy of the eye depth, already computed)\n", v1.text.size() - v0.text.size());
    if (g_fail) { std::printf("fogvtx_text: %d FAILURES\n", g_fail); return 1; }
    std::printf("fogvtx_text: PASS\n");
    return 0;
}
