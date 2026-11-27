// 1.2 vitaGL scene entry (dependencies/vitaGL/source/gxm.c: init_depth_stencil_buffer, scene_reset, scene_reset_failed).
// The driver's own functions, extracted from gxm.c by run_vitagl_scene_entry_test.sh (not copies), over a model of sceGxm in which allocations,
// render-target creation and sceGxmBeginScene can be made to fail. The checks: a failed entry returns false and leaves no scene open (no second
// end of a scene that is over, no begin without a target), is not retried by the next draw of the same frame (an allocation failure has already cost the
// allocator's recovery), is retried in the next frame, does not stop another framebuffer's scene, and with the 1.1 switch (vglSetRobustScenes(0)) the
// old behaviour is kept.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char GLboolean;
typedef int SceUID;
typedef unsigned SceSize;
typedef unsigned GLenum;
typedef int SceGxmMultisampleMode;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define NULL_ ((void *)0)
#define VGL_ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))
#define SCE_GXM_TILE_SIZEX 32
#define SCE_GXM_TILE_SIZEY 32
#define SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT 4096
#define SCE_GXM_MULTISAMPLE_2X 1
#define SCE_GXM_MULTISAMPLE_4X 2
#define SCE_GXM_MULTISAMPLE_NONE 0
#define SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M 0
#define SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8 1
#define SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR 0
#define SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED 1
#define SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED 1
#define SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED 2   // (0038)
#define SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED 2
#define SCE_GXM_REGION_CLIP_OUTSIDE 1
#define VGL_RT_CPU_TIMERS 4
#define VGL_RT_GPU_TIMERS 8
#define VGL_RT_NOLOAD 1
#define STORE_DEPTH_STENCIL
#define SKIP_SPLASHSCREEN
#define LOG_ERRORS
enum { DEPTHBUFFER_MISSING, DEPTHBUFFER_WANTS_STENCIL, DEPTHBUFFER_READY };

typedef struct { int unused; } SceGxmRenderTarget;
typedef struct { int unused; } SceGxmContext;
typedef struct { void *data; } SceGxmColorSurface;
typedef struct { void *depthData; void *stencilData; int mode; } SceGxmDepthStencilSurface;
typedef struct { void *data; uint32_t fg_rt_frame; } texture;   // (fg_rt_frame: 0034)
typedef struct { int unused; } SceGxmSyncObject;
typedef struct { uint32_t index; uint32_t vsync; } SceSharedFbInfo;
typedef struct {
    GLboolean active, is_float;
    uint8_t depthbuffer_state;
    SceGxmRenderTarget *target, *target2;
    unsigned char dual_rt;   /* 0063 */
    SceGxmColorSurface colorbuffer;
    SceGxmDepthStencilSurface depthbuffer;
    SceGxmDepthStencilSurface *depthbuffer_ptr;
    int width, height;
    SceGxmMultisampleMode msaa;
    texture *tex;
    GLboolean nodepth;                                  // 0038 vglFramebufferNoDepth
    uint16_t depth_share_w, depth_share_h, depth_row_w; // 0044 vglFramebufferDepthShare (0: not shared; the model never shares)
} framebuffer;
#define FB_MSAA(fb) ((fb)->msaa)
typedef struct { int x, y, w, h; } rect_t;
typedef struct { int x, y, w, h; } viewport_t;

