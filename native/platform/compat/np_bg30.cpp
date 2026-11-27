// 1.3 MSC_BG30 (np/bg30.h): the per-picture decision (full / light / proof re-use), the record for the renderer and the send-side skip.
// Render side only; nothing here is read by the simulation.
#include "np/bg30.h"
#include "np/bg30_env.h"
#include "np/framesend.h"
#include "np/hot_knob.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void np_gx_bg30(unsigned int kind, const float* view) __attribute__((weak));   // platform/gx (EXT_BG30)
bool NpLanActive() __attribute__((weak));   // LAN / online sessions are never paired (the pair controller excludes them too)
extern unsigned int gNetworkRandomSeed __attribute__((weak));   // (touched only by the test-only MSC_BG30_BREAK=2 mutant: the determinism check must catch it)

namespace {
struct State {
    int kind = 0;           // the picture being built
    int prevKind = 0;
    bool havePrev = false;
    float prevView[16] = {};
    float prevFov = 0;
    unsigned long pics = 0, full = 0, light = 0, reuse = 0, none = 0, cuts = 0, horizon = 0, crowdSkips = 0;
    unsigned long why[NPBG30_WHY_COUNT] = {};
    unsigned long held30 = 0, stadiumSkips = 0;
    unsigned long pans = 0, balCrowdDo = 0, balCrowdSkip = 0, balStadDo = 0, balStadSkip = 0;
    bool crowdPrep = false, stadPrep = false;
    float lastPan = 0, maxPan = 0;
    bool heldEligible = false;
    double emaFull = 0, emaLight = 0;
    unsigned long nFull = 0, nLight = 0;
};
State s;
volatile int s_sendLight = 0;   // the picture being sent (set in stream order, after the previous send's join)

int ModeFromEnv() {
    return NpBg30ParseMode(getenv("MSC_BG30"));   // (np/bg30_env.h: the one parser, shared with NpViewLayers and the renderer)
}
int LogOn() {
    const char* e = getenv("MSC_BG30_LOG");
    return !(e && *e == '0');
}

struct Rec {
    unsigned int kind;
    float view[16];
};
void EmitCall(const void* data) {
    Rec r;
    memcpy(&r, data, sizeof r);
    s_sendLight = r.kind == NPBG30_LIGHT;
    if (np_gx_bg30) np_gx_bg30(r.kind, r.view);
}
// MSC_BG30_BREAK=2 (test only): a light picture leaks into the simulation (its seed is stirred): NP_DET_TRACE must differ from MSC_BG30=0.
void BreakLeak(int kind) {
    static const int brk = [] { const char* e = getenv("MSC_BG30_BREAK"); return e && *e ? atoi(e) : 0; }();
    if (brk == 2 && kind == NPBG30_LIGHT && &gNetworkRandomSeed != 0) gNetworkRandomSeed = gNetworkRandomSeed * 1103515245u + 12345u;
}
}  // namespace

extern "C" {

int NpBg30Mode(void) { return NP_HOT_KNOB(int, ModeFromEnv()); }
int NpBg30ScreensStrict(void) {
    static const int v = [] { const char* e = getenv("MSC_BG30_SCREENS"); return e && *e && atoi(e) > 0 ? 1 : 0; }();
    return v;
}

int NpBg30CameraJump(const float* a, const float* b, float fovA, float fovB) {
    // camera positions: p = -T R^T (row vectors: eye = X R + T, R orthonormal)
    float pa[3], pb[3];
    for (int j = 0; j < 3; ++j) {
        pa[j] = -(a[12] * a[j * 4 + 0] + a[13] * a[j * 4 + 1] + a[14] * a[j * 4 + 2]);
        pb[j] = -(b[12] * b[j * 4 + 0] + b[13] * b[j * 4 + 1] + b[14] * b[j * 4 + 2]);
    }
    const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
#if defined(NP_BG30_BREAK) && NP_BG30_BREAK == 3
    if (dx * dx + dy * dy + dz * dz > 100.0f) return 1;   // (mutant: a 10 m threshold)
#else
    if (dx * dx + dy * dy + dz * dz > 1.0f) return 1;
#endif
    // view direction (the third column of R, negated): more than 1 degree apart is a cut too
    float dot = 0;
    for (int j = 0; j < 3; ++j) dot += a[j * 4 + 2] * b[j * 4 + 2];
    if (dot < 0.99985f) return 1;   // cos(1 degree)
    if (fabsf(fovA - fovB) > 1.0f) return 1;
    return 0;
}

int NpBg30SeesPlane(const float* v, float fovDeg, float aspect) {
    // world directions of the eye axes: column c of R (rows 0..2) is eye axis c in world coordinates
    const float ty = tanf(fovDeg * 0.5f * 3.14159265f / 180.0f), tx = ty * aspect;
    for (int side = -1; side <= 1; side += 2) {
        float d[3];
        for (int j = 0; j < 3; ++j) d[j] = side * tx * v[j * 4 + 0] + ty * v[j * 4 + 1] - v[j * 4 + 2];
        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(d[2] < -0.05f * len)) return 0;   // the top corners must look down by about 3 degrees at least
    }
    return 1;
}

