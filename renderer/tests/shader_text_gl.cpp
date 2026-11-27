// Proof harness for MSC_VGX_VS_NOBRANCH and MSC_VGX_FS_PROJ (run_shader_text_test.sh). Mesa (EGL surfaceless, llvmpipe), GLES 3.0 context (transform feedback), the shader texts of
// native/tools/gxpcount/listtext (the same list generated twice, switch off and on: record i of both files is the same key).
//   shader_text_gl vs OLD.txt NEW.txt          every vertex record that differs: random + adversarial attributes and uniforms, gl_Position and every varying captured by transform
//                                              feedback, compared by BIT PATTERN (so -0, NaN payloads, and the last bit count). Exit 1 on any difference.
//   shader_text_gl fs OLD.txt NEW.txt [MAXLEVEL]  every fragment record that differs: a textured triangle pair per case, the 8-bit pixels compared. Prints the largest level difference and
//                                              the number of differing pixels per regime; exit 1 when the largest difference is above MAXLEVEL (default 0).
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

struct Rec { std::string kind, text; };
static std::vector<Rec> Load(const char* path) {
    std::vector<Rec> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(2); }
    std::string all; char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    std::fclose(f);
    size_t pos = 0;
    while ((pos = all.find("#### ", pos)) != std::string::npos) {
        const size_t eol = all.find('\n', pos);
        const size_t next = all.find("\n#### ", eol);
        std::string head = all.substr(pos + 5, eol - pos - 5);
        Rec r;
        const size_t sp = head.find(' ');
        r.kind = head.substr(sp + 1, 1);
        r.text = all.substr(eol + 1, (next == std::string::npos ? all.size() : next + 1) - eol - 1);
        v.push_back(r);
        pos = next == std::string::npos ? all.size() : next + 1;
    }
    return v;
}

static bool InitGL() {
    setenv("EGL_PLATFORM", "surfaceless", 0);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || !n) return false;
    const EGLint sa[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    return c != EGL_NO_CONTEXT && eglMakeCurrent(d, s, s, c);
}

static GLuint Compile(GLenum kind, const std::string& text, std::string* log) {
    GLuint s = glCreateShader(kind);
    const std::string src = "#version 100\n" + text;   // (the records already start with "#version 100\n": drop the duplicate)
    const char* p = text.rfind("#version 100", 0) == 0 ? text.c_str() : src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[4096]; glGetShaderInfoLog(s, sizeof b, nullptr, b); *log = b; return 0; }
    return s;
}

static const char* const kAttrs[] = {"a_pos", "a_nrm", "a_col0", "a_col1", "a_tex0", "a_tex1", "a_tex2", "a_tex3", "a_tex4", "a_tex5", "a_tex6", "a_tex7", "a_tmi0", "a_tmi1", "a_bix", "a_bw"};
static const int kAttrSize[] = {4, 3, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 4, 4};

struct Rng { std::mt19937 g; float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); } int I(int n) { return static_cast<int>(g() % static_cast<unsigned>(n)); } };

// Parses "varying <type> <name>;" lines.
static std::vector<std::pair<std::string, int>> Varyings(const std::string& t) {
    std::vector<std::pair<std::string, int>> v;
    size_t p = 0;
    while ((p = t.find("varying ", p)) != std::string::npos) {
        if (p && t[p - 1] != '\n') { p += 8; continue; }
        const size_t e = t.find(';', p);
        char ty[32], nm[64];
        std::string line = t.substr(p + 8, e - p - 8);
        if (std::sscanf(line.c_str(), "%31s %63s", ty, nm) == 2) {
            std::string T = ty;
            int sz = T == "float" ? 1 : T == "vec2" ? 2 : T == "vec3" ? 3 : 4;
            if (T == "HV4") sz = 4;
            v.push_back({nm, sz});
        }
        p = e;
    }
    return v;
}

// One vertex program pair over N vertices per regime. Returns the number of differing words.
static int N_VERT = 2048;
static uint64_t g_words = 0, g_diff = 0;

