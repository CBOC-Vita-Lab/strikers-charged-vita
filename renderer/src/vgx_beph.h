// be24 MSC_VGX_BEPH=0|1|N (default 0; a measuring aid, no behaviour): where the render thread's time goes, per scene class, in a line every N pictures (1: every 300).
// Included by vgx_backend_gl.cpp only, before Draw() (it uses MicroNow() and the call counters C of that file).
//
// Why: no console log had the back end split by phase. v1 (be24) timed WALL time only: the first console run (17 Nov, bephbase) showed 49 ms a picture in the goal cut-scene against
// 21-24 ms of thread CPU in the same run's [lane] line (be 20.9/36.5: CPU / wall), so about half of every phase was a wait (the display copy's swap and the GPU queue limit, a scene end,
// a full command ring) and the "cpu" of the line was wall time.
// be25 (v2):
//   - every phase and every sampled draw phase has BOTH figures, wall/cpu in ms (us for the draw phases): the difference is what the thread waited for;
//   - only every MSC_VGX_BEPH_EVERY-th picture (default 4) is timed, and one draw in MSC_VGX_BEPH_DRAWS (default 32) inside it (v1 read the clock 286 times a picture);
//   - the cost of one mark (wall read + thread-cpu read) is measured once and taken out of every span, so the figures are net;
//   - the host's wait in the swap and the GPU queue limit (VgxHostWaitUs, counted only when MSC_VGX_BEPH is set) and the clock reads of the rest of the back end are printed;
//   - MSC_VGX_BEPH_CPU=0 leaves out the thread-cpu read (wall figures only, a cheaper mark).
// The line: [beph] scene <n>(<name>) <pics> timed pictures ...: wall <w> cpu <c> ms/pic | prep: house <w/c> ct <w/c> upload <w/c> scans <w/c> plans <w/c> | commands: draw <w/c> upload ... |
//   draws/pic <n>, sampled <m>: us per draw (wall/cpu): pre .. state .. uniforms .. textures .. layout .. issue .. (sum ..) | per picture: glCalls scenes uniformCalls uniformBytes ringBytes ...
// The sampled draw phases: pre = program look-up and every census/plan test up to the state; state = ApplyRenderState; uniforms = UseProgram and the uniform blocks; textures =
// BindTexture x n; layout = SetLayout / direct constants / element buffer; issue = the glDrawElements (vitaGL: the program, textures, uniform ring and sceGxmDraw).
namespace beph {
enum Cmd { K_DRAW, K_UPLOAD, K_FREE, K_SHADER, K_COPYTEX, K_COPYDISP, K_PRESENT, K_TARGET, K_BUFUP, K_BUFFREE, K_PASS, K_PROGRAM, K_VARIANT, K_NCMD };
enum Pre { P_HOUSE, P_ATL, P_CT, P_DYN, P_UPLOAD, P_FXD, P_SHQ, P_SCANS, P_PLANS, P_NPRE };   // (house, AtlateApply, CtScan, dyncache::Prepare, UploadStream, FxDecide, ShqScan with the shadow ledger, the other scans, the rest up to the first command)
enum Sub { S_PRE, S_STATE, S_UNI, S_TEX, S_LAY, S_ISSUE, S_N };
extern "C" double NpFpsPlatformThreadClockMs(void) __attribute__((weak));
extern "C" uint64_t VgxHostWaitUs(void) __attribute__((weak));   // (host: us spent in the swap and the GPU queue limit so far; counted only when MSC_VGX_BEPH is set)
struct Snap { uint32_t t = 0; double u = 0.0; };   // wall (us, 32 bit) and the thread's cpu (us) at a mark
struct Class {
    uint32_t pics = 0;
    double all = 0, allCpu = 0, wait = 0;
    double pre[P_NPRE] = {}, preC[P_NPRE] = {}, cmd[K_NCMD] = {}, cmdC[K_NCMD] = {};
    double sub[S_N] = {}, subC[S_N] = {};
    uint64_t draws = 0, sampled = 0;
    uint64_t reads = 0, otherReads = 0;
    uint64_t cnt[15] = {};
    uint64_t cDraws[16] = {}, cStaticDraws[16] = {}, cVtx[16] = {}, cBytes[16] = {};   // by draw class (CLS_*): dynamic draws, static-buffer draws, vertices and bytes of the dynamic ones
};
inline int g_mode = -1;        // 0 off, else pictures per line
inline Class g_cls[4];
inline int g_scene = 0;
inline Snap g_last, g_t0;
inline int g_cur = -1;         // command kind charged at the next mark (-1: in the preparation)
inline Snap g_sub[S_N + 1];
inline bool g_subSet[S_N + 1];
inline uint32_t g_sampleNth = 0;
inline uint32_t g_readsNow = 0;
inline bool g_inPic = false;
inline bool g_cpuOn = false;
inline uint32_t g_every = 4, g_drawEvery = 32, g_picN = 0;
inline double g_cw = 0.0, g_cc = 0.0;   // wall us and thread-cpu us that one mark costs, measured once
inline double g_wait0 = 0.0;
inline uint32_t g_other0 = 0;           // g_clockCalls at Begin (the back end's own reads)
inline double CpuUs() {
#if defined(__VGX_VITA__)
    return &NpFpsPlatformThreadClockMs ? NpFpsPlatformThreadClockMs() * 1000.0 : -1.0;
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return -1.0;
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
#endif
}
inline double WaitUs() { return &VgxHostWaitUs ? static_cast<double>(VgxHostWaitUs()) : 0.0; }
inline Snap Take() {
    ++g_readsNow;
    Snap s;
    s.t = MicroNow();
    if (g_cpuOn) s.u = CpuUs();
    return s;
}
inline bool On() {
    if (g_mode < 0) {
        const long v = EnvInt("MSC_VGX_BEPH", 0);
        g_mode = v <= 0 ? 0 : (v == 1 ? 300 : static_cast<int>(v));
        if (g_mode > 0) {
            const long e = EnvInt("MSC_VGX_BEPH_EVERY", 4), d = EnvInt("MSC_VGX_BEPH_DRAWS", 32);
            g_every = e < 1 ? 1u : static_cast<uint32_t>(e);
            g_drawEvery = d < 1 ? 1u : static_cast<uint32_t>(d);
            g_cpuOn = EnvInt("MSC_VGX_BEPH_CPU", 1) != 0 && CpuUs() >= 0.0;
            Snap a = Take(), z = a;
            for (int i = 0; i < 200; ++i) z = Take();
            g_cw = static_cast<double>(z.t - a.t) / 200.0;
            g_cc = g_cpuOn ? (z.u - a.u) / 200.0 : 0.0;
        }
    }
    return g_mode > 0;
}
// A switch word read in the hot path without the guard: set once by Begin's first call.
inline bool Live() { return g_mode > 0 && g_inPic; }
inline double NetW(uint32_t d) { const double v = static_cast<double>(d) - g_cw; return v > 0.0 ? v : 0.0; }   // (a span holds one mark's cost besides the work)
inline double NetC(double d) { const double v = d - g_cc; return v > 0.0 ? v : 0.0; }
inline void Begin(int scene) {
    g_inPic = false;
    if ((++g_picN % g_every) != 0) return;
    g_scene = scene & 3;
    g_inPic = true;
    g_readsNow = 0;
    g_cur = -1;
    g_wait0 = WaitUs();
    g_other0 = g_clockCalls;
    g_t0 = g_last = Take();
}
// charge the time since the last mark to the preparation step `p`
inline void Pre(int p) {
    if (!Live()) return;
    const Snap s = Take();
    Class& c = g_cls[g_scene];
    c.pre[p] += NetW(s.t - g_last.t);
    c.preC[p] += NetC(s.u - g_last.u);
    g_last = s;
}
// a command of kind `k` is about to run: charge what ran before it
inline void Cmd(int k) {
    if (!Live() || k == g_cur) return;
    const Snap s = Take();
    Class& c = g_cls[g_scene];
    if (g_cur >= 0) { c.cmd[g_cur] += NetW(s.t - g_last.t); c.cmdC[g_cur] += NetC(s.u - g_last.u); }
    else { c.pre[P_PLANS] += NetW(s.t - g_last.t); c.preC[P_PLANS] += NetC(s.u - g_last.u); }
    g_last = s;
    g_cur = k;
}
// one per Draw(): every g_drawEvery-th draw reads the clocks at the end of each sub-phase
struct DrawScope {
    bool sampled = false;
    DrawScope() {
        if (!Live()) return;
        ++g_cls[g_scene].draws;
        if ((++g_sampleNth % g_drawEvery) != 0) return;
        sampled = true;
        for (bool& b : g_subSet) b = false;
        g_sub[S_N] = Take();
    }
    void Census(const DrawRec& r) {   // (the per-class bytes the frame's stream uploads carry)
        if (!Live()) return;
        Class& c = g_cls[g_scene];
        const int k = r.cls < 16 ? r.cls : 15;
        if (r.vbuf) { ++c.cStaticDraws[k]; return; }
        ++c.cDraws[k];
        c.cVtx[k] += r.vtxCount;
        c.cBytes[k] += static_cast<uint64_t>(r.vtxCount) * MakeLayout(r.layoutMask).stride;
    }
    void Mark(int s) { if (sampled) { g_sub[s] = Take(); g_subSet[s] = true; } }   // the end of sub-phase s (in order; one that is not reached takes nothing)
    ~DrawScope() {
        if (!sampled) return;
        Class& c = g_cls[g_scene];
        Snap prev = g_sub[S_N];
        ++c.sampled;
        for (int s = 0; s < S_N; ++s) {
            if (!g_subSet[s]) continue;
            c.sub[s] += NetW(g_sub[s].t - prev.t);
            c.subC[s] += NetC(g_sub[s].u - prev.u);
            prev = g_sub[s];
        }
    }
};
inline void End(const Counts& C) {
    if (!Live()) return;
    const Snap s = Take();
    Class& c = g_cls[g_scene];
    if (g_cur >= 0) { c.cmd[g_cur] += NetW(s.t - g_last.t); c.cmdC[g_cur] += NetC(s.u - g_last.u); }
    c.all += static_cast<double>(s.t - g_t0.t) - g_cw * static_cast<double>(g_readsNow - 1);
    c.allCpu += (s.u - g_t0.u) - g_cc * static_cast<double>(g_readsNow - 1);
    c.reads += g_readsNow;
    c.otherReads += (g_clockCalls - g_other0) - g_readsNow;
    c.wait += WaitUs() - g_wait0;
    ++c.pics;
    c.cnt[0] += C.glCalls; c.cnt[1] += C.scenes; c.cnt[2] += C.uniformCalls; c.cnt[3] += C.uniformBytes; c.cnt[4] += C.ringBytes; c.cnt[5] += C.texBinds;
    c.cnt[6] += C.layoutChanges; c.cnt[7] += C.programUses; c.cnt[8] += C.stateCalls + C.scissorCalls + C.viewportCalls; c.cnt[9] += C.vboBinds; c.cnt[10] += C.streamUploads; c.cnt[11] += C.mergeable; c.cnt[12] += C.cmpCalls; c.cnt[13] += C.cmpBytes; c.cnt[14] += C.bepfDraws;
    g_inPic = false;
    if (c.pics >= static_cast<uint32_t>(g_mode)) {
        static const char* const sn[4] = {"menu", "play", "replay", "cut"};
        static const char* const cn[K_NCMD] = {"draw", "upload", "free", "shader", "copytex", "copydisp", "present", "target", "bufup", "buffree", "pass", "program", "variant"};
        static const char* const pn[P_NPRE] = {"house", "atlate", "ct", "dyncache", "upload", "fxdecide", "shq", "scans", "plans"};
        static const char* const un[S_N] = {"pre", "state", "uniforms", "textures", "layout", "issue"};
        const double n = c.pics, ms = 1.0 / 1000.0;
        std::fprintf(stderr, "[beph] scene %d(%s) %u timed pictures (every %u-th, 1 draw in %u): wall %.2f cpu %.2f ms/pic (net of the marks; host swap+queue wait %.2f) | one mark %.2f us wall %.2f us cpu; "
                             "this profiler %.1f marks/pic, the back end's own clock reads %.1f/pic | wall/cpu, ms | prep:",
                     g_scene, sn[g_scene], c.pics, g_every, g_drawEvery, c.all / n * ms, c.allCpu / n * ms, c.wait / n * ms, g_cw, g_cc, c.reads / n, c.otherReads / n);
        for (int i = 0; i < P_NPRE; ++i) std::fprintf(stderr, " %s %.2f/%.2f", pn[i], c.pre[i] / n * ms, c.preC[i] / n * ms);
        std::fprintf(stderr, " | commands:");
        for (int i = 0; i < K_NCMD; ++i) if (c.cmd[i] > 0 || c.cmdC[i] > 0) std::fprintf(stderr, " %s %.2f/%.2f", cn[i], c.cmd[i] / n * ms, c.cmdC[i] / n * ms);
        std::fprintf(stderr, " | draws/pic %.1f, sampled %.1f/pic, us per draw wall/cpu:", c.draws / n, c.sampled / n);
        double sum = 0, sumC = 0;
        for (int i = 0; i < S_N; ++i) {
            const double w = c.sampled ? c.sub[i] / static_cast<double>(c.sampled) : 0.0, u = c.sampled ? c.subC[i] / static_cast<double>(c.sampled) : 0.0;
            sum += w; sumC += u;
            std::fprintf(stderr, " %s %.2f/%.2f", un[i], w, u);
        }
        std::fprintf(stderr, " (sum %.2f/%.2f) | per pic: glCalls %.0f scenes %.1f uniCalls %.0f uniBytes %.0f ringBytes %.0f texBinds %.0f layouts %.0f progUses %.0f stateCalls %.0f vboBinds %.0f streamUp %.1f mergeable %.1f uniCompares %.0f (%.1f KiB) prefetchedDraws %.0f\n", sum, sumC,
                     c.cnt[0] / n, c.cnt[1] / n, c.cnt[2] / n, c.cnt[3] / n, c.cnt[4] / n, c.cnt[5] / n, c.cnt[6] / n, c.cnt[7] / n, c.cnt[8] / n, c.cnt[9] / n, c.cnt[10] / n, c.cnt[11] / n, c.cnt[12] / n, c.cnt[13] / n / 1024.0, c.cnt[14] / n);
        std::fprintf(stderr, "[beph] scene %d stream data by draw class per picture (class: dynamic draws / vertices / KiB, static-buffer draws):", g_scene);
        for (int k = 0; k < 16; ++k)
            if (c.cDraws[k] || c.cStaticDraws[k]) std::fprintf(stderr, " c%d: %.1f / %.0f / %.1f, %.1f;", k, c.cDraws[k] / n, c.cVtx[k] / n, c.cBytes[k] / n / 1024.0, c.cStaticDraws[k] / n);
        std::fprintf(stderr, "\n");
        c = Class{};
    }
}
}   // namespace beph