float NpBg30PanPx(const float* a, const float* b, float fovA, float fovB, int w, int h) {
    // the plane z = 0 seen by b at its screen corners and centre, projected by a (row vectors: eye = X R + T; the eye looks down -z)
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    const float tyB = tanf(fovB * 0.5f * 3.14159265f / 180.0f), txB = tyB * aspect;
    const float tyA = tanf(fovA * 0.5f * 3.14159265f / 180.0f), txA = tyA * aspect;
    float p[3];
    for (int j = 0; j < 3; ++j) p[j] = -(b[12] * b[j * 4 + 0] + b[13] * b[j * 4 + 1] + b[14] * b[j * 4 + 2]);
    if (!(p[2] > 0.0f)) return -1.0f;
    static const float pts[5][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {0, 0}};
    float worst = 0.0f;
    for (int k = 0; k < 5; ++k) {
        float d[3];
        for (int j = 0; j < 3; ++j) d[j] = pts[k][0] * txB * b[j * 4 + 0] + pts[k][1] * tyB * b[j * 4 + 1] - b[j * 4 + 2];
        if (!(d[2] < -1e-4f)) return -1.0f;
        const float s = -p[2] / d[2];
        float X[3];
        for (int j = 0; j < 3; ++j) X[j] = p[j] + d[j] * s;
        float e[3];
        for (int c = 0; c < 3; ++c) e[c] = X[0] * a[0 * 4 + c] + X[1] * a[1 * 4 + c] + X[2] * a[2 * 4 + c] + a[12 + c];
        if (!(e[2] < -1e-4f)) return -1.0f;
        const float nx = e[0] / (-e[2] * txA), ny = e[1] / (-e[2] * tyA);
        const float dx = (nx - pts[k][0]) * 0.5f * static_cast<float>(w), dy = (ny - pts[k][1]) * 0.5f * static_cast<float>(h);
        const float m = sqrtf(dx * dx + dy * dy);
        if (m > worst) worst = m;
    }
    return worst;
}

#if defined(NP_BG30_BREAK) && NP_BG30_BREAK == 5
static bool defined_break5() { return true; }   // (mutant 5: no fast-pan fallback)
#else
static bool defined_break5() { return false; }
#endif
namespace {
float PanLimit() {
    static const float v = [] { const char* e = getenv("MSC_BG30_PANPX"); return e && *e ? static_cast<float>(atof(e)) : 12.0f; }();
    return v;
}
int BalanceBits() {
    static const int v = [] { const char* e = getenv("MSC_BG30_BALANCE"); return e && *e ? atoi(e) : 0; }();
    return v;
}
struct CutRow { int stadium, level, samples, crowdLow, ambientOff; };
CutRow g_cuts[24];
int g_nCuts = 0;
const CutRow* g_cutNow = nullptr;
void LoadCuts() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* e = getenv("MSC_BG30_CUTS");   // the table: empty until the console numbers say which stadium needs which cut
    while (e && *e && g_nCuts < 24) {
        CutRow r = {-1, 0, 2, 0, 0};
        int n = 0;
        if (sscanf(e, "%d:%d:%d:%d:%d%n", &r.stadium, &r.level, &r.samples, &r.crowdLow, &r.ambientOff, &n) >= 1 && n > 0) g_cuts[g_nCuts++] = r;
        const char* c = strchr(e, ',');
        if (!c) break;
        e = c + 1;
    }
}
}  // namespace

