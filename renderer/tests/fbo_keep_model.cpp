// nxgl25 (vitaGL 0062, MSC_VITA_FBO_KEEP): life-cycle model test of the framebuffer object's colour attach / detach / delete. The texts under test are the real text of _glFramebufferTexture2D (shared.h)
// and glDeleteFramebuffers (framebuffers.c), cut out by run_fbo_keep_test.sh; everything around them is a model (textures with reference counts, a ledger of the GPU objects a scene makes and the garbage
// collector frees, a scene entry that mirrors gxm.c scene_reset). Random sequences over 3 objects and 6 textures of 3 sizes, with the switch on and off; under ASan/UBSan (a freed target used again, a leaked or doubly freed one).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
typedef int SceGxmTextureFormat;
struct GxmTex { int w, h, type, fmt; void *data; };
struct texture { GxmTex gxm_tex; int ref_counter; GLboolean dirty; SceGxmTextureFormat format; int id; };
struct ColorSurf { void *data; int w, h; };
struct Target { int w, h, msaa; int id; };
struct Depth { int w, h; int id; };
struct framebuffer { int width, height, stride; void *data; SceGxmTextureFormat format; texture *tex; Target *target; Depth *depthbuffer_ptr; Depth depthbuffer; int depthbuffer_state;
                     GLboolean is_float, active, nodepth; int msaa; ColorSurf colorbuffer; };
#define FB_MSAA(fb) ((fb)->msaa)
#define VGL_FB_DROP_RT2(fb) do { } while (0)   /* 0063: the second render target; this model has one (run_dual_rt_test.sh covers the pair) */
static int g_glerr, g_vglFailed;
struct framebuffer; static framebuffer *g_fbs;   // (names are 1-based indices here: a 32 bit name cannot hold a host pointer; the script rewrites the one cast)
#define FBFROM(id) (g_fbs + ((id) - 1))
static framebuffer *in_use_framebuffer, *active_write_fb, *active_read_fb; static GLboolean dirty_framebuffer;
GLboolean vgl_fb_detach_keep = GL_FALSE;
static GLboolean vgl_fg_on = GL_FALSE, vgl_fg_flushing = GL_FALSE;
static int vgl_fg_delete(int, int, const GLuint *, void *) { return 0; }
// ---- the ledger (what the GPU / collector would see)
static std::set<Target *> g_liveTargets; static std::set<Depth *> g_liveDepths; static std::set<texture *> g_freedTex;
static int g_targetsMade, g_targetsFreed, g_depthsMade, g_depthsFreed, g_texFreed;
static void mark_rt_as_dirty(Target *t) { assert(g_liveTargets.count(t)); g_liveTargets.erase(t); ++g_targetsFreed; delete t; }
static void mark_depth_stencil_as_dirty(Depth *d) { assert(g_liveDepths.count(d)); g_liveDepths.erase(d); ++g_depthsFreed; delete d; }
static void gpu_free_texture(texture *t) { assert(!g_freedTex.count(t)); g_freedTex.insert(t); ++g_texFreed; }
static SceGxmTextureFormat vglGetTexFormat(GxmTex *t) { return t->fmt; }
static void vglGetTexSizes(GxmTex *t, int *w, int *h) { *w = t->w; *h = t->h; }
static int sceGxmTextureGetType(GxmTex *t) { return t->type; }
static int tex_format_to_bytespp(SceGxmTextureFormat) { return 4; }
static void *vglGetTexData(GxmTex *t) { return t->data; }
static int get_color_from_texture(SceGxmTextureFormat f) { return f; }
static void sceGxmColorSurfaceInit(ColorSurf *c, int, int, int, int, int w, int h, int, void *data) { c->data = data; c->w = w; c->h = h; }
#include "under_test_attach.inc"
#include "under_test_delete.inc"

