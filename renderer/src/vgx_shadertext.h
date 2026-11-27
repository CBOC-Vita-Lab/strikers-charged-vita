// vgx_shadertext.h: MSC_VGX_SHADERTEXT_LOG=<file> (diagnostic, off unless set; wave 11, pack17). Appends every distinct shader text the renderer gives the compiler
// (generated shaders and the back end's own programs) to <file> as
//     @@ <vs|fs> <P> <T> <length>\n<text>\n
// P = XXH3-64 of "#version 100\n" + text: what the shader pack and vitaGL's cache index a shader by (the "shader pack MISS" lines). T = XXH3-64 of the text alone
// (HashKey: the "[shaderplay] ... (text hash ...)" lines). tools/pack17/pack17_request.py reads these files to turn a console miss of a back end program, which no shader
// list holds, into its text and the route that makes it. Nothing is read or changed by the game; a run without the variable pays one cached getenv.
#ifndef VGX_SHADERTEXT_H
#define VGX_SHADERTEXT_H
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include "vgx_shadergen.h"

namespace vgx {
// tools15: both keys of one shader text, for the log lines that name a compile ("[shaderkey]" / "[shaderplay]"): P pairs with the "shader pack MISS" lines, T with the older
// "[shaderplay] ... (text hash T)" lines, so a log can be resolved either way (before, the two hashes could not be paired from a log). Log paths only (allocates).
inline void ShaderKeys(const char* text, uint64_t* p, uint64_t* t) {
    const size_t n = std::strlen(text);
    if (t) *t = HashKey(text, n);
    if (p) { const std::string full = std::string("#version 100\n") + text; *p = HashKey(full.data(), full.size()); }
}
inline void ShaderTextNote(bool fragment, const char* text) {
    static const char* const path = std::getenv("MSC_VGX_SHADERTEXT_LOG");
    if (!path || !*path || !text) return;
    static std::mutex mtx;
    static std::set<uint64_t> seen;
    const size_t n = std::strlen(text);
    const uint64_t t = HashKey(text, n);
    const std::string full = std::string("#version 100\n") + text;
    const uint64_t p = HashKey(full.data(), full.size());
    std::lock_guard<std::mutex> lk(mtx);
    if (!seen.insert(p ^ (fragment ? 0x9E3779B97F4A7C15ull : 0)).second) return;
    if (FILE* f = std::fopen(path, "ab")) {
        std::fprintf(f, "@@ %s %016llX %016llX %u\n", fragment ? "fs" : "vs", static_cast<unsigned long long>(p), static_cast<unsigned long long>(t), static_cast<unsigned>(n));
        std::fwrite(text, 1, n, f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}
}  // namespace vgx
#endif
