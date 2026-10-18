#include "shim.h"
namespace {
#include "part_seed.inc"
}
#include "part_bucket.inc"
static std::string Slurp(const char* p) { FILE* f = fopen(p, "rb"); std::string s; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
// no seed in the package: start-up, then every shader of a session handed to the host's entry points
int main(int argc, char** argv) {
    g_root = argv[1];
    const char* folder = FOLDER;
    sceIoMkdir("ux0:data/mscharged/shadercache", 0777); sceIoMkdir(folder, 0777);
    const char* use = SeedShaderCache(folder);
    std::snprintf(vgl_shader_cache_path, sizeof vgl_shader_cache_path, "%s/v2", use);
    char d[320]; std::snprintf(d, sizeof d, "%s/v/00", vgl_shader_cache_path);
    std::printf("vitaGL start-up without a seed: %s\n", sceIoMkdir(d, 0777) == 0 ? "WOULD CREATE ALL 512 BUCKETS" : "skips bucket creation");
    static const char* vs = "attribute vec4 a_pos;\nvarying vec2 v_uv;\nvoid main() { v_uv = a_pos.xy; gl_Position = a_pos; }\n";
    static const char* fs = "precision mediump float;\nvarying vec2 v_uv;\nuniform vec4 u_add;\nvoid main() { gl_FragColor = vec4(v_uv, 0.0, 1.0) + u_add; }\n";
    GLuint a = vgxvita_glCreateShader(GL_VERTEX_SHADER); vgxvita_glShaderSource(a, 1, &vs, nullptr);
    GLuint b = vgxvita_glCreateShader(GL_FRAGMENT_SHADER); vgxvita_glShaderSource(b, 1, &fs, nullptr);
    for (int i = 2; i < argc; ++i) {
        const std::string text = Slurp(argv[i]);
        const bool frag = std::strstr(argv[i], "/fs_") != nullptr;
        const char* parts[2] = {"#version 100\n", text.c_str()};
        GLuint s = vgxvita_glCreateShader(frag ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        vgxvita_glShaderSource(s, 2, parts, nullptr);
        vgxvita_glShaderSource(s, 2, parts, nullptr);   // (a second time: no second mkdir)
    }
    unsigned made = 0, ms = 0; vgxvita_cacheFolderStats(&made, &ms);
    std::printf("%d shaders handed over, %u bucket folders made\n", argc - 2 + 2, made);
    return 0;
}