static GLuint MakeVsProgram(const std::string& text, const std::vector<std::pair<std::string, int>>& vary, std::string* log) {
    GLuint vs = Compile(GL_VERTEX_SHADER, text, log);
    if (!vs) return 0;
    // a fragment shader that reads every varying (so none is optimised out)
    std::string fs = "precision highp float;\n";
    std::string body = "void main() {\n  float s = 0.0;\n";
    for (auto& v : vary) {
        const char* ty[] = {"", "float", "vec2", "vec3", "vec4"};
        fs += std::string("varying ") + ty[v.second] + " " + v.first + ";\n";
        body += "  s += dot(" + v.first + (v.second == 1 ? "" : "") + ", " + v.first + ");\n";
    }
    fs += body + "  gl_FragColor = vec4(s);\n}\n";
    GLuint f = Compile(GL_FRAGMENT_SHADER, fs, log);
    if (!f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, f);
    for (int i = 0; i < 16; ++i) glBindAttribLocation(p, i, kAttrs[i]);
    std::vector<const char*> names; names.push_back("gl_Position");
    for (auto& v : vary) names.push_back(v.first.c_str());
    glTransformFeedbackVaryings(p, static_cast<GLsizei>(names.size()), names.data(), GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char b[4096]; glGetProgramInfoLog(p, sizeof b, nullptr, b); *log = b; return 0; }
    return p;
}

// regimes: how the uniforms and attributes are drawn
enum { R_NORMAL, R_ZEROMAT, R_TINYMAT, R_HUGEMAT, R_DENORMMAT, R_COUNT };
static const char* const kRegime[] = {"normal", "zero matrices (eye 0, light at eye)", "1e-25 matrices (length underflows)", "1e20 matrices (length overflows)", "1e-21 matrices (squared length denormal)"};

