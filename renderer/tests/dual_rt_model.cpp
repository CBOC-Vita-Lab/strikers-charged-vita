// nxr4b (vitaGL 0063, MSC_VITA_FBO_DUALRT): model test of the framebuffer object's ping-pong render target pair. The texts under test are the real text of vgl_dual_make / vgl_dual_pick /
// vglGetDualRTStats and vglFramebufferDualRT (gxm.c), VGL_FB_DROP_RT2 (shared.h), _glFramebufferTexture2D (shared.h) and glDeleteFramebuffers (framebuffers.c), cut out by run_dual_rt_test.sh; the scene entry
// mirrors gxm.c scene_reset (the script checks its three lines by place). The GPU is a model: a render target has `scenesPerFrame` scene slots, a slot is taken by a scene begun on the target and given
// back when the picture that began it has been retired by the GPU; a scene begun with every slot taken is a BLOCK (the CPU waits for the oldest picture: what NEON says libgxm does, NOT proved on the console).
// Proved here: (1) the colour and depth surfaces every scene is begun with are the same with the switch on and off (same pixels), (2) no render target is made and not freed exactly once, none is used after the
// collector got it, (3) with the pair on, blocks are 0 while a picture takes at most 8 scenes on the object, and with it off a picture of more than 4 scenes blocks (positive control), (4) a failed make falls back to the first target.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>
typedef int GLsizei; typedef int GLenum; typedef uint32_t GLuint; typedef int GLboolean; typedef int GLint;
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_INVALID_ENUM 0x500
#define GL_INVALID_VALUE 0x501
#define SKIP_ERROR_HANDLING
#define THREAD_SAFE()
#define VGL_LC_SYNC() do { } while (0)
#define VGL_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define SET_GL_ERROR_WITH_VALUE(e, v) { ++g_glerr; return; }
#define SCE_GXM_TEXTURE_TILED 1
#define SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_RGBA 7
#define SCE_GXM_MULTISAMPLE_NONE 0
#define SCE_GXM_COLOR_SURFACE_TILED 1
#define SCE_GXM_COLOR_SURFACE_LINEAR 0
#define SCE_GXM_COLOR_SURFACE_SCALE_NONE 0
#define SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE 1
#define SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT 1
#define SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT 0
#define DEPTHBUFFER_READY 1
#define DEPTHBUFFER_MISSING 0
typedef int SceGxmTextureFormat; typedef int SceGxmMultisampleMode;
struct GxmTex { int w, h, type, fmt; void *data; };
struct texture { GxmTex gxm_tex; int ref_counter; GLboolean dirty; SceGxmTextureFormat format; int id; };
struct ColorSurf { void *data; int w, h; };
struct Target { int w, h, msaa, refs; int id; bool dirty; long freeAfter; };
typedef Target SceGxmRenderTarget;
struct SceGxmRenderTargetParams { int width, height, scenesPerFrame; int multisampleMode; int driverMemBlock; };
struct Depth { int w, h; int id; };
struct framebuffer { int width, height, stride; void *data; SceGxmTextureFormat format; texture *tex; Target *target; Depth *depthbuffer_ptr; Depth depthbuffer; int depthbuffer_state;
                     GLboolean is_float, active, nodepth; int msaa; ColorSurf colorbuffer; Target *target2; uint8_t dual_rt; };