// ---- the state gxm.c and its neighbours keep
framebuffer *in_use_framebuffer, *old_framebuffer, *active_write_fb;
GLboolean dirty_framebuffer, dirty_query, dirty_scissor_state, is_fbo_float, is_rendering_display, skip_scissor_invalidation, scissor_test_state;
GLboolean vgl_rt_hint_valid, vgl_rt_clear_full, vgl_rt_skip_invalidation, is_splashscreen_active;
static GLboolean needs_end_scene = GL_FALSE, needs_scene_reset = GL_TRUE, scene_not_flushed = GL_FALSE;
GLboolean vgl_robust_scenes = GL_TRUE;
static GLboolean scene_fail_valid = GL_FALSE;
static framebuffer *scene_fail_fb = NULL;
static uint32_t scene_fail_frame = 0, vgl_scene_failures = 0;
uint32_t vgl_framecount, vgl_rt_flags;
int msaa_mode, system_app_mode, vsync_interval, gxm_back_buffer_index, gxm_fbo_rt_size = 2, DISPLAY_WIDTH = 64, DISPLAY_HEIGHT = 64;
SceGxmMultisampleMode cur_msaa;
SceGxmContext *gxm_context = (SceGxmContext *)1;
SceGxmRenderTarget *gxm_render_target = (SceGxmRenderTarget *)1;
SceGxmSyncObject *gxm_sync_objects[3];
SceGxmColorSurface gxm_color_surfaces[3];
SceGxmDepthStencilSurface gxm_depth_stencil_surface;
SceUID shared_fb;
SceSharedFbInfo shared_fb_info;
SceUID splash_mutex[2];
float *legacy_pool, *legacy_pool_ptr, *legacy_pool_end;
size_t legacy_pool_size;
rect_t region;
viewport_t gl_viewport;
float x_port, x_scale, y_port, y_scale, z_port, z_scale;
struct { int dummy; } query_fence_unused;

// ---- the model of the platform: counters and fault switches
static unsigned begins, ends, allocs, viewports, depth_inits, target_attempts, queued_clears;
static size_t last_alloc_size;
static SceGxmMultisampleMode target_mode;
static int fail_alloc, fail_stencil_only, fail_begin, fail_target;
static uint8_t arena[1 << 17];
static unsigned arena_used;
static SceGxmRenderTarget dummy_target;
static unsigned long vglog_count;
#define vgl_log(...) (++vglog_count)
#define vgl_memset memset
#define vglDepthStencilSurfaceInit(s, fmt, type, w, d, st) do { (s)->depthData = (d); (s)->stencilData = (st); (void)(fmt); (void)(type); (void)(w); ++depth_inits; } while (0)
static void *gpu_alloc_mapped_aligned_for_gpu(size_t align, size_t size) {
    ++allocs;
    last_alloc_size = size;
    if (fail_alloc) return NULL;
    if (fail_stencil_only && size < 4 * 32 * 32 * 2) return NULL;   // (the stencil surface is the smaller one)
    if (arena_used + size > sizeof arena) arena_used = 0;
    void *p = arena + arena_used;
    arena_used += (unsigned)VGL_ALIGN(size, 64);
    return p;
}
static void *gpu_alloc_mapped_temp(size_t size) { return arena; }
static unsigned dirty_marks;
static void mark_as_dirty(void *p) { ++dirty_marks; }
static void sceGxmDepthStencilSurfaceSetForceLoadMode(SceGxmDepthStencilSurface *s, int m) {}
static void sceGxmDepthStencilSurfaceSetForceStoreMode(SceGxmDepthStencilSurface *s, int m) {}
static int sceGxmBeginScene(SceGxmContext *c, int flags, SceGxmRenderTarget *rt, void *a, void *b, SceGxmSyncObject *sync, SceGxmColorSurface *col, SceGxmDepthStencilSurface *ds) {
    if (fail_begin) return -1;
    assert(rt != NULL && "a scene must not begin without a render target");
    assert(ds == NULL || ds->depthData != NULL);
    ++begins;
    return 0;
}
static void sceGxmEndScene(SceGxmContext *c, void *v, void *n) { ++ends; }
static void scene_end_hook(void) { ++ends; }
static void *sceGxmColorSurfaceGetData(SceGxmColorSurface *s) { return s->data; }
static void _glFramebufferTexture2D(framebuffer *fb, GLenum a, texture *t, int l) { fb->colorbuffer.data = t->data; }
static int setup_fbo_render_target_ms(SceGxmRenderTarget **rt, int w, int h, int refs, SceGxmMultisampleMode mode) {
    ++target_attempts;
    target_mode = mode;
    if (fail_target) return -1;
    *rt = &dummy_target;
    return 0;
}
static const char *get_gxm_error_literal(int r) { return "error"; }
static void sceDisplayWaitVblankStartMulti(int v) {}
static void sceSharedFbBegin(SceUID f, SceSharedFbInfo *i) {}
static void sceKernelSignalSema(SceUID s, int n) {}
static void sceKernelWaitSema(SceUID s, int n, void *t) {}
static void sceKernelDeleteSema(SceUID s) {}
static void clear_splashscreen(void) {}
static void glViewport(int x, int y, int w, int h) { ++viewports; }
static void vglSetViewport(SceGxmContext *c, float a, float b, float d, float e, float f, float g) { ++viewports; }
static void change_cull_mode(void) {}
static void update_scissor_test(void) {}
static void sceGxmSetRegionClip(SceGxmContext *c, int m, int a, int b, int d, int e) {}
static void vgl_dual_make(framebuffer *f) { (void)f; }   /* 0063: the render target pair has its own test (run_dual_rt_test.sh) */
static SceGxmRenderTarget *vgl_dual_pick(framebuffer *f, uint32_t frame) { (void)frame; return f->target; }
static void scn_begin(const void *target, int display) { (void)target; (void)display; }   // (the scene timers' note: no effect on the model)
static GLboolean vgl_depth_share_get(int w, int h, int msaa, SceGxmDepthStencilSurface *s, framebuffer *fb) { return GL_FALSE; }   // 0044: never granted
static void vgl_depth_share_scene(void *d, framebuffer *fb) {}
static GLboolean vgl_fg_on = GL_FALSE;
static uint32_t vgl_rt_scene_begin(SceGxmDepthStencilSurface *ds, uint32_t w, uint32_t h) { return 0; }
static void vgl_mk_scene_begin(SceGxmDepthStencilSurface *ds) { (void)ds; } // (1.3 MASK_KEEP, patch 0026: run_vitagl_maskkeep_test.sh tests it)
static void vgl_mb_scene_begin(SceGxmDepthStencilSurface *ds, uint32_t rt_applied) { (void)ds; (void)rt_applied; } // (1.3 MASK_BG, patch 0029: run_vitagl_maskkeep_test.sh tests it)
static void vgl_mk_forget(const void *surface) { (void)surface; }
static uint32_t vgl_rt_now(void) { return 0; }
static void vgl_rt_cpu_switch(uint32_t a, uint32_t b, uint32_t c) {}
static void vgl_rt_end_scene(SceGxmContext *c) { ++ends; }
static void *vgl_free_unused;

