// 1.3 MSC_BG30 (owner's idea, 13 Nov: "players at 60, everything else at 30"). Included by vgx_backend_gl.cpp only (it uses that file's GL state helpers).
//
// The game marks every picture of live play (EXT_BG30, CmdBuf::bg30):
//   1 FULL: the whole picture as today. The background is the PREFIX of the main EFB scene (stadium, crowd, sky, far scenery: classes CLS_STADIUM,
//     CLS_CROWD, CLS_SKY, CLS_FAR). Far-scenery draws that come after the players (opaque, depth-tested, depth-writing) are drawn at the end of the prefix
//     instead (same picture: nothing they cover can be in front of them). Right before the first foreground draw the EFB colour is copied into a kept
//     texture (the CAPTURE), with this picture's camera (the game's view matrix and the projection, viewport and depth range of the first prefix draw).
//   2 LIGHT: the game drew no background (its background layer views were not sent; vgx_bg30 drops any background draw that still comes). Before the
//     first EFB draw the capture is drawn over the whole target as the PITCH PLANE (z = 0) seen by this picture's camera, textured from the capture
//     through the capture's camera: an exact homography for the pitch, depth = the plane's depth (rasterised), so the players, their shadows and the
//     stadium's shadow volumes are drawn against it as against the real pitch. Anything not on the plane is off by the parallax (w8/bg30.md: p95 < 1 px
//     at Vice's live camera); the strip a pan reveals takes the capture's edge texels (clamp).
//   3 FULL + RE-USE (MSC_BG30=1, the exactness proof of stage a): a full picture that captures and at once draws its own capture back through the same
//     plane mapping (nearest texels, depth left as the background drew it): the picture must be identical to an ordinary one.
// A light picture whose kept background is older than 4 pictures is still re-projected (counted as "old"). Anything else the plan cannot follow (no background prefix, a background draw after the foreground that cannot move, no perspective projection, a
// corner of the screen that does not see the plane, no capture for a light picture) makes it an ordinary picture and is counted ([bg30] census line).
// Diagnostics: MSC_BG30_BREAK=1 (test): the re-use reads one texel to the right (the proof must catch it). MSC_BG30_PLANEZ=<m> the plane height.
// Video memory: one RGBA8 texture of the EFB target's size (960x544: 2.0 MiB), made at the first full picture of a match, kept while the size holds.