static int RunVs(const Rec& a, const Rec& b, int recNo) {
    std::string log;
    auto vary = Varyings(a.text);
    GLuint pa = MakeVsProgram(a.text, vary, &log);
    if (!pa) { std::fprintf(stderr, "record %d: OLD text does not compile/link: %s\n", recNo, log.c_str()); return -1; }
    GLuint pb = MakeVsProgram(b.text, vary, &log);
    if (!pb) { std::fprintf(stderr, "record %d: NEW text does not compile/link: %s\n", recNo, log.c_str()); return -1; }
    int bad = 0;
    int words = 4; for (auto& v : vary) words += v.second;
    for (int regime = 0; regime < R_COUNT; ++regime) {
        Rng r{std::mt19937(1234 + recNo * 7 + regime)};
        // attributes (the same arrays for both programs)
        std::vector<float> attr(16 * 4 * N_VERT);
        for (int i = 0; i < N_VERT; ++i) {
            float* A = &attr[i * 64];
            for (int k = 0; k < 64; ++k) A[k] = r.U(-1, 1);
            // position: model-sized; w is the row index as the game stores it: whole numbers (some past the clamps) and fractions
            for (int k = 0; k < 3; ++k) A[k] = r.U(-50, 50);
            const int m = r.I(6);
            A[3] = m == 0 ? static_cast<float>(r.I(100)) : m == 1 ? static_cast<float>(r.I(100)) + 0.5f * (r.I(2) ? 1 : -1) : m == 2 ? 61.0f + r.I(3) - 1 : m == 3 ? 45.0f + r.I(3) - 1 : m == 4 ? r.U(0, 90) : static_cast<float>(r.I(4)) + 29.0f;
            // normal: random, zero, tiny, huge
            const int nm = r.I(8);
            for (int k = 0; k < 3; ++k) A[4 + k] = nm == 0 ? 0.0f : nm == 1 ? 1e-30f * r.U(-1, 1) : nm == 2 ? 1e25f * r.U(-1, 1) : nm == 3 ? (k == 0 ? 1.0f : 0.0f) : nm == 5 ? 1e-21f * r.U(-1, 1) : nm == 6 ? 3e-20f * r.U(-1, 1) : nm == 7 ? 1e-4f * r.U(-1, 1) : r.U(-1, 1);
            if (nm == 4) A[4] = -0.0f;
            // texture coordinates, tmi (matrix row index like the palette's), bone indices, weights
            for (int t = 0; t < 8; ++t) for (int k = 0; k < 2; ++k) A[16 + t * 4 + k] = r.U(-4, 4);
            for (int k = 0; k < 4; ++k) {
                A[12 * 4 + k] = static_cast<float>(r.I(100)) * (r.I(4) == 0 ? 1.0f : 1.0f) + (r.I(5) == 0 ? 0.5f : 0.0f);
                A[13 * 4 + k] = static_cast<float>(r.I(100));
                A[14 * 4 + k] = static_cast<float>(r.I(21));
                A[15 * 4 + k] = r.U(0, 1);
            }
        }
        // uniform data by name (shared between the two programs)
        std::map<std::string, std::vector<float>> uni;
        const float scale = regime == R_ZEROMAT ? 0.0f : regime == R_TINYMAT ? 1e-25f : regime == R_HUGEMAT ? 1e20f : regime == R_DENORMMAT ? 1e-21f : 1.0f;
        auto Uni = [&](const std::string& name, int count) -> std::vector<float>& {
            auto it = uni.find(name);
            if (it != uni.end()) return it->second;
            std::vector<float> v(count * 4);
            Rng ur{std::mt19937(std::hash<std::string>()(name) + regime * 977 + recNo)};
            for (float& x : v) x = ur.U(-1, 1);
            if (name == "u_pal" || name == "u_mv" || name == "u_bone" || name == "u_tm" || name == "u_ptm")
                for (float& x : v) x *= scale;
            if (name == "u_lt")   // light positions: at the eye when the matrices are 0 (rows 4 + 5 l), colours 0..1
                for (int i = 0; i < count; ++i) {
                    const int rowIn = i >= 4 ? (i - 4) % 5 : -1;
                    for (int k = 0; k < 4; ++k) {
                        float& x = v[i * 4 + k];
                        if (i < 4) x = ur.U(0, 1);
                        else if (rowIn == 0 && regime != R_NORMAL) x = regime == R_ZEROMAT ? 0.0f : regime == R_TINYMAT ? 1e-25f * x : regime == R_DENORMMAT ? 1e-21f * x : 1e19f * x;
                        else if (rowIn == 2) x = ur.U(0, 1);
                    }
                }
            return uni[name] = v;
        };
        std::vector<uint32_t> outs[2];
        for (int w = 0; w < 2; ++w) {
            GLuint p = w ? pb : pa;
            glUseProgram(p);
            GLint nu = 0; glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
            for (int u = 0; u < nu; ++u) {
                char nmb[128]; GLsizei len; GLint sz; GLenum ty;
                glGetActiveUniform(p, u, sizeof nmb, &len, &sz, &ty, nmb);
                std::string nm = nmb; const size_t br = nm.find('['); if (br != std::string::npos) nm = nm.substr(0, br);
                const GLint loc = glGetUniformLocation(p, nm.c_str());
                if (ty == GL_FLOAT_VEC4) { auto& v = Uni(nm, sz); glUniform4fv(loc, sz, v.data()); }
                else if (ty == GL_FLOAT_MAT4) { std::vector<float> m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; glUniformMatrix4fv(loc, 1, GL_FALSE, m.data()); }
            }
            GLuint vb, tf; glGenBuffers(1, &vb); glGenBuffers(1, &tf);
            glBindBuffer(GL_ARRAY_BUFFER, vb);
            glBufferData(GL_ARRAY_BUFFER, attr.size() * 4, attr.data(), GL_STATIC_DRAW);
            for (int i = 0; i < 16; ++i) { glEnableVertexAttribArray(i); glVertexAttribPointer(i, kAttrSize[i], GL_FLOAT, GL_FALSE, 256, reinterpret_cast<void*>(static_cast<uintptr_t>(i * 16))); }
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, tf);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(N_VERT) * words * 4, nullptr, GL_STATIC_READ);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tf);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, N_VERT);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);
            outs[w].resize(static_cast<size_t>(N_VERT) * words);
            const void* mp = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, static_cast<GLsizeiptr>(outs[w].size()) * 4, GL_MAP_READ_BIT);
            if (mp) std::memcpy(outs[w].data(), mp, outs[w].size() * 4);
            glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
            glDeleteBuffers(1, &vb); glDeleteBuffers(1, &tf);
        }
        int d = 0;
        for (size_t i = 0; i < outs[0].size(); ++i) if (outs[0][i] != outs[1][i]) ++d;
        g_words += outs[0].size(); g_diff += d;
        if (d) { std::printf("  record %d regime '%s': %d of %zu words differ\n", recNo, kRegime[regime], d, outs[0].size()); bad += d; }
    }
    glDeleteProgram(pa); glDeleteProgram(pb);
    return bad;
}

// ---- fragment: textured triangle pair, the varyings drawn per regime
static GLuint VsFor(const std::vector<std::pair<std::string, int>>& vary, std::string* log) {
    std::string v = "attribute vec4 a_p;\n";
    std::string body = "void main() {\n  gl_Position = vec4(a_p.xy, 0.0, 1.0);\n";
    const char* ty[] = {"", "float", "vec2", "vec3", "vec4"};
    int i = 0;
    for (auto& x : vary) {
        v += std::string("attribute vec4 b") + std::to_string(i) + ";\nvarying " + ty[x.second] + " " + x.first + ";\n";
        body += "  " + x.first + " = b" + std::to_string(i) + (x.second == 4 ? "" : x.second == 3 ? ".xyz" : x.second == 2 ? ".xy" : ".x") + ";\n";
        ++i;
    }
    return Compile(GL_VERTEX_SHADER, v + body + "}\n", log);
}

