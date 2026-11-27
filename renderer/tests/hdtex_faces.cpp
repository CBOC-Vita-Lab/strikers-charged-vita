// 1.3 MSC_HDTEX_FACES (vgx_hdtex.cpp): the blink's eye frames in a pack stay the game's own by default, whatever else the pack replaces.
//   hdtex_faces <pack path> <eyes replaced: 0|1>      (MSC_HDTEX_FACES comes from the environment; one Install per process)
// The pack is written here in the format of tools/hdtex/README.md. The texture cache's side is stood in for: the hook Install registers is called as the cache calls it.
#include "vgx_hdtex.h"
#include "vgx_state.h"
#include "vgx_texdecode.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
static int (*g_hook)(const TexReplaceQuery&, TexUpload&, uint32_t*) = nullptr;
void SetTextureReplaceHook(int (*fn)(const TexReplaceQuery&, TexUpload&, uint32_t*), uint8_t, bool (*)(uint32_t)) { g_hook = fn; }
void SetReplacedTextureBudget(uint64_t) {}
uint32_t TexLevelSize(uint32_t w, uint32_t h, uint32_t) { return w * h / 2; }   // (CMPR)
}  // namespace vgx

using namespace vgx;

static int g_failed = 0;
#define CHECK(c) do { if (!(c)) { ++g_failed; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static constexpr uint32_t kCmpr = 0xE, kSide = 8;

struct Tex { std::string name; std::vector<uint8_t> level0; uint64_t key; };

static Tex Make(const char* name, uint8_t seed) {
    Tex t{name, std::vector<uint8_t>(kSide * kSide / 2), 0};
    for (size_t i = 0; i < t.level0.size(); ++i) t.level0[i] = static_cast<uint8_t>(seed + i * 7);
    t.key = XXH3_64bits_withSeed(t.level0.data(), t.level0.size(), static_cast<uint64_t>(kCmpr) | (static_cast<uint64_t>(kSide) << 8) | (static_cast<uint64_t>(kSide) << 20));
    return t;
}

template <typename T> static void Put(std::vector<uint8_t>& out, T v) { const size_t at = out.size(); out.resize(at + sizeof v); std::memcpy(&out[at], &v, sizeof v); }

// Each entry: 16x16 DXT1, one level (16 * 16 / 2 = 128 bytes).
static void WritePack(const char* path, std::vector<Tex> texs) {
    std::sort(texs.begin(), texs.end(), [](const Tex& a, const Tex& b) { return a.key < b.key; });
    const uint32_t n = static_cast<uint32_t>(texs.size()), indexOff = 32, namesOff = indexOff + 32 * n;
    std::string names;
    std::vector<uint32_t> nameOff;
    for (const Tex& t : texs) { nameOff.push_back(static_cast<uint32_t>(names.size())); names += t.name; names += '\0'; }
    const uint32_t dataOff = (namesOff + static_cast<uint32_t>(names.size()) + 63) / 64 * 64, fileSize = dataOff + n * 128;
    std::vector<uint8_t> f;
    f.insert(f.end(), {'H', 'D', 'T', 'X'});
    for (uint32_t v : {1u, n, indexOff, namesOff, dataOff, fileSize, 0u}) Put(f, v);
    for (uint32_t i = 0; i < n; ++i) {
        Put(f, texs[i].key);
        Put(f, dataOff + i * 128);
        Put<uint32_t>(f, 128);
        for (uint16_t v : {uint16_t(16), uint16_t(16), uint16_t(kSide), uint16_t(kSide)}) Put(f, v);
        for (uint8_t v : {uint8_t(1), uint8_t(1), uint8_t(kCmpr), uint8_t(0)}) Put(f, v);
        Put(f, nameOff[i]);
    }
    f.insert(f.end(), names.begin(), names.end());
    f.resize(fileSize, 0x55);
    FILE* out = std::fopen(path, "wb");
    CHECK(out && std::fwrite(f.data(), 1, f.size(), out) == f.size());
    std::fclose(out);
}

static bool Replaced(const Tex& t) {
    TexReplaceQuery q{};
    q.data = t.level0.data();
    q.fmt = kCmpr;
    q.width = kSide;
    q.height = kSide;
    TexUpload up;
    uint32_t wait = 0;
    return g_hook(q, up, &wait) == 1;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    using hdtex::IsEyeFrameName;
    CHECK(IsEyeFrameName("mario/mario_eye0 mario/mario.rlt:12:abcd"));
    CHECK(IsEyeFrameName("mario/mario_eye2"));
    CHECK(IsEyeFrameName("birdo_bowser/x mario/mario_eye1 mario/extratextures.rlt:1:ab"));   // (a later name of a shared entry)
    CHECK(!IsEyeFrameName("mario/mariohead mario/mario.rlt:1:ab"));
    CHECK(!IsEyeFrameName("mario/mario_eyegray mario/mario.rlt:1:ab"));
    CHECK(!IsEyeFrameName("mario/mario_eye01x y"));
    CHECK(!IsEyeFrameName("mario/mario_eye"));
    CHECK(!IsEyeFrameName(""));

    const Tex head = Make("mario/mariohead mario/mario.rlt:1:ab", 1), eye0 = Make("mario/mario_eye0 mario/extratextures.rlt:2:cd", 2),
              eye1 = Make("mario/mario_eye1 mario/extratextures.rlt:3:ef", 3), eye2 = Make("mario/mario_eye2 mario/extratextures.rlt:4:01", 4);
    WritePack(argv[1], {head, eye0, eye1, eye2});
    setenv("MSC_HDTEX_PATH", argv[1], 1);
    setenv("MSC_HDTEX_PRELOAD", "8", 1);
    setenv("MSC_HDTEX_BUDGET", "8", 1);
    hdtex::Install(nullptr);
    CHECK(hdtex::Mode() == 1 && g_hook);
    if (!g_hook) return 1;
    const bool eyes = std::atoi(argv[2]) != 0;
    CHECK(Replaced(head));
    CHECK(Replaced(eye0) == eyes && Replaced(eye1) == eyes && Replaced(eye2) == eyes);
    hdtex::Counters c;
    hdtex::GetCounters(c);
    CHECK(c.entries == (eyes ? 4u : 1u));
    if (!g_failed) std::puts("PASS");
    return g_failed ? 1 : 0;
}