namespace bg30 {

struct Cam {
    float proj[16] = {};   // the GL projection the draws use (column-major, UB_PROJ)
    float view[16] = {};   // the game's view matrix (row vectors)
    float vp[4] = {};      // viewport in EFB units (x, y, w, h)
    float zn = 0, zf = 1;  // depth range
    int efbW = 0, efbH = 0;
};
struct State {
    GLuint tex = 0, fbo = 0, vbo = 0;
    int w = 0, h = 0;
    GLuint prog = 0;
    GLint uSrc = -1;
    bool valid = false;
    uint64_t capFrame = 0;
    Cam cap;               // the capture's camera
    // this buffer
    int kind = 0;
    int capAt = -1, warpAt = -1;
    std::vector<uint8_t> skip;   // per command: drawn elsewhere (a moved far draw) or dropped (a background draw in a light picture)
    std::vector<int> late;
    Cam cam;               // this buffer's camera (full pictures: from the first prefix draw; light: from the first perspective EFB draw)
    // census (since the last line)
    uint64_t nFull = 0, nLight = 0, nReuse = 0, nOff = 0, fbOrder = 0, fbPrefix = 0, fbProj = 0, fbCorner = 0, fbNoCap = 0, fbOld = 0, moved = 0, dropped = 0;
    uint64_t pics = 0;
    uint64_t fbP1 = 0, fbP2 = 0, fbP3 = 0, firstCls[32] = {};   // (wave 13 bg30c) why a prefix fallback: an EFB copy in the prefix / the first EFB draw is not background / no foreground draw after the prefix; classes of the first EFB draw
    // wave 10 (bg30b): the hold's seam. The kind of each planned picture in a ring; the GPU ms judged are given a phase by counting presents (assumes one present per planned picture, GPU times about two pictures late)
    uint8_t ring[64] = {};
    uint64_t planned = 0;
    int64_t seqBase = -1;          // planned index of the picture that present number `seq0` was
    uint32_t seq0 = 0;
    bool hooked = false;
    int cutLevel = 0, cutSamples = 2;
    uint64_t gpuN[2] = {0, 0};
    double gpuSum[2] = {0, 0};
    double errPx = 0;      // largest |old - new| motion of the screen corners seen (px of the target), a bound of the revealed strip
};
State g;

inline bool BgCls(uint8_t c) { return c == CLS_STADIUM || c == CLS_CROWD || c == CLS_SKY || c == CLS_FAR; }
inline bool Persp(const float* p) { return p[11] == -1.0f && p[15] == 0.0f; }
int BreakMode() {
#if defined(__VGX_VITA__)
    return 0;
#else
    static const int b = static_cast<int>(EnvInt("MSC_BG30_BREAK", 0));
    return b;
#endif
}
float PlaneZ() {
    static const float z = [] { const char* e = std::getenv("MSC_BG30_PLANEZ"); return e && *e ? static_cast<float>(std::atof(e)) : 0.0f; }();
    return z;
}

// ---- small matrix helpers (4x4, column-major for GL matrices; the game's view is row-major with row vectors, i.e. the same 16 floats read as a
// column-major matrix M satisfy eye = M * X for column vectors)
void MulV(const float* m, const float* v, float* o) {   // o = m * v (column-major)
    for (int i = 0; i < 4; ++i) o[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
}
bool Inv(const float* m, float* inv) {   // general 4x4 inverse (column-major in, column-major out)
    float t[16];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = static_cast<double>(m[0]) * t[0] + static_cast<double>(m[1]) * t[4] + static_cast<double>(m[2]) * t[8] + static_cast<double>(m[3]) * t[12];
    if (det == 0.0 || !std::isfinite(det)) return false;
    const float r = static_cast<float>(1.0 / det);
    for (int i = 0; i < 16; ++i) inv[i] = t[i] * r;
    return true;
}
// world -> clip of a camera: clip = proj * (view * X)
void Clip(const Cam& c, const float* X, float* clip) {
    float e[4];
    MulV(c.view, X, e);
    MulV(c.proj, e, clip);
}
// The point of the plane z = PlaneZ() seen at target position (tx, ty) in [0,1]^2 (EFB rows top first, as the target's): false if the ray misses it.
bool PlanePoint(const Cam& c, float tx, float ty, float* X) {
    const float nx = (tx * static_cast<float>(c.efbW) - c.vp[0]) / c.vp[2] * 2.0f - 1.0f;
    const float ny = (ty * static_cast<float>(c.efbH) - c.vp[1]) / c.vp[3] * 2.0f - 1.0f;
    float pv[16], ipv[16];
    for (int col = 0; col < 4; ++col) MulV(c.proj, c.view + col * 4, pv + col * 4);   // pv = proj * view
    if (!Inv(pv, ipv)) return false;
    float a[4] = {nx, ny, -1.0f, 1.0f}, b[4] = {nx, ny, 1.0f, 1.0f}, A[4], B[4];
    MulV(ipv, a, A);
    MulV(ipv, b, B);
    if (A[3] == 0.0f || B[3] == 0.0f) return false;
    for (int i = 0; i < 3; ++i) { A[i] /= A[3]; B[i] /= B[3]; }
    const float dz = B[2] - A[2];
    if (std::fabs(dz) < 1e-9f) return false;
    const float s = (PlaneZ() - A[2]) / dz;   // along near -> far
    if (!(s > 0.0f)) return false;
    for (int i = 0; i < 3; ++i) X[i] = A[i] + (B[i] - A[i]) * s;
    X[3] = 1.0f;
    return std::fabs(X[0]) < 4000.0f && std::fabs(X[1]) < 4000.0f;
}

void EnsureProgram() {
    if (g.prog) return;
    static unsigned failures = 0;
    if (failures >= 3) return;
    static const char* vs =
        "attribute vec4 a_pos;\nattribute vec3 a_tex0;\nvarying vec3 v_tex0;\n"
        "void main() { v_tex0 = a_tex0; gl_Position = a_pos; }\n";
    static const char* fs =
        "precision highp float;\nuniform sampler2D u_src;\nvarying vec3 v_tex0;\n"
        "void main() { gl_FragColor = texture2D(u_src, v_tex0.xy / v_tex0.z); }\n";   // (texture2DProj spelled out: the console translator is sure to have this form)
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "bg30 vertex"), f = Compile(GL_FRAGMENT_SHADER, fs, "bg30 fragment");
    if (!v || !f) {
        if (v) G(glDeleteShader(v));
        if (f) G(glDeleteShader(f));
        ++failures;
        return;
    }
    const GLuint p = G(glCreateProgram());
    if (!p || p == 0xFFFFFFFFu) { G(glDeleteShader(v)); G(glDeleteShader(f)); ++failures; return; }
    G(glAttachShader(p, v));
    G(glAttachShader(p, f));
    G(glBindAttribLocation(p, AT_POS, "a_pos"));
    G(glBindAttribLocation(p, AT_TEX0, "a_tex0"));
    G(glLinkProgram(p));
    G(glDeleteShader(v)); G(glDeleteShader(f));
    GLint ok = 0;
    G(glGetProgramiv(p, GL_LINK_STATUS, &ok));
    if (!ok) { G(glDeleteProgram(p)); ++failures; return; }
    g.prog = p;
    UseProgram(p);
    g.uSrc = G(glGetUniformLocation(p, "u_src"));
    G(glUniform1i(g.uSrc, 0));
}

// The kept texture and its framebuffer object, at the target's size (made again when the size changes).
bool EnsureTarget() {
    if (g.tex && g.w == g_rtW && g.h == g_rtH) return true;
    if (g.tex) { ForgetTex(g.tex); G(glDeleteTextures(1, &g.tex)); g.tex = 0; }
    if (g.fbo) { if (S.fbo == g.fbo) { G(glBindFramebuffer(GL_FRAMEBUFFER, 0)); S.fbo = ~0u; } G(glDeleteFramebuffers(1, &g.fbo)); g.fbo = 0; }
    g.valid = false;
    G(glGenTextures(1, &g.tex));
    BindTexForEdit(g.tex);
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_rtW, g_rtH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    G(glGenFramebuffers(1, &g.fbo));
    BindFbo(g.fbo);
    AttachColor(g.tex);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    BindFbo(g_efbFbo);
    g_rsValid = false;
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        VGX_ERR("bg30: kept background %dx%d incomplete (0x%X)\n", g_rtW, g_rtH, st);
        ForgetTex(g.tex);
        G(glDeleteTextures(1, &g.tex));
        G(glDeleteFramebuffers(1, &g.fbo));
        g.tex = g.fbo = 0;
        return false;
    }
    g.w = g_rtW;
    g.h = g_rtH;
    VGX_LOG("bg30: kept background %dx%d (%d KiB)\n", g.w, g.h, g.w * g.h * 4 / 1024);
    return true;
}