int NpBg30Decide(const NpBg30In* in) {
    int mode = NpBg30Mode();
    int kind = NPBG30_NONE;
    if (mode != 0 && &NpLanActive != 0 && NpLanActive()) mode = 0;   // (bg30b: no pairs in a LAN or online session)
    if (mode != 0) {
        ++s.pics;
        if (!in->eligible) {
            if (in->why > 0 && in->why < NPBG30_WHY_COUNT) ++s.why[in->why];
        } else if (!NpBg30SeesPlane(in->view, in->fovDeg, 1.8f)) {
            ++s.horizon;
        } else if (mode == 2 && !in->rate60) {
            ++s.held30;   // (held at 30: every picture full and ordinary; the controller hears what pairs would save)
        } else if (mode == 1) {
            kind = NPBG30_REUSE;
        } else {
            bool cut = s.havePrev && NpBg30CameraJump(s.prevView, in->view, s.prevFov, in->fovDeg);
            if (cut) ++s.cuts;
            // wave 10: a pan faster than the limit would reveal a strip the kept background does not have (research-splitrate-pacing 2): this picture starts a new pair as a full one
            if (!cut && s.havePrev && s.prevKind == NPBG30_FULL && PanLimit() > 0.0f && !(defined_break5())) {
                const float pan = NpBg30PanPx(s.prevView, in->view, s.prevFov, in->fovDeg, 960, 544);
                s.lastPan = pan;
                if (pan > s.maxPan) s.maxPan = pan;
                if (pan > PanLimit() || pan < 0.0f) { cut = true; ++s.pans; }
            }
#if !(defined(NP_BG30_BREAK) && NP_BG30_BREAK == 4)   // (mutant 4: a cut or a fast pan leaves what the light picture prepared: the full picture after it would skip its update with nothing fresh)
            if (cut) { s.crowdPrep = false; s.stadPrep = false; }
#endif
#if defined(NP_BG30_BREAK) && NP_BG30_BREAK == 1
            kind = (s.prevKind == NPBG30_FULL) ? NPBG30_LIGHT : NPBG30_FULL;   // (mutant: a cut does not start a new pair)
#elif defined(NP_BG30_BREAK) && NP_BG30_BREAK == 2
            kind = (s.prevKind != NPBG30_LIGHT && !cut) ? NPBG30_LIGHT : NPBG30_FULL;   // (mutant: light after an ordinary picture)
#else
            kind = (s.prevKind == NPBG30_FULL && !cut) ? NPBG30_LIGHT : NPBG30_FULL;
#endif
        }
        switch (kind) {
        case NPBG30_FULL: ++s.full; break;
        case NPBG30_LIGHT: ++s.light; break;
        case NPBG30_REUSE: ++s.reuse; break;
        default: ++s.none; break;
        }
        if (LogOn() && s.pics % 600 == 0) {
            fprintf(stderr, "[bg30g] mode %d last 600 pictures: full %lu light %lu re-use %lu ordinary %lu (not live play %lu, camera %lu, transition %lu, mega %lu,"
                            " captain %lu, pip %lu, replay camera %lu, screen %lu, horizon %lu, held at 30 %lu, cuts %lu, fast pans %lu (largest %.1f px)) crowd skipped %lu, stadium update skipped %lu | balance: crowd update in light %lu / skipped in full %lu, stadium update in light %lu / skipped in full %lu | game thread a picture: full %.2f ms, light %.2f ms"
                            " (%lu / %lu seen), pair saving used %.2f ms\n",
                    mode, s.full, s.light, s.reuse, s.none, s.why[NPBG30_WHY_STATE], s.why[NPBG30_WHY_CAMERA], s.why[NPBG30_WHY_TRANSITION], s.why[NPBG30_WHY_MEGA],
                    s.why[NPBG30_WHY_CAPTAIN], s.why[NPBG30_WHY_PIP], s.why[NPBG30_WHY_REPLAYCAM], s.why[NPBG30_WHY_SCREEN], s.horizon, s.held30, s.cuts, s.pans, s.maxPan, s.crowdSkips, s.stadiumSkips,
                    s.balCrowdDo, s.balCrowdSkip, s.balStadDo, s.balStadSkip, s.emaFull, s.emaLight,
                    s.nFull, s.nLight, NpBg30PairSavingMs());
            s.full = s.light = s.reuse = s.none = s.horizon = s.cuts = s.crowdSkips = s.held30 = s.stadiumSkips = 0;
            s.pans = s.balCrowdDo = s.balCrowdSkip = s.balStadDo = s.balStadSkip = 0;
            s.maxPan = 0;
            memset(s.why, 0, sizeof s.why);
        }
    }
    BreakLeak(kind);
    if (kind != NPBG30_LIGHT && kind != NPBG30_FULL) { s.crowdPrep = false; s.stadPrep = false; }   // (a break in the pair: nothing is prepared)
    s.heldEligible = mode == 2 && in->eligible && !in->rate60;
    s.kind = kind;
    s.prevKind = kind;
    s.havePrev = mode != 0 && in->eligible;
    memcpy(s.prevView, in->view, sizeof s.prevView);
    s.prevFov = in->fovDeg;
    return kind;
}

int NpBg30Kind(void) { return s.kind; }
int NpBg30Light(void) { return s.kind == NPBG30_LIGHT; }

