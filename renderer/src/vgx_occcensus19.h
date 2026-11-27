// census19: depth-tested fill census (MSC_VGX_OCCCENSUS, diagnostic, HOST GL ONLY, changes nothing that is drawn).
//
// The earlier censuses (SHCENSUS / FSCENSUS / FGCENSUS / FXCENSUS) add up the projected area of every triangle: no depth test, no alpha test. On the Vita a blended pixel
// behind opaque geometry that was drawn earlier costs nothing, so those figures can overstate a source. This census measures what the host GPU lets through:
// before a blended draw of the main picture is drawn, the same draw is drawn once more, with the same vertex shader, the same fragment shader (its main() renamed and
// followed by a constant output, so every `discard` still discards), the same depth state (depth writes off) and the same scissor, into a one-off colour texture that
// replaces the framebuffer's colour attachment for that draw, blending ONE + ONE. Each fragment that passes the alpha test and the depth test adds 1/255 to the texel,
// so the sum of the red bytes is the number of fragments that pass depth (overlap inside the draw counted, a texel saturates at 255). The attachment is then put back.
// The real draw follows and is untouched: the pictures are the same with the census on or off (hash proof in w8/census19.md).
//
// GLES2 has no SAMPLES_PASSED count (Mesa's ES2 context only has the boolean EXT query), hence this way. The cost is host time only: one extra draw and one read of the
// draw's scissor rectangle per blended draw of a sampled frame.
//
//   MSC_VGX_OCCCENSUS=1|all | every:N | f1,f2,...    which frames (the buffer's frame counter; frames the host does not rasterise, MSC_VGX_LEAD, are not seen)
//   MSC_VGX_OCCCENSUS_SELFTEST=1                     one line `[occ] selftest ...` first: a full-screen quad must count 1.000 screens, a depth-covered half 0.500, two layers 2.000
//   MSC_VGX_OCCCENSUS_TOP=8                          textures listed a frame
// Needs the census copies of static geometry for the area figure: also set MSC_VGX_SHCENSUS=1 (MSC_VGX_SHCENSUS_WINDOW=999999 keeps it quiet).
// Lines: `[occ] f<frame> scene=<other|play|replay|nis> mega=<0|1> rt=<w>x<h> ...` and per class / texture `[occ] f<frame>   ...`. Unit: screens of the picture (rt pixels).
//
// Included inside the anonymous namespace of vgx_backend_gl.cpp, just before Draw().
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

void Draw(const CmdBuf& buf, const DrawRec& r);
bool FxState(const DrawRec& dr);