// The camera of a draw: its projection block, viewport and depth range, with the buffer's view matrix.
bool CamOf(const CmdBuf& buf, const DrawRec& d, Cam& c) {
    if (d.ublock[UB_PROJ] == kNoBlock || d.ublock[UB_PROJ] + 16 > buf.uni.size()) return false;
    const float* p = buf.uni.data() + d.ublock[UB_PROJ];
    if (!Persp(p)) return false;
    std::memcpy(c.proj, p, sizeof c.proj);
    std::memcpy(c.view, buf.bg30View, sizeof c.view);
    c.vp[0] = d.rs.vpX; c.vp[1] = d.rs.vpY; c.vp[2] = d.rs.vpW; c.vp[3] = d.rs.vpH;
    c.zn = d.rs.zNear; c.zf = d.rs.zFar;
    c.efbW = g_efbW; c.efbH = g_efbH;
    return c.vp[2] > 0 && c.vp[3] > 0;
}

// Plan one buffer (before its commands run). Returns the kind in force (0: an ordinary picture).
// the hold's seam (vgx_hold_bg.h): the pair's background has ONE level, the kept texture's (full size, the samples the picture has); a level above 0 is never "kept", so the ladder's background step
// stays at 0 and a pair that does not fit there is reported (pairFits) and left by the frame-rate controller (NpFpsPairBackground). The per-stadium cut row (np_bg30 NpBg30CutBg) is noted and
// logged as asked; a smaller background target is not built yet.
int HookPhase() {
    hold::BgHooks& h = hold::Bg();
    if (g.planned == 0) return 0;
    if (g.seqBase < 0) { g.seq0 = h.seq; g.seqBase = static_cast<int64_t>(g.planned) - 3 < 0 ? 0 : static_cast<int64_t>(g.planned) - 3; }
    const int64_t idx = g.seqBase + static_cast<int64_t>(static_cast<uint32_t>(h.seq - g.seq0));
    if (idx >= static_cast<int64_t>(g.planned) || static_cast<int64_t>(g.planned) - idx >= 64) return 0;
    const int k = g.ring[static_cast<size_t>(idx) & 63];
    return k == 1 || k == 2 ? k : 0;
}
void HookNoteGpu(int phase, float ms) {
    if (phase != 1 && phase != 2) return;
    g_bg30GpuMs[phase - 1].store(ms, std::memory_order_relaxed);
    g.gpuSum[phase - 1] += ms;
    ++g.gpuN[phase - 1];
}
bool HookApply(int level, int samples, int crowdLow) {
    static int lastL = -1, lastS = -1, lastC = -1;
    if (level != lastL || samples != lastS || crowdLow != lastC) {
        lastL = level; lastS = samples; lastC = crowdLow;
        std::fprintf(stderr, "[bg30] hold asks background level %d, %d samples, crowd %s: %s\n", level, samples, crowdLow ? "reduced" : "full", level == 0 ? "the kept background has only this level" : "no such level");
    }
    return level == 0;
}
bool HookKept(int level) { return level == 0; }
void HookFits(bool fits) {
    std::fprintf(stderr, "[bg30] the pair %s at the bottom of the background ladder (told to the frame-rate controller)\n", fits ? "fits" : "does not fit");
    if (NpFpsPairBackground) NpFpsPairBackground(fits ? 1 : 0);
}
void InstallHooks() {
    if (g.hooked) return;
    g.hooked = true;
    hold::BgHooks& h = hold::Bg();
    h.pairPhase = HookPhase;
    h.bgApply = HookApply;
    h.bgKept = HookKept;
    h.pairFits = HookFits;
    h.noteGpu = HookNoteGpu;
}