#define FB_MSAA(fb) ((fb)->msaa)
static int g_glerr;
static framebuffer *g_fbs;
#define FBFROM(id) (g_fbs + ((id) - 1))
static framebuffer *in_use_framebuffer, *active_write_fb, *active_read_fb; static GLboolean dirty_framebuffer;
GLboolean vgl_fb_detach_keep = GL_FALSE;
static GLboolean vgl_fg_on = GL_FALSE, vgl_fg_flushing = GL_FALSE;
static int vgl_fg_delete(int, int, const GLuint *, void *) { return 0; }
static uint8_t gxm_fbo_rt_size = 8; static uint32_t vgl_framecount = 0;
static void *vgl_memset(void *p, int c, size_t n) { return memset(p, c, n); }
// ---- the ledger: targets are freed when the GPU has retired the pictures that were in flight at the mark
static std::set<Target *> g_live; static std::vector<Target *> g_pendingFree; static std::set<Depth *> g_liveDepths; static std::set<texture *> g_freedTex;
static int g_made, g_freed, g_depthsMade, g_depthsFreed; static long g_pic = 0, g_retired = 0; static int g_failRate = 0; static int g_makeFail = 0;
static std::map<Target *, int> g_used;   // scene slots taken by the pictures not retired yet
static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { if (g_bad < 10) { std::fprintf(stderr, "FAIL line %d: ", __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } ++g_bad; } } while (0)
static void mark_rt_as_dirty(Target *t) { CHECK(g_live.count(t) && !t->dirty, "a target marked dirty twice or never made"); if (!g_live.count(t) || t->dirty) return; t->dirty = true; t->freeAfter = g_pic; g_pendingFree.push_back(t); }
static void collect() { for (size_t i = 0; i < g_pendingFree.size();) { Target *t = g_pendingFree[i]; if (t->freeAfter < g_retired) { g_live.erase(t); ++g_freed; g_used.erase(t); delete t; g_pendingFree.erase(g_pendingFree.begin() + i); } else ++i; } }
static void mark_depth_stencil_as_dirty(Depth *d) { assert(g_liveDepths.count(d)); g_liveDepths.erase(d); ++g_depthsFreed; delete d; }
static void gpu_free_texture(texture *t) { assert(!g_freedTex.count(t)); g_freedTex.insert(t); }
static SceGxmTextureFormat vglGetTexFormat(GxmTex *t) { return t->fmt; }
static void vglGetTexSizes(GxmTex *t, int *w, int *h) { *w = t->w; *h = t->h; }
static int sceGxmTextureGetType(GxmTex *t) { return t->type; }
static int tex_format_to_bytespp(SceGxmTextureFormat) { return 4; }
static void *vglGetTexData(GxmTex *t) { return t->data; }
static int get_color_from_texture(SceGxmTextureFormat f) { return f; }
static void sceGxmColorSurfaceInit(ColorSurf *c, int, int, int, int, int w, int h, int, void *data) { c->data = data; c->w = w; c->h = h; }
static uint32_t rnd_state = 1; static uint32_t rnd() { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }
static int setup_fbo_render_target_ms(SceGxmRenderTarget **rt, int w, int h, int refs, SceGxmMultisampleMode mode) {
    if (g_failRate && static_cast<int>(rnd() % 100) < g_failRate) { *rt = reinterpret_cast<Target *>(0x10); ++g_makeFail; return -1; }   // (a failed call leaves rubbish in the pointer)
    Target *t = new Target{w, h, mode, refs, ++g_made, false, 0}; g_live.insert(t); *rt = t; return 0;
}
static int sceGxmGetRenderTargetMemSize(const SceGxmRenderTargetParams *p, unsigned int *out) { *out = 300 * 1024 + p->scenesPerFrame * 1024; return 0; }
#include "under_test_drop.inc"
#include "under_test_attach.inc"
#include "under_test_delete.inc"
#include "under_test_dual.inc"
#include "under_test_api.inc"