// ---- the model's scene entry: the part of gxm.c scene_reset that makes a framebuffer object's objects (a missing colour texture is a failed scene, as in 0062)
static int g_sceneFail;
static bool model_scene(framebuffer *fb) {
    if (!fb->tex) { ++g_sceneFail; return false; }
    if (fb->colorbuffer.data != fb->tex->gxm_tex.data) _glFramebufferTexture2D(fb, GL_COLOR_ATTACHMENT0, fb->tex, 1);
    if (!(fb->depthbuffer_state & DEPTHBUFFER_READY)) { fb->depthbuffer.w = fb->width; fb->depthbuffer.h = fb->height; Depth *d = new Depth{fb->width, fb->height, ++g_depthsMade}; g_liveDepths.insert(d); fb->depthbuffer_ptr = d; fb->depthbuffer_state |= DEPTHBUFFER_READY; }
    if (!fb->target) { Target *t = new Target{fb->width, fb->height, FB_MSAA(fb), ++g_targetsMade}; g_liveTargets.insert(t); fb->target = t; }
    return true;
}
// nxr6b: the part of scene_reset that decides between "start a scene" and "go on in the running one" (gxm.c: in_use_framebuffer != active_write_fb || needs_scene_reset || dirty_framebuffer). A draw into
// an object whose colour texture was detached must never go on in the running scene (its colour surface is the freed texture's): it must reach the scene start, which refuses it.
static GLboolean g_needsReset; static long g_drawsRefused, g_drawsOk;
static void model_draw(framebuffer *f, int keep, int *bad) {
    active_write_fb = f;
    if (in_use_framebuffer != active_write_fb || g_needsReset || dirty_framebuffer) {
        dirty_framebuffer = GL_FALSE; g_needsReset = GL_FALSE; in_use_framebuffer = active_write_fb;
        if (!model_scene(f)) { g_needsReset = GL_TRUE; ++g_drawsRefused; return; }
        ++g_drawsOk; return;
    }
    if (keep && !f->tex) { if (*bad < 10) std::fprintf(stderr, "FAIL: a draw went on in the running scene of an object with no colour texture (the freed texture's surface)\n"); ++*bad; }
    ++g_drawsOk;
}
static uint32_t rnd_state = 1; static uint32_t rnd() { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }
static int g_bad;
#define CHECK(c, ...) do { if (!(c)) { if (g_bad < 10) { std::fprintf(stderr, "FAIL line %d: ", __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } ++g_bad; } } while (0)