int Plan(const CmdBuf& buf, const DrawRec* recs, bool replay) {
    g.kind = 0;
    g.capAt = g.warpAt = -1;
    g.late.clear();
    // the program and the kept target are made before they are needed in play (a shader compile there is a hitch): at the first buffer, when MSC_BG30 is on
    static const bool on = NpBg30ParseMode(std::getenv("MSC_BG30")) != 0;
    static bool warmed = false;
    if (on && !warmed && !replay && g_rtW > 0 && g_rtH > 0) { warmed = true; EnsureProgram(); EnsureTarget(); }
    if (on && !g.hooked && static_cast<bool>(VGX_KNOB(NpBg30ParseMode(std::getenv("MSC_BG30")) == 2))) InstallHooks();
    const int want = replay ? 0 : buf.bg30;
    if (!replay) {   // (the ring is filled with the kind that comes out below: written at each return through NoteKind)
        g.ring[static_cast<size_t>(g.planned) & 63] = 0;
        ++g.planned;
    }
    if (want == 0) return 0;
    ++g.pics;
    const int n = static_cast<int>(buf.cmds.size());
    g.skip.assign(static_cast<size_t>(n), 0);
    bool inPass = false;
    int first = -1;          // the first EFB draw
    int firstFg = -1;        // (full) the first foreground EFB draw after the prefix
    bool ok = true;
    for (int i = 0; i < n && ok; ++i) {
        const Cmd& c = buf.cmds[static_cast<size_t>(i)];
        if (c.type == CMD_PASS_BEGIN) { inPass = true; continue; }
        if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& ct = buf.copyTex[c.arg];
            if (ct.fromPass || inPass) { inPass = false; continue; }
            if (first >= 0 && firstFg < 0 && want != 2) { ok = false; ++g.fbPrefix; ++g.fbP1; }   // (an EFB copy inside the background prefix)
            continue;
        }
        if (c.type == CMD_COPY_DISP || c.type == CMD_SET_TARGET) {
            if (first >= 0) break;
            continue;
        }
        if (c.type != CMD_DRAW || inPass) continue;
        const DrawRec& d = recs[c.arg];
        if (first < 0) { first = i; ++g.firstCls[d.cls & 31]; }
        if (want == 2) {   // light: background draws are not drawn (the game should not have sent any)
            if (BgCls(d.cls)) { g.skip[static_cast<size_t>(i)] = 1; ++g.dropped; }
            continue;
        }
        if (firstFg < 0) {
            if (i == first && !BgCls(d.cls)) {
                ok = false; ++g.fbPrefix; ++g.fbP2;
                static int shown = 0;   // (wave 13 bg30c diagnostic: what the first EFB draw is, and the first background draw after it)
                if (shown < 6 && buf.frame % 7 == 0) {
                    ++shown;
                    int bgAt = -1, nCmdBefore = i, nCls = 0, nDraw = 0;
                    for (int k = 0; k < n; ++k) { const Cmd& ck = buf.cmds[static_cast<size_t>(k)]; if (ck.type == CMD_DRAW) { ++nDraw; if (recs[ck.arg].cls) ++nCls; } }
                    for (int k = i; k < n; ++k) { const Cmd& ck = buf.cmds[static_cast<size_t>(k)]; if (ck.type == CMD_DRAW && BgCls(recs[ck.arg].cls)) { bgAt = k; break; } }
                    char ty[160]; int tn = 0;
                    for (int k = 0; k < i && k < 24 && tn < 150; ++k) tn += std::snprintf(ty + tn, sizeof(ty) - static_cast<size_t>(tn), "%d ", static_cast<int>(buf.cmds[static_cast<size_t>(k)].type));
                    ty[tn] = 0;
                    std::fprintf(stderr, "[bg30] diag f%llu want %d: cmds %d, first EFB draw at cmd %d (cmd types before: %s) vs %u fs %u cls %d layer %d prim %d blend %d depthT %d depthW %d vp %.0f,%.0f,%.0f,%.0f tex0 %ux%u; first background draw at cmd %d (draw cls %d); draws with a class: %d of %d\n",
                                 static_cast<unsigned long long>(buf.frame), want, n, nCmdBefore, ty, d.vsId, d.fsId, d.cls, d.layer, d.prim, d.rs.blend ? 1 : 0, d.rs.depthTest ? 1 : 0, d.rs.depthWrite ? 1 : 0,
                                 d.rs.vpX, d.rs.vpY, d.rs.vpW, d.rs.vpH, 0u, 0u, bgAt, bgAt >= 0 ? recs[buf.cmds[static_cast<size_t>(bgAt)].arg].cls : -1, nCls, nDraw);
                }
                break;
            }   // (no background first: nothing to keep)
            if (!BgCls(d.cls)) firstFg = i;
            continue;
        }
        if (BgCls(d.cls)) {   // a background draw after the foreground: only an opaque far / sky draw that writes depth can move to the prefix
            const RenderState& r = d.rs;
            if ((d.cls == CLS_FAR || d.cls == CLS_SKY) && !r.blend && r.depthTest && r.depthWrite && r.colorWrite) {
                g.late.push_back(i);
                g.skip[static_cast<size_t>(i)] = 1;
            } else {
                ok = false;
                ++g.fbOrder;
            }
        }
    }
    if (!ok || first < 0) { ++g.nOff; return 0; }
    if (want == 2) {
        // the camera: the first EFB draw with a perspective projection
        bool have = false;
        for (int i = first; i < n && !have; ++i) {
            const Cmd& c = buf.cmds[static_cast<size_t>(i)];
            if (c.type == CMD_DRAW && !g.skip[static_cast<size_t>(i)]) have = CamOf(buf, recs[c.arg], g.cam);
            if (c.type == CMD_COPY_DISP) break;
        }
        if (!have) { ++g.fbProj; ++g.nOff; return 0; }
        if (!g.valid || !g.tex) { ++g.fbNoCap; ++g.nOff; return 0; }
        if (buf.frame > g.capFrame + 4) ++g.fbOld;   // (counted, still drawn: an old background re-projected is better than none, the game sent no background)
        // all four corners of the target must see the plane, in the new and in the kept camera
        float X[4];
        for (int k = 0; k < 4; ++k) {
            float c0[4];
            if (!PlanePoint(g.cam, static_cast<float>(k & 1), static_cast<float>(k >> 1), X)) { ++g.fbCorner; ++g.nOff; return 0; }
            Clip(g.cap, X, c0);
            if (!(c0[3] > 0.0f)) { ++g.fbCorner; ++g.nOff; return 0; }
        }
        g.warpAt = first;
        g.kind = 2;
        ++g.nLight;
        g.ring[static_cast<size_t>(g.planned - 1) & 63] = 2;
        return 2;
    }
    if (firstFg < 0) { ++g.fbPrefix; ++g.fbP3; ++g.nOff; return 0; }
    if (!CamOf(buf, recs[buf.cmds[static_cast<size_t>(first)].arg], g.cam)) { ++g.fbProj; ++g.nOff; return 0; }
    if (want == 3) {   // the re-use needs the plane in all four corners too
        float X[4];
        for (int k = 0; k < 4; ++k)
            if (!PlanePoint(g.cam, static_cast<float>(k & 1), static_cast<float>(k >> 1), X)) { ++g.fbCorner; ++g.nOff; return 0; }
    }
    g.capAt = firstFg;
    g.kind = want;
    g.moved += g.late.size();
    if (want == 3) ++g.nReuse; else ++g.nFull;
    if (want == 1) g.ring[static_cast<size_t>(g.planned - 1) & 63] = 1;
    return want;
}

