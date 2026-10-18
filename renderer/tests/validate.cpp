#include <cstdio>
#include <cstdlib>

static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

#if __has_include("../src/vgx_validate.h")
#include "../src/vgx_validate.h"
#include <limits>

using namespace vgx;
using namespace vgx::validate;

int main() {
    Check(!ParseWindow(nullptr).enabled && !ParseWindow("").enabled && !ParseWindow("0").enabled, "default/off does not enable validation");
    Check(ParseWindow("12").Contains(12) && !ParseWindow("12").Contains(13), "single exact frame");
    Check(ParseWindow("0:0").Contains(0), "explicit frame zero");
    Check(ParseWindow("12:15").Contains(15), "four-frame interval accepted");
    for (const char* s : {"1:5", "2:1", "-1", "1:2junk", " 1", "1:", "1:2:3", "18446744073709551616"})
        Check(ParseWindow(s).invalid && !ParseWindow(s).enabled, "invalid/unbounded selector rejected");
    Check(ParseWindow("18446744073709551615").Contains(UINT64_MAX), "max uint64 parsed without overflow");
    Check(Hash("hello", 5) == UINT64_C(0xa430d84680aabd0b), "stable FNV-1a known answer");

    std::vector<uint8_t> vertices(5 * 16);
    for (unsigned i = 0; i < 5; ++i) {
        float pos[] = {float(i), float(i + 1), float(i + 2), 3.0f};
        std::memcpy(vertices.data() + i * 16, pos, sizeof pos);
    }
    std::vector<uint16_t> indices = {0, 2, 3, 4};
    std::vector<float> uniforms(16 + 24 + 384, 0.0f);
    DrawRec draw;
    draw.vtxFirst = 2; draw.vtxCount = 3; draw.idxOffset = 1; draw.idxCount = 3;
    draw.ublock[UB_PROJ] = 0; draw.ublock[UB_MV] = 16; draw.ublock[UB_PAL] = 40;
    UniformRange ranges[] = {{UB_PROJ, 0, 16}, {UB_MV, 0, 24}, {UB_PAL, 0, 384}};
    auto inspect = [&]() { return Inspect(draw, {vertices.data(), vertices.size(), indices.data(), indices.size()},
                                         uniforms.data(), uniforms.size(), ranges, 3, true, 61); };
    Result good = inspect();
    Check(good.errors == 0 && good.references == 3 && good.minIndex == 2 && good.maxIndex == 4, "valid absolute indices and uniform ranges");
    Check(good.vertexBytes == 48 && good.indexBytes == 6 && good.uniformBytes == uniforms.size() * 4, "exact referenced byte accounting");
    Check(good.firstPosition[0] == f2u(2.0f), "bounded position sample is from first referenced vertex");
    Check(inspect().vertexHash == good.vertexHash && inspect().uniformHash == good.uniformHash, "repeatable content hashes");
    vertices[0] ^= 1;
    Check(inspect().vertexHash == good.vertexHash, "unreferenced buffer bytes excluded");
    vertices[32] ^= 1;
    Check(inspect().vertexHash != good.vertexHash, "referenced corruption changes vertex hash");
    vertices[32] ^= 1;
    uniforms[1] = 1;
    Check(inspect().uniformHash != good.uniformHash, "uniform corruption changes hash");
    uniforms[1] = 0;
    indices[1] = 5;
    Check(inspect().badVertexRefs == 1 && inspect().outsideDeclaredRefs == 1, "out-of-buffer index detected without read");
    indices[1] = 1;
    Check(inspect().badVertexRefs == 0 && inspect().outsideDeclaredRefs == 1, "in-buffer index outside declared draw detected");
    indices[1] = 2;
    draw.idxOffset = UINT32_MAX;
    Check(inspect().badIndexRange == 1 && inspect().references == 0, "invalid index range is not dereferenced");
    draw.idxOffset = 1; draw.idxCount = UINT32_MAX;
    Check(inspect().badIndexRange == 1, "index count overflow rejected");
    draw.idxCount = 3; draw.vtxOffset = UINT32_MAX;
    Check(inspect().badVertexRefs == 3, "vertex base overflow rejected");
    draw.vtxOffset = 0;
    draw.ublock[UB_PAL] = UINT32_MAX - 1;
    Check(inspect().badUniformRanges == 1, "uniform offset outside arena rejected");
    draw.ublock[UB_PAL] = kNoBlock;
    Check(inspect().badUniformRanges == 1, "missing active uniform block rejected");
    draw.ublock[UB_PAL] = 40;
    uint32_t nan = 0x7fc00001, inf = 0x7f800000;
    std::memcpy(vertices.data() + 32, &nan, 4);
    std::memcpy(uniforms.data() + 2, &inf, 4);
    Result nonfinite = inspect();
    Check(nonfinite.nonfinitePositionRefs == 1 && nonfinite.nonfiniteUniforms == 1, "bitwise NaN/Inf detection survives fast-math");
    Check(nonfinite.nonfiniteByBlock[UB_PROJ] == 1 && nonfinite.nonfiniteByBlock[UB_PAL] == 0, "nonfinite uniforms identify the affected matrix block");
    float x = 2; std::memcpy(vertices.data() + 32, &x, 4); uniforms[2] = 0;
    float row = -1; std::memcpy(vertices.data() + 44, &row, 4);
    Check(inspect().badPaletteRefs == 1, "negative palette row detected");
    row = 3.5f; std::memcpy(vertices.data() + 44, &row, 4);
    Check(inspect().badPaletteRefs == 1, "fractional palette row detected");
    row = 63; std::memcpy(vertices.data() + 44, &row, 4);
    Check(inspect().badPaletteRefs == 0 && inspect().clampedPaletteRefs == 1, "legal hardware row clamped by shader is distinguished");
    row = 64; std::memcpy(vertices.data() + 44, &row, 4);
    Check(inspect().badPaletteRefs == 1, "six-bit palette source range enforced");
    for (uint32_t word : {0x7fc00001u, 0x7f800000u, 0xff800000u, 0xffc00001u}) {
        std::memcpy(vertices.data() + 44, &word, 4);
        Check(inspect().badPaletteRefs == 1, "nonfinite palette word never reaches integer conversion");
    }
    row = 3; std::memcpy(vertices.data() + 44, &row, 4);
    draw.layoutMask = VL_NRM | VL_TEX0;
    vertices.resize(5 * MakeLayout(draw.layoutMask).stride);
    std::memcpy(vertices.data() + 2 * MakeLayout(draw.layoutMask).stride + 16, &nan, 4);
    Check(inspect().nonfiniteAttributeRefs == 1, "normal/tex float fields checked without treating colors as floats");
    draw.layoutMask |= 1u << 20;
    Check(inspect().badLayout == 1, "unknown layout flags are reported");
    draw.layoutMask &= ~(1u << 20);

    DrawRec relative;
    relative.vtxOffset = 16; relative.vtxCount = 1; relative.idxCount = 1;
    uint16_t index = 0;
    std::vector<uint8_t> relativeVertices(32);
    std::memcpy(relativeVertices.data() + 16, &x, 4);
    Result rel = Inspect(relative, {relativeVertices.data(), relativeVertices.size(), &index, 1}, nullptr, 0, nullptr, 0, false, 61);
    Check(rel.errors == 0 && rel.firstPosition[0] == f2u(2.0f), "relative-index base offset preserved");
    UniformRange post{UB_TEXM, 96, 12};
    relative.ublock[UB_TEXM] = 4;
    std::vector<float> postUniforms(112, 1.0f);
    rel = Inspect(relative, {relativeVertices.data(), relativeVertices.size(), &index, 1}, postUniforms.data(), postUniforms.size(), &post, 1, false, 61);
    Check(rel.errors == 0 && rel.uniformBytes == 48, "post-matrix extra offset and packed active length");
    postUniforms.resize(111);
    Check(Inspect(relative, {relativeVertices.data(), relativeVertices.size(), &index, 1}, postUniforms.data(), postUniforms.size(), &post, 1, false, 61).badUniformRanges == 1, "one-float short post-matrix range rejected");
    relative.layoutMask = VL_COL0; relative.vtxOffset = 0;
    relativeVertices.assign(20, 0);
    std::memset(relativeVertices.data() + 16, 0xff, 4);
    rel = Inspect(relative, {relativeVertices.data(), relativeVertices.size(), &index, 1}, nullptr, 0, nullptr, 0, false, 61);
    Check(rel.errors == 0, "packed RGBA bytes never produce floating-point NaN reports");

    Session off(ParseWindow(nullptr));
    BufUpload upload; upload.bufId = 4; upload.vtx.resize(64); upload.idx = {0, 1, 2};
    off.Upload(upload);
    Check(off.ShadowBytes() == 0, "disabled session retains no CPU geometry");
    Session session(ParseWindow("10"), 80);
    session.Upload(upload);
    Check(session.ShadowBytes() == 70 && session.Find(4).vertices != nullptr, "opt-in static buffer shadow");
    upload.vtx[0] = 7;
    Check(session.Find(4).vertices[0] == 0, "shadow owns uploaded bytes");
    upload.vtx.resize(80);
    session.Upload(upload);
    Check(session.ShadowBytes() == 0 && session.Find(4).vertices == nullptr && session.shadowDrops == 1, "oversized replacement never leaves stale shadow");
    upload.vtx.resize(64); session.Upload(upload); session.Free(4);
    Check(session.ShadowBytes() == 0 && session.Find(4).vertices == nullptr, "free removes reused-id shadow");
    session.Upload(upload); session.Finish(10);
    Check(!session.Enabled() && session.ShadowBytes() == 0, "last selected frame releases diagnostic memory");
    std::puts("PASS: bounded validator selector, real buffer/index/uniform checks, hashes, NaN/Inf, palette and shadow lifecycle");
}
#else
int main() { Check(false, "final-backend validation helper is absent"); }
#endif