void NpBg30Emit(void) {
    if (NpBg30Mode() == 0) return;
    Rec r;
    r.kind = static_cast<unsigned int>(s.kind);
    memcpy(r.view, s.prevView, sizeof r.view);
    NpFsDefer(EmitCall, &r, sizeof r);
}

void NpBg30Begin(void) {
    if (NpBg30Mode() == 0) return;
    s.kind = NPBG30_NONE;
    Rec r;
    r.kind = NPBG30_NONE;
    memset(r.view, 0, sizeof r.view);
    NpFsDefer(EmitCall, &r, sizeof r);
}

int NpBg30SendLight(void) { return s_sendLight; }

int NpBg30Phase(void) { return s.kind == NPBG30_FULL ? 1 : (s.kind == NPBG30_LIGHT ? 2 : 0); }

// The balanced pair. A light picture does the work for the pair (and says a full one may skip it); a full picture skips only what a light one prepared since its last skip.
int NpBg30CrowdSlot(void) {
    if (!(BalanceBits() & 1) || NpBg30Mode() != 2) return NPBG30_SLOT_NORMAL;
    if (s.kind == NPBG30_LIGHT) { s.crowdPrep = true; ++s.balCrowdDo; return NPBG30_SLOT_DO_PAIR; }
    if (s.kind == NPBG30_FULL && s.crowdPrep) {
        s.crowdPrep = false;
        ++s.balCrowdSkip;
        return NPBG30_SLOT_SKIP;
    }
    return NPBG30_SLOT_NORMAL;
}
int NpBg30StadiumSlot(void) {
    if (!(BalanceBits() & 2) || NpBg30Mode() != 2) return NPBG30_SLOT_NORMAL;
    if (s.kind == NPBG30_LIGHT) { s.stadPrep = true; ++s.balStadDo; return NPBG30_SLOT_DO_PAIR; }
    if (s.kind == NPBG30_FULL && s.stadPrep) { s.stadPrep = false; ++s.balStadSkip; return NPBG30_SLOT_SKIP; }
    return NPBG30_SLOT_NORMAL;
}

void NpBg30StadiumSet(int stadium) {
    LoadCuts();
    g_cutNow = nullptr;
    for (int i = 0; i < g_nCuts && stadium >= 0; ++i)
        if (g_cuts[i].stadium == stadium) g_cutNow = &g_cuts[i];
    if (stadium >= 0 && NpBg30Mode() != 0)
        fprintf(stderr, "[bg30] stadium %d: background cut row %s (table of %d rows)%s\n", stadium, g_cutNow ? "found" : "none", g_nCuts,
                g_cutNow ? (g_cutNow->level || g_cutNow->crowdLow || g_cutNow->ambientOff || g_cutNow->samples != 2 ? "" : " (all zero: no cut)") : "");
}
int NpBg30CutCrowdLow(void) { return NpBg30Mode() == 2 && g_cutNow && g_cutNow->crowdLow ? 1 : 0; }
int NpBg30CutAmbientOff(void) { return NpBg30Mode() == 2 && g_cutNow && g_cutNow->ambientOff ? 1 : 0; }
int NpBg30CutBg(int* level, int* samples) {
    if (NpBg30Mode() != 2 || !g_cutNow) return 0;
    if (level) *level = g_cutNow->level;
    if (samples) *samples = g_cutNow->samples;
    return 1;
}

void NpBg30NoteCost(double ms) {
    if (NpBg30Mode() != 2 || !(ms > 0.0) || ms > 200.0) return;
    if (s.kind == NPBG30_FULL) { s.emaFull = s.nFull ? 0.95 * s.emaFull + 0.05 * ms : ms; ++s.nFull; }
    else if (s.kind == NPBG30_LIGHT) { s.emaLight = s.nLight ? 0.95 * s.emaLight + 0.05 * ms : ms; ++s.nLight; }
}
int NpBg30HeldEligible(void) { return s.heldEligible ? 1 : 0; }
double NpBg30PairSavingMs(void) {
    if (s.nFull >= 30 && s.nLight >= 30) { const double d = s.emaFull - s.emaLight; return d > 0 ? 0.5 * d : 0.0; }
    static const double est = [] { const char* e = getenv("MSC_BG30_SAVE_MS"); return e && *e ? atof(e) : 1.25; }();   // (console estimate: 2.5 ms a light picture, w8/bg30.md)
    return est;
}
void NpBg30NoteCrowdSkip(void) { ++s.crowdSkips; }
void NpBg30NoteStadiumSkip(void) { ++s.stadiumSkips; }
void NpBg30Reset(void) {
    s = State{};
    s_sendLight = 0;
}

}  // extern "C"