namespace occ19 {
struct Key {
    int cls;
    uint32_t tex;
    uint8_t layer;
    bool operator<(const Key& o) const { return cls != o.cls ? cls < o.cls : (tex != o.tex ? tex < o.tex : layer < o.layer); }
};
struct Row {
    uint64_t draws = 0, sat = 0;
    double area = 0, pass = 0;
    uint64_t unmeasured = 0;   // draws whose area could not be projected (skinned, no copy of the buffer): their passed pixels are still counted
    uint32_t w = 0, h = 0;
};
struct Saved {
    GLint fbo = 0, colType = 0, colName = 0;
    bool ok = false;
};
struct State {
    bool init = false, on = false, all = false, bad = false, tested = false;
    int every = 0, top = 8, selftest = 0;
    std::set<uint64_t> list;
    uint64_t frame = ~0ull;
    bool sampled = false;
    uint8_t scene = 0;
    bool mega = false;
    int rtW = 0, rtH = 0;
    GLuint cntTex = 0, cntFbo = 0;
    int cntW = 0, cntH = 0;
    bool busy = false;
    std::map<Key, Row> rows;
    uint64_t skipMs = 0, skipSize = 0, skipFs = 0, skipInc = 0, measured = 0;
    std::map<uint16_t, ShaderSource*> fs;   // fragment id | 0x8000 -> census clone (the id is a 16-bit key of the program table)
    std::vector<uint8_t> buf;
};
inline State& St() { static State s; return s; }

inline bool On() {
    State& s = St();
    if (!s.init) {
        s.init = true;
        s.top = static_cast<int>(EnvInt("MSC_VGX_OCCCENSUS_TOP", 8));
        s.selftest = static_cast<int>(EnvInt("MSC_VGX_OCCCENSUS_SELFTEST", 0));
        if (const char* e = std::getenv("MSC_VGX_OCCCENSUS")) {
            const std::string v(e);
            if (v == "all" || v == "1") s.all = true;
            else if (v.compare(0, 6, "every:") == 0) s.every = std::max(1, std::atoi(v.c_str() + 6));
            else if (v != "0" && !v.empty())
                for (size_t i = 0; i < v.size();) {
                    size_t j = v.find(',', i);
                    if (j == std::string::npos) j = v.size();
                    s.list.insert(std::strtoull(v.substr(i, j - i).c_str(), nullptr, 10));
                    i = j + 1;
                }
        }
        s.on = s.all || s.every > 0 || !s.list.empty();
#ifdef __VGX_VITA__
        s.on = false;   // host only
#endif
#ifdef VGX_GL_STUB
        s.on = false;
#endif
    }
    return s.on && !s.bad;
}

inline void Flush() {
    State& s = St();
    if (s.frame == ~0ull || !s.sampled || s.rows.empty()) { s.rows.clear(); return; }
    static const char* const nm[7] = {"fx-additive(marked)", "additive(unmarked)", "soft-blend", "blend+depth-write", "shadow/alpha-only", "other", "indirect(warble)"};
    static const char* const sc[4] = {"other", "play", "replay", "nis"};
    const double screen = static_cast<double>(s.rtW) * s.rtH;
    double area[7] = {}, pass[7] = {};
    uint64_t dr[7] = {};
    double allA = 0, allP = 0;
    for (const auto& kv : s.rows) {
        area[kv.first.cls] += kv.second.area;
        pass[kv.first.cls] += kv.second.pass;
        dr[kv.first.cls] += kv.second.draws;
        allA += kv.second.area;
        allP += kv.second.pass;
    }
    std::fprintf(stderr, "[occ] f%llu scene=%s mega=%d rt=%dx%d blended draws %llu: passed depth %.3f screens, area (no depth test) %.3f screens; not measured: pass/ms %llu size %llu shader %llu incomplete %llu\n",
                 static_cast<unsigned long long>(s.frame), sc[s.scene & 3], s.mega ? 1 : 0, s.rtW, s.rtH, static_cast<unsigned long long>(s.measured), allP / screen, allA / screen,
                 static_cast<unsigned long long>(s.skipMs), static_cast<unsigned long long>(s.skipSize), static_cast<unsigned long long>(s.skipFs), static_cast<unsigned long long>(s.skipInc));
    for (int c = 0; c < 7; ++c)
        if (dr[c]) std::fprintf(stderr, "[occ] f%llu   class %-20s draws %5llu passed %7.3f area %7.3f screens\n", static_cast<unsigned long long>(s.frame), nm[c], static_cast<unsigned long long>(dr[c]), pass[c] / screen, area[c] / screen);
    std::vector<std::pair<double, Key>> top;
    for (const auto& kv : s.rows) top.emplace_back(kv.second.pass, kv.first);
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < top.size() && static_cast<int>(i) < s.top; ++i) {
        const Row& r = s.rows[top[i].second];
        std::fprintf(stderr, "[occ] f%llu     tex%-5u %3ux%-3u layer %2u %-20s draws %4llu passed %7.3f area %7.3f screens%s%s\n", static_cast<unsigned long long>(s.frame), top[i].second.tex, r.w, r.h,
                     static_cast<unsigned>(top[i].second.layer), nm[top[i].second.cls], static_cast<unsigned long long>(r.draws), r.pass / screen, r.area / screen, r.pass / screen > 0.75 ? " >0.75" : "",
                     r.sat ? " SATURATED" : "");
    }
    s.rows.clear();
    s.measured = s.skipMs = s.skipSize = s.skipFs = s.skipInc = 0;
}

inline bool EnsureTarget(int w, int h) {
    State& s = St();
    if (s.cntTex && s.cntW == w && s.cntH == h) return true;
    if (s.cntTex) { G(glDeleteTextures(1, &s.cntTex)); s.cntTex = 0; }
    if (!s.cntFbo) G(glGenFramebuffers(1, &s.cntFbo));
    GLint prevTex = 0, prevFbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    G(glGenTextures(1, &s.cntTex));
    G(glBindTexture(GL_TEXTURE_2D, s.cntTex));
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    G(glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex)));
    G(glBindFramebuffer(GL_FRAMEBUFFER, s.cntFbo));
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.cntTex, 0));
    G(glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo)));
    s.cntW = w;
    s.cntH = h;
    return true;
}

