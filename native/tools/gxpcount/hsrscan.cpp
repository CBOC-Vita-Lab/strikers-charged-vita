// What the hidden-surface proofs (MSC_VGX_HSR_PROVEN, vgx_shadergen.h) can say about the programs of a warm-up list, by (fragment key, blend function):
//   hsrscan <copy of a vgx_shaderlist.bin / .shl>
// One line per fragment key that has an alpha test or a blend in some variant, then the totals: variants (program x blend function x layout), counted once per (fragment key, blend key).
#include "../../../renderer/src/vgx_draw.cpp"
#include <map>
#include <set>
using namespace vgx;
namespace vgx { bool LogEnabled() { return false; } void SubmitToBackend(CmdBuf&) {} }
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    SetShaderListFile(argv[1]);
    EnsureShaderList();
    std::set<std::pair<uint32_t, uint32_t>> seen;
    unsigned nv = 0, blended = 0, tested = 0, bt = 0, alpha1 = 0, rangeTest = 0, constTest = 0, srcAlphaBlend = 0, opaqueNow = 0;
    std::map<uint32_t, int> byKey;
    for (const VariantCmd& v : g_buf.variants) {
        if (!seen.insert({v.fsId, v.blendKey}).second) continue;
        const FsKey& k = g_fsReg.keys[v.fsId];
        ++nv;
        const bool blend = v.blendKey & 1u;
        const uint32_t sf = (v.blendKey >> 2) & 15u, df = (v.blendKey >> 6) & 15u;
        float lo, hi;
        FsFinalAlphaRange(k, lo, hi);
        const bool one = AlphaProvenOne(k);
        blended += blend; tested += k.alphaTestOn; bt += blend && k.alphaTestOn;
        opaqueNow += !blend && !k.alphaTestOn;
        const bool srcA = blend && !(v.blendKey & 2u) && (sf == BF_SRC_ALPHA || sf == BF_ONE) && (df == BF_INV_SRC_ALPHA || df == BF_ZERO) && !(sf == BF_ONE && df == BF_ZERO);
        srcAlphaBlend += srcA;
        alpha1 += srcA && one;
        // an alpha test that no reference can fail on: look at every pair of references? Only the ones a draw has are known; report the range for the tests with a narrower range than 0..1
        if (k.alphaTestOn) { if (lo > 0.0f || hi < 1.0f) ++rangeTest; if (AlphaTestAlwaysPasses(k, 0, 0) && false) ++constTest; }
        if ((blend || k.alphaTestOn) && (lo > 0.0f || hi < 1.0f || one))
            std::printf("fs %3u blend %03x (src %u dst %u) test %u (0x%02x) stages %u dstA %u alpha range [%.4f, %.4f]%s%s\n", v.fsId, v.blendKey, sf, df, k.alphaTestOn, k.alphaTest, k.numStages, k.dstAlpha, lo, hi, one ? " PROVEN 1" : "", srcA && one ? " -> opaque" : "");
    }
    std::printf("variants %u: blended %u, alpha-tested %u (both %u), neither %u; blended by source alpha (SRC_ALPHA|ONE x INV_SRC_ALPHA|ZERO) %u of which alpha proven 1: %u; alpha-tested with a range narrower than 0..1: %u\n",
                nv, blended, tested, bt, opaqueNow, srcAlphaBlend, alpha1, rangeTest);
    return 0;
}