static texture slot0;
int main() {
    const int sz[6][2] = {{728, 512}, {768, 544}, {960, 544}, {728, 544}, {768, 512}, {728, 512}};
    long keptReuse = 0, madeNew = 0, steps = 0;
    for (int keep = 0; keep <= 1; ++keep)
    for (uint32_t seed = 1; seed <= 400; ++seed) {
        rnd_state = seed * 7919u; vgl_fb_detach_keep = keep;
        slot0 = texture{}; slot0.gxm_tex = GxmTex{1, 1, 0, 0, nullptr};
        texture tex[6]; for (int i = 0; i < 6; ++i) { tex[i] = texture{}; tex[i].gxm_tex = GxmTex{sz[i][0], sz[i][1], 0, 0, new uint8_t[16]}; tex[i].id = i + 1; }
        framebuffer fbs[3]; g_fbs = fbs; for (auto &f : fbs) { f = framebuffer{}; f.active = true; f.depthbuffer_state = DEPTHBUFFER_MISSING; }
        in_use_framebuffer = nullptr; active_write_fb = active_read_fb = nullptr; dirty_framebuffer = GL_FALSE; g_needsReset = GL_FALSE;
        std::set<texture *> deleted; g_freedTex.clear();
        for (int step = 0; step < 300; ++step, ++steps) {
            framebuffer &f = fbs[rnd() % 3]; const int op = rnd() % 12; texture &t = tex[rnd() % 6];
            if (!f.active) { if (op < 2) { f = framebuffer{}; f.active = true; } continue; }
            const int w0 = f.width, h0 = f.height; Target *tg0 = f.target; Depth *d0 = f.depthbuffer_ptr; texture *t0 = f.tex;
            if (op < 3) {   // attach (not a deleted texture)
                if (deleted.count(&t)) continue;
                _glFramebufferTexture2D(&f, GL_COLOR_ATTACHMENT0, &t, t.id);
                CHECK(f.tex == &t && f.width == t.gxm_tex.w && f.height == t.gxm_tex.h && f.colorbuffer.data == t.gxm_tex.data, "attach state");
                const bool same = t0 == nullptr ? (w0 == t.gxm_tex.w && h0 == t.gxm_tex.h) : (w0 == t.gxm_tex.w && h0 == t.gxm_tex.h);
                if (w0 && same && !(keep == 0 && !t0)) { CHECK(f.target == tg0 && f.depthbuffer_ptr == d0, "same size attach lost its objects (keep %d, had tex %d)", keep, t0 != nullptr); if (keep && !t0 && tg0) ++keptReuse; }
                if (w0 && !same) { CHECK(f.target == nullptr && f.depthbuffer_ptr == nullptr, "a different size kept its objects"); }
            } else if (op < 5) {   // detach
                _glFramebufferTexture2D(&f, GL_COLOR_ATTACHMENT0, &slot0, 0);
                CHECK(f.tex == nullptr, "detach left a texture");
                if (keep) CHECK(f.width == w0 && f.height == h0 && f.target == tg0 && f.depthbuffer_ptr == d0, "kept detach changed the object");
                else CHECK(f.target == nullptr, "old detach kept the target");
            } else if (op < 7) {   // a scene
                const bool ok = model_scene(&f);
                CHECK(ok == (f.tex != nullptr), "scene result");
                if (ok) { CHECK(f.target && f.target->w == f.width && f.target->h == f.height && f.target->msaa == FB_MSAA(&f), "target does not fit the object"); CHECK(f.depthbuffer_ptr && f.depthbuffer_ptr->w == f.width && f.depthbuffer_ptr->h == f.height, "depth does not fit"); if (tg0 && f.target == tg0) ++madeNew; }
            } else if (op < 8) {   // delete the texture name (what the renderer does after DetachFromPool): freed now when no object holds it
                if (deleted.count(&t)) continue;
                deleted.insert(&t); t.dirty = true; if (t.ref_counter == 0) gpu_free_texture(&t);
            } else if (op >= 10) {   // nxr6b: a draw into the object (a scene entry, or the scene already running)
                model_draw(&f, keep, &g_bad);
            } else if (op < 9) {   // delete the object
                GLuint id = static_cast<GLuint>(&f - fbs) + 1;
                glDeleteFramebuffers(1, &id);
                if (in_use_framebuffer == &f) in_use_framebuffer = nullptr; if (active_write_fb == &f) active_write_fb = nullptr;
                CHECK(!f.active && f.target == nullptr && f.depthbuffer_ptr == nullptr && f.tex == nullptr, "a deleted object still points at its objects");
            }
            // invariants after every step
            for (int k = 0; k < 6; ++k) { int holders = 0; for (auto &g : fbs) if (g.active && g.tex == &tex[k]) ++holders; CHECK(tex[k].ref_counter == holders, "ref_counter %d vs %d holders (tex %d)", tex[k].ref_counter, holders, k);
                CHECK(!(deleted.count(&tex[k]) && tex[k].ref_counter == 0) || g_freedTex.count(&tex[k]), "a deleted unheld texture not freed"); }
            std::set<Target *> held; std::set<Depth *> heldD;
            for (auto &g : fbs) if (g.active) { if (g.target) { held.insert(g.target); CHECK(g.target->w == g.width && g.target->h == g.height, "held target size"); } if (g.depthbuffer_ptr) heldD.insert(g.depthbuffer_ptr); }
            CHECK(held == g_liveTargets && heldD == g_liveDepths, "ledger: live objects are not exactly the held ones (%zu/%zu targets, %zu/%zu depths)", held.size(), g_liveTargets.size(), heldD.size(), g_liveDepths.size());
        }
        for (auto &g : fbs) if (g.active) { GLuint id = static_cast<GLuint>(&g - fbs) + 1; glDeleteFramebuffers(1, &id); }
        CHECK(g_liveTargets.empty() && g_liveDepths.empty(), "objects left after deleting every framebuffer");
        for (auto &t : tex) { CHECK(t.ref_counter == 0, "reference left"); delete[] static_cast<uint8_t *>(t.gxm_tex.data); }
    }
    std::printf("%ld steps; targets made %d freed %d; depths made %d freed %d; scenes refused %d; draws refused %ld ok %ld; same-size re-attaches that kept their objects %ld; failed checks %d\n", steps, g_targetsMade, g_targetsFreed, g_depthsMade, g_depthsFreed, g_sceneFail, g_drawsRefused, g_drawsOk, keptReuse, g_bad);
    CHECK(g_targetsMade == g_targetsFreed && g_depthsMade == g_depthsFreed, "a made object was not freed exactly once");
    if (g_bad) return 1;
    std::puts("OK");
    return 0;
}