#include "scene_fixture.inc"

static framebuffer fbs[3];
static texture textures[3];

static void new_fb(framebuffer *f, texture *t) {
    memset(f, 0, sizeof *f);
    f->width = f->height = 64;
    f->active = GL_TRUE;
    f->tex = t;
    f->colorbuffer.data = t->data = (void *)0x1000;
}
static void reset_counts(void) { begins = ends = allocs = viewports = depth_inits = target_attempts = 0; }
static void next_frame(void) {   // what vglSwapBuffers leaves
    ++vgl_framecount;
    needs_end_scene = GL_FALSE;
    if (!needs_scene_reset) ++ends;
    needs_scene_reset = GL_TRUE;
}

int main(void) {
    gxm_depth_stencil_surface.depthData = arena;
    new_fb(&fbs[0], &textures[0]);
    new_fb(&fbs[1], &textures[1]);
    // 1. the display: a scene begins
    active_write_fb = NULL;
    assert(scene_reset() == GL_TRUE && begins == 1 && ends == 0 && !needs_scene_reset && needs_end_scene);
    assert(scene_reset() == GL_TRUE && begins == 1);   // nothing to do
    // 2. a framebuffer object whose depth surface cannot be allocated: the display scene is ended, nothing else happens, the entry fails
    active_write_fb = &fbs[0];
    fail_alloc = 1;
    reset_counts();
    assert(scene_reset() == GL_FALSE);
    assert(ends == 1 && begins == 0 && allocs == 1 && depth_inits == 0);
    assert(needs_scene_reset && !needs_end_scene && vgl_scene_failures == 1);
    assert(fbs[0].depthbuffer_ptr == NULL && !(fbs[0].depthbuffer_state & DEPTHBUFFER_READY));   // nothing published
    // 3. the next draw of the same frame is not tried again (no allocation, no second end of the finished scene)
    reset_counts();
    assert(scene_reset() == GL_FALSE && scene_reset() == GL_FALSE);
    assert(allocs == 0 && ends == 0 && begins == 0 && vgl_scene_failures == 1);
    // 4. another framebuffer is not held back: it gets its scene in the same frame
    active_write_fb = &fbs[1];
    fail_alloc = 0;
    reset_counts();
    assert(scene_reset() == GL_TRUE && begins == 1 && ends == 0 && depth_inits == 1 && fbs[1].depthbuffer_ptr != NULL);
    // 5. back to the failed one in the same frame: still skipped (its scene would have to end the open one for nothing)
    active_write_fb = &fbs[0];
    reset_counts();
    assert(scene_reset() == GL_FALSE && allocs == 0 && ends == 0 && begins == 0);
    // 6. the next frame tries again, now with room
    next_frame();
    reset_counts();
    assert(scene_reset() == GL_TRUE && allocs == 1 && begins == 1 && depth_inits == 1);   // (a colour-only object: a depth surface, no stencil)
    assert((fbs[0].depthbuffer_state & DEPTHBUFFER_READY) && fbs[0].depthbuffer_ptr == &fbs[0].depthbuffer);
    // 7. a stencil surface that cannot be allocated after the depth surface: the depth surface is given back, nothing is published
    new_fb(&fbs[2], &textures[2]);
    fbs[2].depthbuffer_state = DEPTHBUFFER_WANTS_STENCIL;
    next_frame();
    active_write_fb = &fbs[2];
    fail_stencil_only = 1;
    reset_counts();
    dirty_marks = 0;
    assert(scene_reset() == GL_FALSE && depth_inits == 0 && dirty_marks == 1 && fbs[2].depthbuffer_ptr == NULL);
    fail_stencil_only = 0;
    // 8. sceGxmBeginScene refuses: no scene is left open, and a later frame can try again
    next_frame();
    fail_begin = 1;
    reset_counts();
    assert(scene_reset() == GL_FALSE && begins == 0 && needs_scene_reset && !needs_end_scene);
    fail_begin = 0;
    assert(scene_reset() == GL_FALSE);   // (same frame: skipped)
    next_frame();
    reset_counts();
    assert(scene_reset() == GL_TRUE && begins == 1);
    // 9. the render target cannot be created: no begin without a target
    new_fb(&fbs[0], &textures[0]);
    active_write_fb = &fbs[0];
    next_frame();
    fail_target = 1;
    reset_counts();
    assert(scene_reset() == GL_FALSE && begins == 0 && target_attempts == 1);
    fail_target = 0;
    next_frame();
    reset_counts();
    assert(scene_reset() == GL_TRUE && begins == 1);
    // 10. the display's own begin failing is handled the same way
    active_write_fb = NULL;
    next_frame();
    fail_begin = 1;
    assert(scene_reset() == GL_FALSE && needs_scene_reset && !needs_end_scene);
    fail_begin = 0;
    next_frame();
    assert(scene_reset() == GL_TRUE);
    // Per-FBO multisampling still uses the real allocation/failure path and target mode.
    for (int mode = SCE_GXM_MULTISAMPLE_2X; mode <= SCE_GXM_MULTISAMPLE_4X; ++mode) {
        new_fb(&fbs[0], &textures[0]);
        fbs[0].msaa = mode;
        active_write_fb = &fbs[0];
        next_frame();
        fail_alloc = 1;
        reset_counts();
        assert(scene_reset() == GL_FALSE && allocs == 1 && begins == 0 && target_attempts == 0);
        assert(fbs[0].depthbuffer_ptr == NULL && !(fbs[0].depthbuffer_state & DEPTHBUFFER_READY));
        fail_alloc = 0;
        next_frame();
        reset_counts();
        assert(scene_reset() == GL_TRUE && allocs == 1 && begins == 1 && target_attempts == 1);
        assert(cur_msaa == mode && target_mode == mode);
        assert(last_alloc_size == (size_t)(4 * 64 * 64 * (mode == SCE_GXM_MULTISAMPLE_4X ? 4 : 2)));
    }
    // 11. the 1.1 behaviour with the switch off: a refused begin is not an error to the caller, and nothing is bounded per frame
    vgl_robust_scenes = GL_FALSE;
    next_frame();
    fail_begin = 1;
    reset_counts();
    const unsigned failures_before = vgl_scene_failures;
    assert(scene_reset() == GL_TRUE && vgl_scene_failures == failures_before);
    fail_begin = 0;
    printf("PASS: scene entry (allocation, render target and begin failures) over a model of sceGxm; %u failures counted, %lu driver log lines\n", vgl_scene_failures, vglog_count);
    return 0;
}
