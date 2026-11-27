// The texts of a warm-up list, as the front end generates them with this environment, for shc.c (see the README in this folder's run.sh):
//   listtext <copy of vgx_shaderlist.bin> <out in.txt> [option word for the record header]
// Each record's name is the 16-digit XXH3 of the text, the hash vitaGL files the compiled shader under (pack, cache folder). The list file may be appended to: pass a copy.
#include "../../../renderer/src/vgx_draw.cpp"
#include <string>
using namespace vgx;
namespace vgx { bool LogEnabled() { return false; } void SubmitToBackend(CmdBuf&) {} }
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    SetShaderListFile(argv[1]);
    EnsureShaderList();
    FILE* f = std::fopen(argv[2], "wb");
    unsigned nf = 0, nv = 0;
    for (const Cmd& c : g_buf.cmds) {
        if (c.type != CMD_SHADER) continue;
        const ShaderSource* s = g_buf.shaders[c.arg];
        if (!s->fragment && !std::getenv("LISTTEXT_VS")) continue;
        (s->fragment ? nf : nv)++;
        const std::string t = "#version 100\n" + s->text;   // (what the back end hands to vitaGL: kShaderVersion, then the text)
        std::fprintf(f, "#### %016llX %s %s\n%s\n", static_cast<unsigned long long>(XXH3_64bits(t.data(), t.size())), s->fragment ? "f" : "v", argc > 3 ? argv[3] : "", t.c_str());
    }
    // LISTTEXT_VSFACTS=1: every listed vertex key also with the light facts of MSC_VGX_VS_TRIM, as records <hash>.A (all lights directional: both attenuations 1), .C (spot term only: point lights),
    // .U (colour 1 unread, added to .A) and .R (mode 2: far lights, on .U). Names carry the hash of the plain text.
    if (std::getenv("LISTTEXT_VSFACTS"))
        for (const Cmd& c : g_buf.cmds) {
            if (c.type != CMD_SHADER || g_buf.shaders[c.arg]->fragment) continue;
            const ShaderSource* s = g_buf.shaders[c.arg];
            const std::string t = "#version 100\n" + s->text;
            const unsigned long long h = XXH3_64bits(t.data(), t.size());
            uint8_t mask = 0;
            for (const auto& ch : s->vkey->chan) mask |= ch.lightMask;
            for (const char* v : {"A", "C", "U", "R"}) {
                VsKey k = *s->vkey;
                k.chan[1].pad = mask;
                k.chan[2].pad = *v == 'C' ? 0 : mask;
                if (*v == 'U' || *v == 'R') k.tg[7].pad[0] = 1;
                if (*v == 'R') k.chan[3].pad = mask;
                ShaderSource o;
                GenerateVertexShader(k, o);
                std::fprintf(f, "#### %016llX.%s v \n#version 100\n%s\n", h, v, o.text.c_str());
            }
        }
    std::fclose(f);
    std::printf("%u fragment, %u vertex\n", nf, nv);
    return 0;
}