// W5 RT (the console's EFB scene planner): the capture ends the EFB scene at command i.
void RtBreakAt(int i) {
    Rt& rt = g_rt;
    if (!rt.scanned || i < 0 || i >= rt.n || static_cast<int>(rt.brk.size()) != rt.n) return;
    rt.brk[static_cast<size_t>(i)] = 1;
    for (int k = rt.n - 2; k >= 0; --k) rt.nextBreak[static_cast<size_t>(k)] = rt.brk[static_cast<size_t>(k) + 1] ? k + 1 : rt.nextBreak[static_cast<size_t>(k) + 1];
    rt.firstBreak = rt.n > 0 ? (rt.brk[0] ? 0 : rt.nextBreak[0]) : 0;
}

// Draw the kept background through the plane as camera `now` sees it. reuse: the proof (nearest texels, depth left alone).
void Warp(const Cam& now, bool reuse) {
    EnsureProgram();
    if (!g.prog || !g.tex) return;
    float vtx[4 * 7];
    for (int k = 0; k < 4; ++k) {
        const float tx = static_cast<float>(k & 1), ty = static_cast<float>(k >> 1);
        float X[4], c1[4], c0[4];
        if (!PlanePoint(now, tx, ty, X)) return;
        Clip(now, X, c1);
        Clip(g.cap, X, c0);
        // position in the whole target's clip space (the viewport below is the whole target): x_t = (vpX + (x/w * 0.5 + 0.5) vpW) / efbW * 2 - 1, times w
        const float ax = now.vp[2] / static_cast<float>(now.efbW), bx = (now.vp[0] + 0.5f * now.vp[2]) / static_cast<float>(now.efbW) * 2.0f - 1.0f;
        const float ay = now.vp[3] / static_cast<float>(now.efbH), by = (now.vp[1] + 0.5f * now.vp[3]) / static_cast<float>(now.efbH) * 2.0f - 1.0f;
        float* v = vtx + k * 7;
        v[0] = ax * c1[0] + bx * c1[3];
        v[1] = ay * c1[1] + by * c1[3];
        v[2] = c1[2];
        v[3] = c1[3];
        // texture coordinate through the kept camera, homogeneous: u = (vpX + (x/w * 0.5 + 0.5) vpW) / efbW
        const float ux = 0.5f * g.cap.vp[2] / static_cast<float>(g.cap.efbW), uxw = (g.cap.vp[0] + 0.5f * g.cap.vp[2]) / static_cast<float>(g.cap.efbW);
        const float uy = 0.5f * g.cap.vp[3] / static_cast<float>(g.cap.efbH), uyw = (g.cap.vp[1] + 0.5f * g.cap.vp[3]) / static_cast<float>(g.cap.efbH);
        v[4] = ux * c0[0] + uxw * c0[3];
        v[5] = uy * c0[1] + uyw * c0[3];
        v[6] = c0[3];
        if (reuse && BreakMode() == 1) v[4] += v[6] / static_cast<float>(g.w);   // (MSC_BG30_BREAK=1: one texel to the right)
        if (!reuse) {   // the census: how far the corner moved between the two pictures (px of the target), the width of a revealed strip
            const float ox = v[4] / v[6] * static_cast<float>(g.w) - tx * static_cast<float>(g.w), oy = v[5] / v[6] * static_cast<float>(g.h) - ty * static_cast<float>(g.h);
            const double e = std::sqrt(static_cast<double>(ox) * ox + static_cast<double>(oy) * oy);
            if (e > g.errPx) g.errPx = e;
        }
    }
    // state: the whole target, no scissor, no blend, no cull; colour and alpha written; depth: the plane's (light) or left alone (re-use)
    BindFbo(g_efbFbo);
    SetScissor(false);
    SetViewport(0, 0, g_rtW, g_rtH);
    SetDepthRange(now.zn, now.zf);
    SetBlendOff();
    SetCull(CULL_NONE);
    SetColorMask(true, true);
    if (reuse) { SetDepthTest(false); SetDepthMask(false); }
    else { SetDepthTest(true, GL_ALWAYS); SetDepthMask(true); }
    UseProgram(g.prog);
    BindTex(0, g.tex);
    const GLint filt = reuse ? GL_NEAREST : GL_LINEAR;
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt));
    g_uniDirtyV = g_uniDirtyF = false;
    if (!g.vbo) G(glGenBuffers(1, &g.vbo));
    BindArrayBuf(g.vbo);
    G(glBufferData(GL_ARRAY_BUFFER, sizeof vtx, vtx, GL_STREAM_DRAW));
    for (int a = 0; a < AT_COUNT; ++a) {
        const bool want = a == AT_POS || a == AT_TEX0;
        if (want && !(S.attrEnabled & (1u << a))) { G(glEnableVertexAttribArray(static_cast<GLuint>(a))); S.attrEnabled |= 1u << a; }
        if (!want && (S.attrEnabled & (1u << a))) { G(glDisableVertexAttribArray(static_cast<GLuint>(a))); S.attrEnabled &= ~(1u << a); }
        S.attrConstSet &= ~(1u << a);
    }
    G(glVertexAttribPointer(AT_POS, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr));
    G(glVertexAttribPointer(AT_TEX0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<const void*>(4 * sizeof(float))));
    S.layoutVbo = ~0u;   // (the next draw sets its own layout)
    S.layoutMask = 0;
    TouchTarget(OP_DRAW);
    G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    ++g_stats.drawCallsThisFrame;
    g_rsValid = false;
}