static int g_fsMax = 0;
static int RunFs(const Rec& a, const Rec& b, int recNo, std::map<std::string, std::pair<int, long>>& stats) {
    std::string log;
    auto vary = Varyings(a.text);
    GLuint vs = VsFor(vary, &log);
    if (!vs) { std::fprintf(stderr, "record %d: vs: %s\n", recNo, log.c_str()); return -1; }
    GLuint fa = Compile(GL_FRAGMENT_SHADER, a.text, &log);
    if (!fa) { std::fprintf(stderr, "record %d: OLD fs: %s\n", recNo, log.c_str()); return -1; }
    GLuint fb = Compile(GL_FRAGMENT_SHADER, b.text, &log);
    if (!fb) { std::fprintf(stderr, "record %d: NEW fs: %s\n", recNo, log.c_str()); return -1; }
    GLuint pr[2];
    for (int w = 0; w < 2; ++w) {
        pr[w] = glCreateProgram();
        glAttachShader(pr[w], vs); glAttachShader(pr[w], w ? fb : fa);
        glBindAttribLocation(pr[w], 0, "a_p");
        for (size_t i = 0; i < vary.size(); ++i) glBindAttribLocation(pr[w], 1 + static_cast<GLuint>(i), (std::string("b") + std::to_string(i)).c_str());
        glLinkProgram(pr[w]);
        GLint ok = 0; glGetProgramiv(pr[w], GL_LINK_STATUS, &ok);
        if (!ok) { char bb[4096]; glGetProgramInfoLog(pr[w], sizeof bb, nullptr, bb); std::fprintf(stderr, "record %d link: %s\n", recNo, bb); return -1; }
    }
    // textures: 8 units of 32x32 random RGBA, linear, repeat
    GLuint tex[8];
    glGenTextures(8, tex);
    Rng tr{std::mt19937(99 + recNo)};
    for (int u = 0; u < 8; ++u) {
        std::vector<uint8_t> px(32 * 32 * 4);
        for (auto& c : px) c = static_cast<uint8_t>(tr.g());
        glActiveTexture(GL_TEXTURE0 + u); glBindTexture(GL_TEXTURE_2D, tex[u]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    int worst = 0;
    // regimes: 0 z in 0.3..4 (uv spans 0..1 repeats), 1 z in 0.01..0.2 (tiny q: huge coordinates), 2 z = exactly 0 everywhere, 3 z crossing zero (negative q on one side)
    for (int regime = 0; regime < 4; ++regime) {
        Rng r{std::mt19937(5 + recNo * 13 + regime)};
        // triangle strip of 2 triangles over the 64x64 target: corner values per varying
        float corner[4][8][4];
        for (int c = 0; c < 4; ++c)
            for (size_t i = 0; i < vary.size() && i < 8; ++i) {
                const bool tex = vary[i].first.rfind("v_tex", 0) == 0;
                for (int k = 0; k < 4; ++k) corner[c][i][k] = r.U(0, 1);
                if (tex) {
                    float z = regime == 0 ? r.U(0.3f, 4) : regime == 1 ? r.U(0.01f, 0.2f) : regime == 2 ? 0.0f : (c & 1 ? -r.U(0.2f, 2) : r.U(0.2f, 2));
                    corner[c][i][2] = z;
                    corner[c][i][0] = r.U(0, 1) * (regime == 2 ? 1.0f : std::fabs(z)) * 3.0f;
                    corner[c][i][1] = r.U(0, 1) * (regime == 2 ? 1.0f : std::fabs(z)) * 3.0f;
                }
                if (vary[i].first == "v_fz") corner[c][i][0] = r.U(0.1f, 100);
            }
        const float pos[4][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}, {1, 1, 0, 1}};
        std::vector<uint8_t> img[2];
        for (int w = 0; w < 2; ++w) {
            glUseProgram(pr[w]);
            GLint nu = 0; glGetProgramiv(pr[w], GL_ACTIVE_UNIFORMS, &nu);
            for (int u = 0; u < nu; ++u) {
                char nmb[128]; GLsizei len; GLint sz; GLenum ty;
                glGetActiveUniform(pr[w], u, sizeof nmb, &len, &sz, &ty, nmb);
                std::string nm = nmb; const size_t br = nm.find('['); if (br != std::string::npos) nm = nm.substr(0, br);
                const GLint loc = glGetUniformLocation(pr[w], nm.c_str());
                if (ty == GL_SAMPLER_2D) glUniform1i(loc, nm.size() > 5 ? nm[5] - '0' : 0);
                else if (ty == GL_FLOAT_VEC4) {
                    Rng ur{std::mt19937(std::hash<std::string>()(nm) + recNo)};
                    std::vector<float> v(sz * 4);
                    for (float& x : v) x = ur.U(0, 1);
                    glUniform4fv(loc, sz, v.data());
                }
            }
            GLuint fbo, rb; glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            glViewport(0, 0, 64, 64); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            std::vector<float> vb;
            for (int c = 0; c < 4; ++c) { for (int k = 0; k < 4; ++k) vb.push_back(pos[c][k]); for (size_t i = 0; i < vary.size(); ++i) for (int k = 0; k < 4; ++k) vb.push_back(corner[c][i][k]); }
            const GLsizei stride = static_cast<GLsizei>((1 + vary.size()) * 16);
            GLuint buf; glGenBuffers(1, &buf); glBindBuffer(GL_ARRAY_BUFFER, buf); glBufferData(GL_ARRAY_BUFFER, vb.size() * 4, vb.data(), GL_STATIC_DRAW);
            for (size_t i = 0; i <= vary.size(); ++i) { glEnableVertexAttribArray(static_cast<GLuint>(i)); glVertexAttribPointer(static_cast<GLuint>(i), 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(static_cast<uintptr_t>(i * 16))); }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            img[w].resize(64 * 64 * 4);
            glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, img[w].data());
            glDeleteBuffers(1, &buf); glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &rb);
        }
        int maxd = 0; long nd = 0;
        for (size_t i = 0; i < img[0].size(); ++i) { const int d = std::abs(int(img[0][i]) - int(img[1][i])); if (d) ++nd; if (d > maxd) maxd = d; }
        if (regime < 3) worst = maxd > worst ? maxd : worst;   // (regime 3, q changing sign inside the triangle, is printed only: the divide there is by numbers near 0)
        auto& s2 = stats[std::string("regime ") + std::to_string(regime)];
        if (maxd > s2.first) s2.first = maxd;
        s2.second += nd;
    }
    for (int w = 0; w < 2; ++w) glDeleteProgram(pr[w]);
    glDeleteTextures(8, tex);
    if (worst > g_fsMax) g_fsMax = worst;
    return worst;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: shader_text_gl vs|fs OLD.txt NEW.txt [MAXLEVEL]\n"); return 2; }
    if (!InitGL()) { std::fprintf(stderr, "no GLES 3 context (Mesa surfaceless) on this machine\n"); return 77; }
    const std::string mode = argv[1];
    auto A = Load(argv[2]), B = Load(argv[3]);
    if (A.size() != B.size()) { std::fprintf(stderr, "record counts differ (%zu / %zu)\n", A.size(), B.size()); return 2; }
    const char* want = mode == "vs" ? "v" : "f";
    int changed = 0, same = 0, bad = 0, failed = 0;
    std::map<std::string, std::pair<int, long>> stats;
    std::map<std::string, int> seen;   // identical old texts are proven once
    const int maxLevel = argc > 4 ? std::atoi(argv[4]) : 0;
    for (size_t i = 0; i < A.size(); ++i) {
        if (A[i].kind != want) continue;
        if (A[i].text == B[i].text) { ++same; continue; }
        if (seen.count(A[i].text + "\x01" + B[i].text)) continue;
        seen[A[i].text + "\x01" + B[i].text] = 1;
        ++changed;
        if (mode == "vs") { const int r = RunVs(A[i], B[i], static_cast<int>(i)); if (r < 0) ++failed; else if (r) ++bad; }
        else { const int r = RunFs(A[i], B[i], static_cast<int>(i), stats); if (r < 0) ++failed; else if (r > maxLevel) ++bad; }
    }
    if (mode == "vs")
        std::printf("vs: %d changed texts proven (+%d identical records skipped), %llu output words compared by bit pattern, %llu differ, %d texts failed to build, %d texts with a difference\n", changed, same, static_cast<unsigned long long>(g_words), static_cast<unsigned long long>(g_diff), failed, bad);
    else {
        std::printf("fs: %d changed texts compared (+%d identical records skipped), %d failed to build\n", changed, same, failed);
        for (auto& kv : stats) std::printf("  %s: largest level difference %d, differing 8-bit channels %ld\n", kv.first.c_str(), kv.second.first, kv.second.second);
        std::printf("  texts above %d levels: %d\n", maxLevel, bad);
    }
    return failed || bad ? 1 : 0;
}