// The colour attachment of the framebuffer that is bound becomes the count texture (cleared). false: not possible here (the caller draws nothing extra).
inline bool Begin(int w, int h, Saved& sv) {
    State& s = St();
    sv = Saved{};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &sv.fbo);
    if (!sv.fbo) return false;
    G(glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &sv.colType));
    G(glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &sv.colName));
    if (sv.colType != GL_TEXTURE) { ++s.skipMs; return false; }
    EnsureTarget(w, h);
    // clear the count texture through its own framebuffer (the scissor test and the colour mask are context state: set aside)
    const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean cm[4];
    GLfloat cc[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    G(glBindFramebuffer(GL_FRAMEBUFFER, s.cntFbo));
    G(glClear(GL_COLOR_BUFFER_BIT));
    G(glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(sv.fbo)));
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.cntTex, 0));
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(sv.colName), 0));
        ++s.skipInc;
        return false;
    }
    sv.ok = true;
    return true;
}
// Put the real attachment back and add up the red bytes of the rectangle (x, y, w, h in framebuffer pixels). sat: texels that reached 255.
inline double End(const Saved& sv, int x, int y, int w, int h, uint64_t& sat) {
    State& s = St();
    sat = 0;
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(sv.colName), 0));
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s.cntW) w = s.cntW - x;
    if (y + h > s.cntH) h = s.cntH - y;
    if (w <= 0 || h <= 0) return 0.0;
    s.buf.resize(static_cast<size_t>(w) * h * 4);
    G(glBindFramebuffer(GL_FRAMEBUFFER, s.cntFbo));
    G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
    G(glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s.buf.data()));
    G(glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(sv.fbo)));
    uint64_t sum = 0;
    for (size_t i = 0; i < s.buf.size(); i += 4) {
        sum += s.buf[i];
        if (s.buf[i] == 255) ++sat;
    }
    return static_cast<double>(sum);
}

// The census clone of a fragment shader: main() renamed, a main() that calls it and then writes 1/255. false: the text has no `void main` to rename.
inline const ShaderSource* CloneFs(uint16_t fsId) {
    State& s = St();
    const uint16_t id = static_cast<uint16_t>(fsId | 0x8000u);
    auto it = s.fs.find(id);
    if (it != s.fs.end()) return it->second;
    ShaderSource* c = nullptr;
    if (fsId < 0x8000u && fsId < g_fsSrc.size() && g_fsSrc[fsId]) {
        const ShaderSource& o = *g_fsSrc[fsId];
        const size_t m = o.text.find("void main");
        if (m != std::string::npos) {
            c = new ShaderSource(o);
            c->id = id;
            c->text.replace(m, 9, "void vgx_occ_main");
            c->text += "\nvoid main() { vgx_occ_main(); gl_FragColor = vec4(0.00392156862745098); }\n";
        }
    }
    s.fs[id] = c;
    if (c) CompileShaderSlot(*c, true);
    return c;
}

inline int ClassOf(const DrawRec& r, const ShaderSource* fs) {
    const RenderState& rs = r.rs;
    if (fs->text.find("vec3 ind0 =") != std::string::npos) return 6;
    if (!rs.colorWrite && rs.alphaWrite) return 4;
    if (FxState(r)) return r.fx ? 0 : 1;
    return rs.depthWrite ? 3 : 2;
}