// Full pictures: the moved far draws, then the copy of the EFB colour (and, for the proof, its re-use).
template <class DrawFn>
void Capture(const CmdBuf& buf, const DrawRec* recs, DrawFn draw) {
    for (const int i : g.late) draw(i, recs[buf.cmds[static_cast<size_t>(i)].arg]);
    if (!EnsureTarget()) { g.valid = false; return; }
    BindFbo(g.fbo);   // (the desktop's multisampled EFB is resolved by this bind; the console's EFB scene ends and stores its resolved colour)
    SetScissor(false);
    SetViewport(0, 0, g.w, g.h);
    SetBlendOff();
    SetDepthTest(false);
    SetDepthMask(false);
    SetCull(CULL_NONE);
    SetColorMask(true, true);
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero[4] = {0, 0, 0, 0};
    // (1:1 at texel centres: any filter returns the texel itself, so the EFB texture's own filter is left as it is)
    DrawQuad(g_efbColor, 0.0f, 0.0f, 1.0f, 1.0f, ident, zero);
    BindFbo(g_efbFbo);
    g.cam.efbW = g_efbW;
    g.cam.efbH = g_efbH;
    g_rsValid = false;
    g.cap = g.cam;
    g.capFrame = buf.frame;
    g.valid = true;
    if (g.kind == 3) Warp(g.cam, true);
}