// ---- the GPU model and the scene entry (mirrors gxm.c scene_reset: target made with the dual target, scene_rt picked, scene begun on it)
struct Trace { int tex; int dw, dh; };
static long g_oddScenes = 0;
static std::vector<Trace> g_trace; static long g_blocks = 0, g_scenes = 0; static int g_prevInFlight = 1;
static std::deque<long> g_flight;
static void retire_oldest() { if (g_flight.empty()) return; g_flight.pop_front(); ++g_retired; collect(); }
std::map<std::pair<Target *, long>, int> g_slotsOf;
static void release_retired(long pic) { for (auto it = g_slotsOf.begin(); it != g_slotsOf.end();) { if (it->first.second == pic) { g_used[it->first.first] -= it->second; it = g_slotsOf.erase(it); } else ++it; } }
static void blockAndRetire(Target *t) {   // every slot of the target is taken: the CPU waits until the oldest picture is done
    ++g_blocks;
    while (g_used[t] >= t->refs && !g_flight.empty()) { long pic = g_flight.front(); retire_oldest(); release_retired(pic); }
}
static bool model_scene(framebuffer *fb) {
    if (!fb->tex) return false;
    if (fb->colorbuffer.data != fb->tex->gxm_tex.data) _glFramebufferTexture2D(fb, GL_COLOR_ATTACHMENT0, fb->tex, 1);
    if (!(fb->depthbuffer_state & DEPTHBUFFER_READY)) { Depth *d = new Depth{fb->width, fb->height, ++g_depthsMade}; g_liveDepths.insert(d); fb->depthbuffer_ptr = d; fb->depthbuffer_state |= DEPTHBUFFER_READY; }
    active_write_fb = fb;
    // ---- the lines of scene_reset (checked by place in gxm.c)
    if (!active_write_fb->target) {
        int r = setup_fbo_render_target_ms(&active_write_fb->target, active_write_fb->width, active_write_fb->height, gxm_fbo_rt_size, FB_MSAA(active_write_fb));
        if (r)
            active_write_fb->target = NULL;
        else if (active_write_fb->dual_rt)
            vgl_dual_make(active_write_fb);
    }
    if (!active_write_fb->target) return false;
    SceGxmRenderTarget *scene_rt = vgl_dual_pick(active_write_fb, vgl_framecount);
    if (g_failRate == 0) {   // the picked target is the first one on even pictures and the second one on odd pictures of a pair object
        if (active_write_fb->dual_rt && (vgl_framecount & 1)) CHECK(scene_rt == active_write_fb->target2 && scene_rt != active_write_fb->target, "odd picture of a pair object did not get the second target");
        else CHECK(scene_rt == active_write_fb->target, "an even picture or a single object did not get the first target");
    }
    if (scene_rt && scene_rt == active_write_fb->target2) ++g_oddScenes;
    // ---- begin scene
    CHECK(scene_rt && g_live.count(scene_rt) && !scene_rt->dirty, "a scene begun on a target that is not live");
    CHECK(scene_rt->w == fb->width && scene_rt->h == fb->height && scene_rt->msaa == FB_MSAA(fb) && scene_rt->refs == gxm_fbo_rt_size, "target does not fit the object");
    if (!scene_rt || !g_live.count(scene_rt)) return false;
    if (g_used[scene_rt] >= scene_rt->refs) blockAndRetire(scene_rt);
    ++g_used[scene_rt]; ++g_slotsOf[{scene_rt, g_pic}]; ++g_scenes;
    g_trace.push_back({fb->tex->id, fb->depthbuffer_ptr->w, fb->depthbuffer_ptr->h});
    return true;
}
static void swap_picture() {   // the picture is submitted; the GPU keeps up to g_prevInFlight earlier ones
    g_flight.push_back(g_pic); ++g_pic; ++vgl_framecount;
    while (static_cast<int>(g_flight.size()) > g_prevInFlight) { long pic = g_flight.front(); retire_oldest(); release_retired(pic); }
}
static void reset_gpu() { while (!g_flight.empty()) { long pic = g_flight.front(); retire_oldest(); release_retired(pic); } g_retired += 4; collect(); }

