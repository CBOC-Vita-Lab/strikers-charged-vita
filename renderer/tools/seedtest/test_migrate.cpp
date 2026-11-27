// Host test of the cache-layout code of vita/vgx_host_vita.cpp (version helpers, keys, the one-time move of an older cache): run_migrate.sh cuts the blocks out
// of that file and builds this against the POSIX stand-ins of shim.h.   usage: ./tm <root dir>
#include "shim.h"
#include <string>
namespace vgx { constexpr unsigned kShaderGenVersion = 2; }
#define MSC_VGL_ID "vitaGL 9a14d17-glb built with: -DHAVE_SHADER_CACHE -DSTORE_DEPTH_STENCIL -DSKIP_SPLASHSCREEN -DHAVE_SHARK_LOG -DLOG_ERRORS -DHAVE_SCRATCH_MEMORY -DSKIP_ERROR_HANDLING -DDRAW_SPEEDHACK uniform-ring-8MiB"
constexpr const char* kHostShaderAbi = "tex0-7 on TEXCOORD0-7, col0/1 on TEXCOORD8/9, uv on TEXCOORD0; mode GLOBAL; 1";
constexpr const char* kShaderCacheDir = "ux0:data/mscharged/shadercache";
constexpr const char* kSeedDir = "app0:shadercache";
enum { VGXN_BOOT_SEED_COPY = 1 };
static void BootProgress(int, unsigned, unsigned) {}
static unsigned long BootMs() { return 0; }
static unsigned g_version = 3;
extern "C" unsigned vglStrikersShaderCacheVersion(void) { return g_version; }
static std::vector<uint64_t> g_packed;
struct PackEntry { uint64_t hash; uint32_t kind; };
static const PackEntry* PackFind(int kind, uint64_t hash) {
    static PackEntry e; for (uint64_t h : g_packed) if (h == hash && kind == 0) { e = {h, 0}; return &e; } return nullptr;
}
namespace {
#include "part_seedcopy.inc"
#include "part_version.inc"
#include "part_keys.inc"
#include "part_move.inc"
}
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static void Put(const std::string& rel, const char* data = "x") { std::string p = std::string(g_root) + "/" + rel; std::string d = p.substr(0, p.rfind('/')); std::string cmd = "mkdir -p " + d; if (system(cmd.c_str())) {} FILE* f = fopen(p.c_str(), "wb"); fputs(data, f); fclose(f); }
static bool Has(const std::string& rel) { struct stat s; return stat((std::string(g_root) + "/" + rel).c_str(), &s) == 0; }
int main(int argc, char** argv) {
    g_root = argv[1];
    // keys
    CHECK(CacheKeyOf(kLegacyVglId) == 0xbc447993u);               // the key the 1.0/1.1 packages are filed under
    CHECK(LegacyCacheKey() && std::strcmp(LegacyCacheKey(), "bc447993") == 0);
    char nk[16]; std::snprintf(nk, sizeof(nk), "%08x", (unsigned)CacheKeyOf(MSC_VGL_ID));
    std::printf("new key %s, legacy key %s\n", nk, LegacyCacheKey());
    CHECK(std::strcmp(nk, "bc447993") != 0);
    CHECK(VglCacheVersion() == 3 && std::strcmp(VglCacheVersionDir(), "v3") == 0);
    char name[64]; ShaderFileName(name, sizeof(name), "d", 0x00ABCDEF12345678ull, 3); CHECK(std::strcmp(name, "d/00ABCDEF12345678.gxp") == 0);
    ShaderFileName(name, sizeof(name), "d", 0x00ABCDEF12345678ull, 2); CHECK(std::strcmp(name, "d/ABCDEF12345678.gxp") == 0);
    uint64_t h; CHECK(ParseShaderFileName("ABCDEF12345678.gxp", &h) && h == 0x00ABCDEF12345678ull); CHECK(ParseShaderFileName("00ABCDEF12345678.gxp", &h) && h == 0x00ABCDEF12345678ull);
    CHECK(!ParseShaderFileName("00ABCDEF12345678.gxp.tmp", &h)); CHECK(!ParseShaderFileName("seeded", &h)); CHECK(!ParseShaderFileName("XYZ.gxp", &h));
    // a console with a 1.1 cache: old key folder, version 2, short names; one shader is in the pack, one is a leftover temp file
    const std::string old = std::string("ux0/data/mscharged/shadercache/") + LegacyCacheKey() + "/v2";
    Put(old + "/v/00/ABCDEF12345678.gxp"); Put(old + "/v/9A/9A12345678901234.gxp"); Put(old + "/f/1F/1F00000000000001.gxp", "f"); Put(old + "/v/9A/9A12345678901234.gxp.tmp");
    Put(old + "/v/AB/AB00000000000009.gxp");   // in the pack
    g_packed.push_back(0xAB00000000000009ull);
    Put(std::string("ux0/data/mscharged/shadercache/") + nk + "/v3/f/1F/1F00000000000001.gxp", "newer");   // exists already: kept, not overwritten
    const std::string folder = std::string("ux0:data/mscharged/shadercache/") + nk;
    // the same key with an older version folder (a seed copied from an older package)
    Put(std::string("ux0/data/mscharged/shadercache/") + nk + "/v2/f/77/7700000000000abc.gxp");
    MoveOlderCache(folder.c_str());
    const std::string nb = std::string("ux0/data/mscharged/shadercache/") + nk + "/v3";
    CHECK(Has(nb + "/v/00/00ABCDEF12345678.gxp")); CHECK(!Has(old + "/v/00/ABCDEF12345678.gxp"));
    CHECK(Has(nb + "/v/9A/9A12345678901234.gxp"));
    CHECK(Has(old + "/v/AB/AB00000000000009.gxp") && !Has(nb + "/v/AB/AB00000000000009.gxp"));   // pack-covered: left alone
    CHECK(Has(old + "/f/1F/1F00000000000001.gxp"));                                           // the existing new file wins, the old stays
    CHECK(Has(nb + "/f/77/7700000000000ABC.gxp"));                                            // same key, version 2 -> 3, padded
    CHECK(Has(old + "/v/9A/9A12345678901234.gxp.tmp"));                                      // not a cache file: untouched
    CHECK(Has(std::string("ux0/data/mscharged/shadercache/") + nk + "/cache-moved"));
    // a second start does nothing
    Put(old + "/v/55/5500000000000001.gxp");
    MoveOlderCache(folder.c_str());
    CHECK(Has(old + "/v/55/5500000000000001.gxp"));
    // an older library (no function behaviour here: version 2) never moves anything
    std::printf(g_fail ? "FAILED: %d\n" : "migration test passed\n", g_fail);
    return g_fail ? 1 : 0;
}