// Called before the real draw. Measures the draw with the depth state it will meet; the real draw is not touched.
inline void Measure(const CmdBuf& buf, const DrawRec& r) {
    State& s = St();
    if (buf.frame != s.frame) {
        Flush();
        s.frame = buf.frame;
        s.scene = buf.scene;
        s.mega = g_fgMega;
        s.sampled = s.all || (s.every > 0 && buf.frame % static_cast<uint64_t>(s.every) == 0) || s.list.count(buf.frame) != 0;
        if (s.sampled) { s.rtW = g_rtW; s.rtH = g_rtH; }
    }
    if (!s.sampled || s.busy) return;
    if (r.prim != PRIM_TRIANGLES || r.idxCount < 3 || !r.rs.blend) return;
    if (g_inPass || g_fxOpen || g_czPre || g_czUse || g_czRs || g_drawReplay) return;
    if (g_rtW != s.rtW || g_rtH != s.rtH) { ++s.skipSize; return; }
    if (r.fsId >= g_fsSrc.size() || !g_fsSrc[r.fsId]) return;
    const ShaderSource* ofs = g_fsSrc[r.fsId];
    const ShaderSource* cfs = CloneFs(r.fsId);
    if (!cfs) { ++s.skipFs; return; }
    const ShaderSource* vs = r.vsId < g_vsSrc.size() ? g_vsSrc[r.vsId] : nullptr;
    Saved sv;
    if (!Begin(g_rtW, g_rtH, sv)) return;
    DrawRec r2 = r;
    r2.fsId = cfs->id;
    r2.rs.blend = 1;
    r2.rs.blendEq = BE_ADD;
    r2.rs.blendSrc = BF_ONE;
    r2.rs.blendDst = BF_ONE;
    r2.rs.colorWrite = 1;
    r2.rs.alphaWrite = 1;
    r2.rs.depthWrite = 0;
    r2.rs.dstAlphaPass = 0;
    s.busy = true;
    g_drawReplay = true;   // (the other censuses and counters skip a replayed draw)
    Draw(buf, r2);
    g_drawReplay = false;
    s.busy = false;
    int x = 0, y = 0, w = g_rtW, h = g_rtH;
    if (S.scissorOn == 1) { x = S.sc[0]; y = S.sc[1]; w = S.sc[2]; h = S.sc[3]; }
    uint64_t sat = 0;
    const double pass = End(sv, x, y, w, h, sat);
    uint32_t tris = 0, behind = 0;
    ShcExtra ex;
    double area = ShcPixels(buf, r, vs, tris, behind, ex);
    const int cls = ClassOf(r, ofs);
    const int m = ofs->cTexAlphaMap != 0xFF && ofs->cTexAlphaMap < 8 ? ofs->cTexAlphaMap : 0;
    const TexBind& tb = r.tex[m];
    Row& row = s.rows[Key{cls, tb.texId, r.layer}];
    ++row.draws;
    ++s.measured;
    row.pass += pass;
    if (area >= 0.0) row.area += area; else ++row.unmeasured;
    if (sat) ++row.sat;
    if (tb.texId && tb.texId < g_tex.size()) { row.w = g_tex[tb.texId].w; row.h = g_tex[tb.texId].h; }
}