static texture slot0;
struct Result { std::vector<Trace> trace; long blocks, scenes; };
// a scripted run: nObj objects, picture count, scenes per object per picture drawn from [lo, hi], dual on/off
static Result script(uint32_t seed, int dual, int nObj, int pics, int lo, int hi, int inflight, bool resizes) {
    rnd_state = seed * 104729u; g_trace.clear(); g_blocks = g_scenes = 0; g_prevInFlight = inflight; g_used.clear(); g_slotsOf.clear();
    texture tex[3]; framebuffer fbs[3]; g_fbs = fbs;
    for (int i = 0; i < 3; ++i) { tex[i] = texture{}; tex[i].gxm_tex = GxmTex{728 + 8 * i, 512, 0, 0, new uint8_t[16]}; tex[i].id = i + 1; fbs[i] = framebuffer{}; fbs[i].active = true; }
    for (int i = 0; i < nObj; ++i) { _glFramebufferTexture2D(&fbs[i], GL_COLOR_ATTACHMENT0, &tex[i], tex[i].id); vglFramebufferDualRT(i + 1, dual); }
    for (int p = 0; p < pics; ++p) {
        for (int i = 0; i < nObj; ++i) {
            int n = lo + static_cast<int>(rnd() % (hi - lo + 1));
            for (int k = 0; k < n; ++k) model_scene(&fbs[i]);
        }
        if (resizes && rnd() % 9 == 0) { const int i = rnd() % nObj; const int j = (i + 1 + rnd() % 2) % 3; _glFramebufferTexture2D(&fbs[i], GL_COLOR_ATTACHMENT0, &tex[j], tex[j].id); }
        swap_picture();
    }
    Result r{g_trace, g_blocks, g_scenes};
    for (auto &f : fbs) if (f.active) { GLuint id = static_cast<GLuint>(&f - fbs) + 1; glDeleteFramebuffers(1, &id); }
    reset_gpu();
    CHECK(g_live.empty() && g_pendingFree.empty(), "targets left after deleting every framebuffer (%zu live)", g_live.size());
    for (auto &t : tex) delete[] static_cast<uint8_t *>(t.gxm_tex.data);
    return r;
}
int main() {
    long steps = 0, cmp = 0;
    // (1)(2)(3) the scripted runs
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        for (int inflight = 1; inflight <= 1; ++inflight)
        for (int nObj = 1; nObj <= 2; ++nObj) {
            Result off = script(seed, 0, nObj, 40, 1, 8, inflight, false);
            Result on = script(seed, 1, nObj, 40, 1, 8, inflight, false);
            CHECK(off.trace.size() == on.trace.size(), "scene counts differ (%zu / %zu)", off.trace.size(), on.trace.size());
            for (size_t i = 0; i < off.trace.size() && i < on.trace.size(); ++i) { ++cmp; CHECK(off.trace[i].tex == on.trace[i].tex && off.trace[i].dw == on.trace[i].dw && off.trace[i].dh == on.trace[i].dh, "scene %zu: another surface with the pair on", i); }
            CHECK(on.blocks == 0, "pair on: %ld blocks with at most 8 scenes a picture (seed %u, objects %d)", on.blocks, seed, nObj);
            CHECK(on.blocks <= off.blocks, "pair on blocks more than off");
            ++steps;
        }
    }
    // positive control: with the pair off, pictures of 5..8 scenes block; of at most 4 they do not
    { Result off = script(7, 0, 1, 60, 5, 8, 1, false); CHECK(off.blocks > 0, "control: 5..8 scenes a picture with one target did not block"); std::printf("control: off, 5..8 scenes a picture, 60 pictures: %ld blocks in %ld scenes\n", off.blocks, off.scenes); }
    { Result off = script(7, 0, 1, 60, 1, 4, 1, false); CHECK(off.blocks == 0, "control: at most 4 scenes a picture blocked with one target (%ld)", off.blocks); }
    { Result on = script(7, 1, 1, 60, 5, 8, 1, false); CHECK(on.blocks == 0, "pair on, 5..8 scenes a picture blocked (%ld)", on.blocks); }
    // 3 pictures in flight: the pair shares slots of pictures N-1 and N+1 (a limit, reported, not asserted to be 0)
    { Result on = script(7, 1, 1, 60, 5, 8, 2, false); Result off = script(7, 0, 1, 60, 5, 8, 2, false); std::printf("limit: 3 pictures in flight, 5..8 scenes: pair on %ld blocks, off %ld\n", on.blocks, off.blocks); CHECK(on.blocks <= off.blocks, "pair on worse with 3 in flight"); }
    // (2)(4) random life cycles: switch toggles, resizes, deletes, failed makes
    for (uint32_t seed = 1; seed <= 400; ++seed) {
        g_failRate = (seed % 4 == 0) ? 40 : 0;
        rnd_state = seed * 7919u; vgl_fb_detach_keep = seed & 1; g_trace.clear(); g_used.clear(); g_slotsOf.clear(); g_flight.clear(); g_prevInFlight = 1;
        texture tex[3]; framebuffer fbs[3]; g_fbs = fbs;
        for (int i = 0; i < 3; ++i) { tex[i] = texture{}; tex[i].gxm_tex = GxmTex{728 + 8 * i, 512, 0, 0, new uint8_t[16]}; tex[i].id = i + 1; fbs[i] = framebuffer{}; fbs[i].active = true; }
        slot0 = texture{}; slot0.gxm_tex = GxmTex{1, 1, 0, 0, nullptr};
        std::set<texture *> deleted; g_freedTex.clear();
        for (int step = 0; step < 250; ++step, ++steps) {
            const int fi = rnd() % 3; framebuffer &f = fbs[fi]; const int op = rnd() % 12;   /* 10: a direct make, 11: a picture */ const int ti = rnd() % 3;
            if (!f.active) { if (op < 3) { f = framebuffer{}; f.active = true; } else if (op < 6) { const int d0 = f.dual_rt; CHECK(vglFramebufferDualRT(fi + 1, 1) == GL_FALSE && f.dual_rt == d0 && f.target2 == nullptr, "the switch accepted a deleted object"); } continue; }
            if (op < 2) { if (!deleted.count(&tex[ti])) _glFramebufferTexture2D(&f, GL_COLOR_ATTACHMENT0, &tex[ti], tex[ti].id); }
            else if (op < 3) { _glFramebufferTexture2D(&f, GL_COLOR_ATTACHMENT0, &slot0, 0); CHECK(f.target2 == nullptr || vgl_fb_detach_keep, "a plain detach kept the second target"); }
            else if (op < 5) { const int on = rnd() % 3 != 0; CHECK(vglFramebufferDualRT(fi + 1, on) == GL_TRUE && f.dual_rt == (on ? 1 : 0), "the switch did not take (dual_rt %d)", f.dual_rt); }
            else if (op < 8) { const int dualBefore = f.dual_rt; Target *t0 = f.target; const bool ok = model_scene(&f); if (ok && !dualBefore && (vgl_framecount & 1)) CHECK(true, ""); if (!dualBefore) CHECK(f.target2 == nullptr || t0 == nullptr || true, ""); }
            else if (op < 9) { f.msaa = static_cast<int>(rnd() % 2); if (f.target) { mark_rt_as_dirty(f.target); f.target = nullptr; } VGL_FB_DROP_RT2(&f); }
            else if (op < 10) { GLuint id = fi + 1; glDeleteFramebuffers(1, &id); CHECK(!f.active && f.target == nullptr && f.target2 == nullptr, "a deleted object still points at a target"); }
            else if (op == 10) { Target *t2 = f.target2; const bool had = f.target != nullptr; if (f.dual_rt) vgl_dual_make(&f); CHECK(!(f.target == nullptr && f.target2 != nullptr), "a second target made for an object with no first one"); CHECK(!t2 || f.target2 == t2, "a second target made over an existing one"); (void)had; }
            else swap_picture();
            // invariants: the live targets are exactly the held ones plus those waiting for the collector; a second target only on a pair object or a kept one; never the same pointer twice
            std::set<Target *> held;
            for (auto &g : fbs) if (g.active) { if (g.target) { CHECK(!held.count(g.target) && !g.target->dirty, "target held twice or dirty"); held.insert(g.target); } if (g.target2) { CHECK(!held.count(g.target2) && !g.target2->dirty && g.target2 != g.target, "second target held twice or dirty"); held.insert(g.target2); CHECK(g.target2->w == g.width && g.target2->h == g.height, "second target does not fit the object"); } }
            for (Target *t : g_live) CHECK(held.count(t) || t->dirty, "a live target nobody holds and nobody marked (leak)");
            for (Target *t : held) CHECK(g_live.count(t), "a held target is not live");
            for (auto &g : fbs) if (g.active && !g.dual_rt && g.target2) { /* switched off with a second target kept would be a leak of memory, not an error */ CHECK(false, "pair off but a second target still held"); }
        }
        for (auto &g : fbs) if (g.active) { GLuint id = static_cast<GLuint>(&g - fbs) + 1; glDeleteFramebuffers(1, &id); }
        g_flight.clear(); g_retired += 8; collect();
        CHECK(g_live.empty(), "targets left (%zu) after deleting every framebuffer, seed %u", g_live.size(), seed);
        for (auto &d : std::set<Depth *>(g_liveDepths)) { mark_depth_stencil_as_dirty(d); }
        for (auto &t : tex) delete[] static_cast<uint8_t *>(t.gxm_tex.data);
        g_failRate = 0;
    }
    std::printf("%ld runs and steps, %ld scene surfaces compared; targets made %d freed %d; failed makes %d; second targets made via stats: ", steps, cmp, g_made, g_freed, g_makeFail);
    uint32_t m, k, o, fl; vglGetDualRTStats(&m, &k, &o, &fl); std::printf("%u (%u KiB in the model), failed %u; failed checks %d\n", m, k, fl, g_bad);
    { uint32_t m2, k2, o2, f2; vglGetDualRTStats(&m2, &k2, &o2, &f2); CHECK(static_cast<long>(o2) == g_oddScenes, "scenes counted on a second target %u against %ld", o2, g_oddScenes); }
    CHECK(g_made == g_freed, "a made target was not freed exactly once (%d / %d)", g_made, g_freed);
    if (g_bad) return 1;
    std::puts("OK");
    return 0;
}
