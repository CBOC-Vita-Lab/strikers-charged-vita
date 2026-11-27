// shaderkey.cpp (tools15): vgx::ShaderKeys gives T = XXH3 of the text and P = XXH3 of "#version 100\n" + text, the two hashes one compile log line names.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "vgx_shadertext.h"
int main() {
    int bad = 0;
    const char* texts[] = {"void main(){}", "precision mediump float;\nvoid main(){ gl_FragColor = vec4(1.0); }", ""};
    for (const char* t : texts) {
        uint64_t p = 0, tt = 0;
        vgx::ShaderKeys(t, &p, &tt);
        const std::string full = std::string("#version 100\n") + t;
        const uint64_t wantT = XXH3_64bits(t, std::strlen(t)), wantP = XXH3_64bits(full.data(), full.size());
        if (tt != wantT || p != wantP || p == tt) { std::printf("FAIL: '%.20s' P %016llX T %016llX\n", t, (unsigned long long)p, (unsigned long long)tt); ++bad; }
    }
    uint64_t p = 1;
    vgx::ShaderKeys("x", &p, nullptr);   // a null T is allowed
    vgx::ShaderKeys("x", nullptr, nullptr);
    if (bad == 0) std::printf("shaderkey: all passed\n");
    return bad != 0;
}
