// W5-SEED: which compiled-shader files does a vgx shader list need, and which does a seed folder have?
//   seedcheck <list.shl> [<cache dir: .../<key>/v2>] [--names]
// For every vertex / fragment key of the list the renderer's own generator (vgx_shadergen.cpp) makes the GLSL text; vitaGL names its cache file by
// XXH3-64 of "#version 100\n" + text (v/ or f/, then the top byte as a folder, then the hash in hex, ".gxp"; checked against a seed that vitaGL itself
// wrote: tools/seedtest/xh.c). Prints one line per shader with "have"/"MISSING" when a cache dir is given; exit status 1 if any is missing.
// Build: g++ -std=c++17 -O1 -w -I<vgx>/src -I<xxhash> -DXXH_INLINE_ALL seedcheck.cpp <vgx>/src/vgx_shadergen.cpp -o seedcheck
#include "vgx_shadergen.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
using namespace vgx;
struct ListHeader { char magic[8]; uint32_t vsKeySize, fsKeySize, genVersion, reserved; };
static bool Exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <list> [<cache dir>] [--names]\n", argv[0]); return 2; }
    std::string dir; bool names = false;
    for (int i = 2; i < argc; ++i) { if (!std::strcmp(argv[i], "--names")) names = true; else dir = argv[i]; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 2; }
    ListHeader hd;
    if (std::fread(&hd, sizeof hd, 1, f) != 1 || std::memcmp(hd.magic, "VGXSHL01", 8) != 0) { std::fprintf(stderr, "not a shader list\n"); return 2; }
    if (hd.vsKeySize != sizeof(VsKey) || hd.fsKeySize != sizeof(FsKey) || hd.genVersion != kShaderGenVersion) {
        std::fprintf(stderr, "list from another generator (key sizes %u/%u gen %u; this build %zu/%zu gen %u)\n", hd.vsKeySize, hd.fsKeySize, hd.genVersion, sizeof(VsKey), sizeof(FsKey), kShaderGenVersion);
        return 2;
    }
    unsigned nVs = 0, nFs = 0, missing = 0, have = 0;
    uint32_t type;
    while (std::fread(&type, 4, 1, f) == 1) {
        ShaderSource src;
        bool frag = false;
        if (type == 1) { VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; GenerateVertexShader(k, src); ++nVs; }
        else if (type == 2) { FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; src.fragment = true; frag = true; GenerateFragmentShader(k, src); ++nFs; }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, sizeof v, 1, f) != 1) break; continue; }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, sizeof v, 1, f) != 1) break; continue; }
        else { std::fprintf(stderr, "unknown record type %u\n", type); return 2; }
        std::string text = "#version 100\n" + src.text;
        const unsigned long long h = XXH3_64bits(text.data(), text.size());
        char rel[64];
        std::snprintf(rel, sizeof rel, "%c/%02X/%llX.gxp", frag ? 'f' : 'v', (unsigned)(h >> 56), h);
        if (!dir.empty()) {
            const bool ok = Exists(dir + "/" + rel);
            ok ? ++have : ++missing;
            if (names || !ok) std::printf("%s %s %s\n", ok ? "have   " : "MISSING", frag ? "fs" : "vs", rel);
        } else if (names) std::printf("%s\n", rel);
    }
    std::fclose(f);
    std::printf("%u vertex shaders, %u fragment shaders; %s%u have, %u missing\n", nVs, nFs, dir.empty() ? "(no cache dir) " : "", have, missing);
    return missing ? 1 : 0;
}
