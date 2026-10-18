#include "shim.h"
namespace {
#include "part_seed.inc"
}
#include "part_bucket.inc"
static void Ops(const char* what) { std::printf("%-34s open %u, mkdir ok %u / failed %u, stat %u, read %u, write %u, dopen %u, remove %u\n", what, g_ops[0], g_ops[1], g_ops[2], g_ops[3], g_ops[4], g_ops[5], g_ops[6], g_ops[7]); std::memset(g_ops, 0, sizeof g_ops); }
int main(int argc, char** argv) {
    g_root = argv[1];
    const char* folder = FOLDER;
    sceIoMkdir("ux0:data/mscharged/shadercache", 0777); sceIoMkdir(folder, 0777); std::memset(g_ops, 0, sizeof g_ops);
    for (int pass = 0; pass < argc - 2 + 1; ++pass) {
        if (pass > 0) { if (std::strcmp(argv[pass + 1], "app0") == 0) setenv("MSC_VITA_SEED", "app0", 1); else if (std::strcmp(argv[pass + 1], "touch") == 0) { std::string e = g_root + "/app0/eboot.bin"; FILE* f = fopen(e.c_str(), "ab"); fputc('x', f); fclose(f); } }
        const char* use = SeedShaderCache(folder);
        std::printf("pass %d: vitaGL would use %s\n", pass, use);
        Ops("  seed step:");
        // what vglInit then does
        std::snprintf(vgl_shader_cache_path, sizeof vgl_shader_cache_path, "%s/v2", use);
        sceIoMkdir(use, 0777); sceIoMkdir(vgl_shader_cache_path, 0777); char d[320]; std::snprintf(d, sizeof d, "%s/v", vgl_shader_cache_path); sceIoMkdir(d, 0777);
        std::snprintf(d, sizeof d, "%s/v/00", vgl_shader_cache_path); const bool storm = sceIoMkdir(d, 0777) == 0;
        std::printf("  vitaGL start-up: %s\n", storm ? "WOULD CREATE ALL 512 BUCKETS" : "skips bucket creation");
        std::memset(g_ops, 0, sizeof g_ops);
        std::memset(g_bucketSeen, 0, sizeof g_bucketSeen); g_bucketsMade = 0;
    }
    // the shaders of a session: file names must be the ones vitaGL uses, their folders must exist afterwards
    return 0;
}
