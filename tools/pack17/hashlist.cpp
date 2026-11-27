// hashlist: for every shader key of a vgx shader list (VGXSHL01), the two hashes of its generated text, one line each:
//   <vs|fs> <ordinal among that kind, in file order> <P: 16 hex digits> <T: 16 hex digits>
// P is what the shader pack and the vitaGL cache index a shader by (the "shader pack MISS" lines of the console log): XXH3-64 of "#version 100\n" + the generated GLSL text (the same
// rule as native/tools/seed/seedcheck.cpp, which this is a copy of with the cache lookups left out). T is the renderer's own text hash (HashKey of the text alone: the
// "[shaderplay] ... (text hash ...)" lines). Used by tools/pack17/pack17_request.py.
// Build (32-bit, the lists' key structs are 32-bit; see tools/pack17/build_hashlist.sh):
//   g++ -m32 -std=c++17 -O1 -w -Irenderer/src -Irenderer/third_party -DXXH_INLINE_ALL tools/pack17/hashlist.cpp renderer/src/vgx_shadergen.cpp -o hashlist
#include "vgx_shadergen.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cstdio>
#include <cstring>
#include <string>
using namespace vgx;
struct ListHeader { char magic[8]; uint32_t vsKeySize, fsKeySize, genVersion, reserved; };
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s <list.shl>\n", argv[0]); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 2; }
    ListHeader hd;
    if (std::fread(&hd, sizeof hd, 1, f) != 1 || std::memcmp(hd.magic, "VGXSHL01", 8) != 0) { std::fprintf(stderr, "%s: not a shader list\n", argv[1]); return 2; }
    if (hd.vsKeySize != sizeof(VsKey) || hd.fsKeySize != sizeof(FsKey) || hd.genVersion != kShaderGenVersion) {
        std::fprintf(stderr, "%s: list from another generator (key sizes %u/%u gen %u; this build %zu/%zu gen %u)\n", argv[1], hd.vsKeySize, hd.fsKeySize, hd.genVersion, sizeof(VsKey), sizeof(FsKey), kShaderGenVersion);
        return 3;
    }
    unsigned nVs = 0, nFs = 0;
    uint32_t type;
    while (std::fread(&type, 4, 1, f) == 1) {
        ShaderSource src;
        bool frag = false;
        if (type == 1) { VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; GenerateVertexShader(k, src); }
        else if (type == 2) { FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; src.fragment = true; frag = true; GenerateFragmentShader(k, src); }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, sizeof v, 1, f) != 1) break; continue; }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, sizeof v, 1, f) != 1) break; continue; }
        else if (type == 5) { uint32_t v[2]; if (std::fread(v, sizeof v, 1, f) != 1) break; continue; }
        else { std::fprintf(stderr, "%s: unknown record type %u\n", argv[1], type); return 2; }
        const std::string text = "#version 100\n" + src.text;
        const unsigned long long h = XXH3_64bits(text.data(), text.size()), t = XXH3_64bits(src.text.data(), src.text.size());
        std::printf("%s %u %016llX %016llX\n", frag ? "fs" : "vs", frag ? nFs++ : nVs++, h, t);
    }
    std::fclose(f);
    return 0;
}
