// 1.3 MSC_VGX_FS_HALF / MSC_VGX_TEX2D and the warm-up list, on the real front end (vgx_draw.cpp EnsureShaderList and Lookup): a list made with the switches off, read with them on.
//   shader_forms_list make FILE      writes a list of plain keys
//   shader_forms_list read FILE OUT  reads it, as a run with this environment does: writes the generated texts to OUT (the warm-up's command buffer) and meets one new key (a first use in play)
// Run by run_shader_forms_test.sh.
#include "../src/vgx_draw.cpp"
#include <string>

using namespace vgx;

namespace vgx { bool LogEnabled() { return false; } void SubmitToBackend(CmdBuf&) {} }

static FsKey Fs(int n) {
    FsKey k;
    std::memset(static_cast<void*>(&k), 0, sizeof k);
    k.numStages = 1; k.numTexGens = 1;
    k.stage[0].texEnable = 1; k.stage[0].rasChan = 0; k.stage[0].rasSwap = k.stage[0].texSwap = 0xE4;
    k.stage[0].color = 15u | 10u << 4 | 8u << 8 | 15u << 12 | 1u << 19;
    k.stage[0].alpha = 7u << 4 | 5u << 7 | 4u << 10 | 7u << 13 | 1u << 19;
    k.stage[0].kcsel = static_cast<uint8_t>(n);
    return k;
}
static VsKey Vs(int n) {
    VsKey k;
    std::memset(static_cast<void*>(&k), 0, sizeof k);
    k.numTexGens = 1; k.numChans = 1;
    k.tg[0].src = 5; k.tg[0].proj = static_cast<uint8_t>(n & 1);
    return k;
}
static std::string g_out;
static void Sink(CmdBuf& b) {
    unsigned vs = 0, fs = 0, prog = 0;
    std::string all;
    for (const Cmd& c : b.cmds) {
        if (c.type == CMD_SHADER) {
            const ShaderSource* s = b.shaders[c.arg];
            (s->fragment ? fs : vs)++;
            all += s->text;
        } else if (c.type == CMD_PROGRAM) ++prog;
    }
    if (FILE* f = std::fopen(g_out.c_str(), "wb")) { std::fwrite(all.data(), 1, all.size(), f); std::fclose(f); }
    std::printf("warm-up: %u vertex shaders, %u fragment shaders, %u programs\n", vs, fs, prog);
}
int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "make")) {
        FILE* f = std::fopen(argv[2], "wb");
        ListHeader h{};
        std::memcpy(h.magic, kListMagic, 8);
        h.vsKeySize = sizeof(VsKey); h.fsKeySize = sizeof(FsKey); h.genVersion = kShaderGenVersion;
        std::fwrite(&h, sizeof h, 1, f);
        for (int i = 0; i < 2; ++i) { const uint32_t t = 1; const VsKey k = Vs(i); std::fwrite(&t, 4, 1, f); std::fwrite(&k, sizeof k, 1, f); }
        for (int i = 0; i < 3; ++i) { const uint32_t t = 2; const FsKey k = Fs(i); std::fwrite(&t, 4, 1, f); std::fwrite(&k, sizeof k, 1, f); }
        for (uint32_t i = 0; i < 3; ++i) { const uint32_t r[3] = {3, i & 1, i}; std::fwrite(r, sizeof r, 1, f); }
        std::fclose(f);
        return 0;
    }
    if (argc != 4) return 2;
    g_out = argv[3];
    SetShaderListFile(argv[2]);
    EnsureShaderList();
    Sink(g_buf);
    // A key that is not in the list, met in play: appended in its plain form with its program pair, once.
    const FsKey n = Fs(9);
    const uint16_t id = Lookup(g_fsReg, n, true, GenerateFragmentShader);
    const uint32_t pair[2] = {0u, id};
    if (g_progSeen.insert(id).second) ListAppend(3, pair, sizeof pair);
    std::printf("new key: id %u, generated form %u\n", id, g_fsReg.sources[id]->fkey->prec);
    if (g_shaderList) std::fclose(g_shaderList);
    return 0;
}