void CensusLine(uint64_t frame) {
    if (g.pics == 0 || frame % 600 != 0) return;
    std::fprintf(stderr, "[bg30] f%llu pictures %llu: full %llu, light %llu, re-use %llu, ordinary %llu | fallbacks: prefix %llu order %llu projection %llu corner %llu no-capture %llu old %llu"
                         " | far draws moved %llu, background draws dropped %llu | corner motion max %.1f px | kept %dx%d\n",
                 static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g.pics), static_cast<unsigned long long>(g.nFull), static_cast<unsigned long long>(g.nLight),
                 static_cast<unsigned long long>(g.nReuse), static_cast<unsigned long long>(g.nOff), static_cast<unsigned long long>(g.fbPrefix), static_cast<unsigned long long>(g.fbOrder),
                 static_cast<unsigned long long>(g.fbProj), static_cast<unsigned long long>(g.fbCorner), static_cast<unsigned long long>(g.fbNoCap), static_cast<unsigned long long>(g.fbOld),
                 static_cast<unsigned long long>(g.moved), static_cast<unsigned long long>(g.dropped), g.errPx, g.w, g.h);
    {
        char fc[256]; int n = 0; fc[0] = 0;
        for (int k = 0; k < 32 && n < 220; ++k) if (g.firstCls[k]) n += std::snprintf(fc + n, sizeof(fc) - static_cast<size_t>(n), " cls%d=%llu", k, static_cast<unsigned long long>(g.firstCls[k]));
        std::fprintf(stderr, "[bg30] prefix fallbacks by reason: EFB copy in the prefix %llu, first EFB draw not background %llu, no foreground after the prefix %llu | class of the first EFB draw:%s\n",
                     static_cast<unsigned long long>(g.fbP1), static_cast<unsigned long long>(g.fbP2), static_cast<unsigned long long>(g.fbP3), fc);
        g.fbP1 = g.fbP2 = g.fbP3 = 0; std::memset(g.firstCls, 0, sizeof(g.firstCls));
    }
    g.pics = g.nFull = g.nLight = g.nReuse = g.nOff = g.fbPrefix = g.fbOrder = g.fbProj = g.fbCorner = g.fbNoCap = g.fbOld = g.moved = g.dropped = 0;
    g.errPx = 0;
}

} // namespace bg30