// One line at the first sampled draw: the machinery against quads of known area, in a framebuffer of its own (so the real picture is not involved).
inline void SelfTest(int w, int h) {
    State& s = St();
    static const char* vs = "attribute vec4 a_pos;\nuniform vec4 u_z;\nvoid main() { gl_Position = vec4(a_pos.xy * u_z.xy + u_z.zw, u_z.w * 0.0 + a_pos.z, 1.0); }\n";
    static const char* fs = "precision mediump float;\nvoid vgx_occ_main() { gl_FragColor = vec4(1.0); }\nvoid main() { vgx_occ_main(); gl_FragColor = vec4(0.00392156862745098); }\n";
    GLuint v = G(glCreateShader(GL_VERTEX_SHADER)), f = G(glCreateShader(GL_FRAGMENT_SHADER));
    G(glShaderSource(v, 1, &vs, nullptr));
    G(glCompileShader(v));
    G(glShaderSource(f, 1, &fs, nullptr));
    G(glCompileShader(f));
    GLuint p = G(glCreateProgram());
    G(glAttachShader(p, v));
    G(glAttachShader(p, f));
    G(glBindAttribLocation(p, 0, "a_pos"));
    G(glLinkProgram(p));
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { std::fprintf(stderr, "[occ] selftest: the test program did not link\n"); return; }
    GLuint colTex = 0, dep = 0, fbo = 0, vbo = 0;
    G(glGenTextures(1, &colTex));
    G(glBindTexture(GL_TEXTURE_2D, colTex));
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    G(glGenRenderbuffers(1, &dep));
    G(glBindRenderbuffer(GL_RENDERBUFFER, dep));
    G(glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h));
    G(glGenFramebuffers(1, &fbo));
    G(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colTex, 0));
    G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dep));
    const float quad[12] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};   // (a triangle strip; z is replaced by the vertex shader's constant)
    G(glGenBuffers(1, &vbo));
    G(glBindBuffer(GL_ARRAY_BUFFER, vbo));
    G(glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW));
    G(glUseProgram(p));
    G(glEnableVertexAttribArray(0));
    G(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr));
    const GLint uz = glGetUniformLocation(p, "u_z");
    G(glViewport(0, 0, w, h));
    G(glDisable(GL_SCISSOR_TEST));
    G(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    // The depth buffer holds 1.0 (far). A "wall" over the left half is drawn at depth 0.0 with depth writes on and no colour: the covered half.
    G(glDepthMask(GL_TRUE));
    G(glClearDepthf(1.0f));
    G(glClear(GL_DEPTH_BUFFER_BIT));
    auto drawQuad = [&](float sx, float ox, float depthZ) {
        // u_z = (scale x, scale y, offset x, offset y); the third vertex coordinate carries the depth through a_pos.z, so patch it per call
        float q[12];
        std::memcpy(q, quad, sizeof q);
        for (int i = 0; i < 4; ++i) q[i * 3 + 2] = depthZ;
        G(glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW));
        G(glUniform4f(uz, sx, 1.0f, ox, 0.0f));
        G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    };
    G(glEnable(GL_DEPTH_TEST));
    G(glDepthFunc(GL_LEQUAL));
    G(glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE));
    drawQuad(0.5f, -0.5f, -0.5f);   // clip z -0.5 is window z 0.25: nearer than the quads below (0.0 clip = 0.5 window)
    G(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    G(glDepthMask(GL_FALSE));
    G(glEnable(GL_BLEND));
    G(glBlendEquation(GL_FUNC_ADD));
    G(glBlendFunc(GL_ONE, GL_ONE));
    struct Case { const char* name; int quads; double want; } cases[3] = {{"full-screen quad, no cover", 1, 1.0}, {"same quad, left half behind a depth wall", 1, 0.5}, {"two full-screen layers, no cover", 2, 2.0}};
    double got[3] = {};
    bool good = true;
    for (int c = 0; c < 3; ++c) {
        // count into the framebuffer's own colour attachment replaced by the count texture, exactly as Measure does
        Saved sv;
        if (!Begin(w, h, sv)) { good = false; break; }
        if (c == 1) G(glEnable(GL_DEPTH_TEST));
        else G(glDisable(GL_DEPTH_TEST));
        for (int q = 0; q < cases[c].quads; ++q) drawQuad(1.0f, 0.0f, 0.0f);
        uint64_t sat = 0;
        got[c] = End(sv, 0, 0, w, h, sat) / (static_cast<double>(w) * h);
        if (std::fabs(got[c] - cases[c].want) > 0.0005) good = false;
    }
    std::fprintf(stderr, "[occ] selftest %dx%d: %s %.4f (want 1), %s %.4f (want 0.5), %s %.4f (want 2): %s\n", w, h, cases[0].name, got[0], cases[1].name, got[1], cases[2].name, got[2], good ? "PASS" : "FAIL");
    G(glDisable(GL_BLEND));
    G(glDisable(GL_DEPTH_TEST));
    G(glDepthMask(GL_TRUE));
    G(glDisableVertexAttribArray(0));
    G(glBindBuffer(GL_ARRAY_BUFFER, 0));
    G(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    G(glUseProgram(0));
    G(glDeleteFramebuffers(1, &fbo));
    G(glDeleteRenderbuffers(1, &dep));
    G(glDeleteTextures(1, &colTex));
    G(glDeleteBuffers(1, &vbo));
    G(glDeleteProgram(p));
    G(glDeleteShader(v));
    G(glDeleteShader(f));
    S = GlState();   // every cached GL state is now unknown: the next draw sets what it needs
}
}  // namespace occ19

// The hook (vgx_backend_gl.cpp, Draw): before the real draw of a blended draw on the picture.
inline void OccDraw(const CmdBuf& buf, const DrawRec& r) {
    occ19::State& s = occ19::St();
    if (!s.tested) {
        s.tested = true;
        std::atexit([] { occ19::Flush(); });
        if (s.selftest) occ19::SelfTest(g_rtW > 0 ? g_rtW : 640, g_rtH > 0 ? g_rtH : 448);
    }
    occ19::Measure(buf, r);
}
