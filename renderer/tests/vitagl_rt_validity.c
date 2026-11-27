/* Actual whole rt.c + actual public ABI; deterministic kernel/GXM boundaries. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vitaGL.h"

#define _SHARED_H_ /* Replace platform-only shared dependencies, not rt.c methods. */
#define VGL_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define SCE_GXM_TILE_SIZEX 32
#define SCE_GXM_TILE_SIZEY 32
#define SCE_GXM_MULTISAMPLE_2X 2
#define SCE_GXM_MULTISAMPLE_4X 4
#define SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED 0
#define SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED 1
#define SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED 0
#define SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED 1

typedef struct framebuffer { int unused; } framebuffer;
static framebuffer *active_write_fb;
static int cur_msaa;
static unsigned int notifications[65];
static SceGxmNotification query_fence = { &notifications[0], 0 };
static unsigned checks, failures, submissions, gpu_completed, notification_waits;
static unsigned first_count, observed, injected_follow, idle_calls;
static int all_untimed, drops, independent_drop;
static uint32_t tick, observed_seq, observed_done, observed_gpu, observed_timed, observed_scenes;
static jmp_buf leave_watcher;

static void check(int pass, const char *label) {
    ++checks;
    if (!pass) { ++failures; printf("FAIL %s\n", label); }
}

static uint32_t sceKernelGetProcessTimeLow(void) { return ++tick; }
static void *sceGxmGetNotificationRegion(void) { return notifications; }
static int sceKernelCreateSema(const char *name, int a, int b, int c, void *d) { return 1; }
/* patch 0023 (vglRtWaitScenesDone): the event flag calls rt.c now makes; the model has no waiter, so a wait times out at once */
typedef unsigned int SceUInt;
#define SCE_EVENT_WAITMULTIPLE 0x1000
#define SCE_EVENT_WAITOR 1
#define SCE_EVENT_WAITCLEAR_PAT 0x20
static int sceKernelCreateEventFlag(const char *name, unsigned attr, unsigned pat, void *opt) { return 2; }
static int sceKernelSetEventFlag(int id, unsigned pat) { return 0; }
static int sceKernelWaitEventFlag(int id, unsigned pat, unsigned mode, unsigned *res, SceUInt *timeout) { return (int)0x80028005; }
static int sceKernelCreateThread(const char *name, int (*fn)(SceSize, void *), int p, int s, int a, int m, void *o) { return 1; }
static int sceKernelStartThread(int thread, int n, void *data) { return 0; }
static int sceKernelSignalSema(int sema, int n) { return 0; }
static int sceKernelWaitSema(int sema, int n, void *timeout);
static int sceGxmEndScene(SceGxmContext *context, void *vertex, const SceGxmNotification *notification) {
    ++submissions;
    check(notification->address >= notifications && notification->address < notifications + 65, "BOUNDARY notification inside region");
    return 0;
}
static void sceGxmNotificationWait(const SceGxmNotification *notification) {
    ++notification_waits;
    check(notification->address != query_fence.address, "BOUNDARY no unrequested shared-fence wait added");
    *notification->address = notification->value;
    gpu_completed = notification->value;
}
static void sceGxmDepthStencilSurfaceSetForceLoadMode(SceGxmDepthStencilSurface *d, int mode) { d->load = mode; }
static void sceGxmDepthStencilSurfaceSetForceStoreMode(SceGxmDepthStencilSurface *d, int mode) { d->store = mode; }
static int sceGxmDepthStencilSurfaceGetForceLoadMode(SceGxmDepthStencilSurface *d) { return d->load; }
static int sceGxmDepthStencilSurfaceGetForceStoreMode(SceGxmDepthStencilSurface *d) { return d->store; }
#define vgl_log(...) ((void)0)
#define vgl_fast_memcpy memcpy

#include RT_SOURCE

_Static_assert(sizeof(vglRtGpuFrame) == 28, "Actual public GPU frame ABI");
_Static_assert(sizeof(vglRtCpuFrame) == 16, "Actual public CPU frame ABI");
_Static_assert(sizeof(uint32_t) == 4 && sizeof(GLuint) == 4, "Clock/handle widths");

static void submit_scene(void) {
    ++query_fence.value; /* Actual gxm.c scene_end caller contract. */
    vgl_rt_end_scene(NULL);
}

static int sceKernelWaitSema(int sema, int n, void *timeout) {
    ++idle_calls;
    check(idle_calls <= 4, "BOUNDARY bounded deterministic watcher lifecycle");
    if (independent_drop && !injected_follow) {
        check(rt_consumed == rt_produced, "DROP-ONLY preceding frame drained");
        check(vglRtRingDrops() == 8, "DROP-ONLY eight early scenes of current frame dropped");
        injected_follow = 1;
        /* Still the SAME frame that lost eight scenes; now all records are timed. */
        for (unsigned i = 0; i < 3; ++i) submit_scene();
        vgl_rt_frame_end();
        submit_scene();
        return 0;
    }
    if (drops && !injected_follow) {
        /* Ring-full dropped all later frame records. Submit only after real drain. */
        check(rt_consumed == rt_produced, "DROP first ring drained before follow-up");
        check(vglRtRingDrops() == first_count - RT_SCENES, "DROP actual ring overflow counted");
        injected_follow = 1;
        submit_scene();
        return 0;
    }
    if (!observed) {
        vglRtGpuFrame sample;
        vglRtStats stats;
        uint32_t submitted, done;
        observed_seq = vglRtLastGpuFrame(&sample);
        vglRtGetStats(&stats);
        vglRtSceneProgress(&submitted, &done);
        check(observed_seq != 0, "SAMPLE actual watcher published preceding frame");
        observed_timed = sample.timed;
        observed_scenes = sample.scenes;
        observed_done = done;
        observed_gpu = gpu_completed;
        if (drops || independent_drop || all_untimed || first_count > RT_SLOTS - 16)
            check(sample.timed == 0, "VALIDITY incomplete timing never offered as full-frame sample");
        else
            check(sample.timed == sample.scenes && sample.timed == first_count, "VALIDITY complete frame remains accepted");
        check(sample.scenes == (independent_drop ? 3 : (all_untimed ? 12 : (drops ? RT_SCENES : first_count))), "SAMPLE original scene count preserved");
        check(stats.gpu_frames[(observed_seq - 1) % RT_FRAMES].timed == (independent_drop ? 3 : (all_untimed ? 0 : (first_count < 48 ? first_count : 48))), "COUNTER raw timing count not rewritten");
        check(stats.gpu_scenes_timed == (independent_drop ? 52 : (first_count < 48 ? first_count + 1 : (drops ? 49 : 48))), "COUNTER actual timed scenes retained");
        check(stats.gpu_scenes_untimed == rt_produced - stats.gpu_scenes_timed, "COUNTER untimed scene accounting retained");
        check(submitted == rt_produced, "SUBMISSION actual recorded count retained");
        /* Progress is reported only: no requirement to preserve premature GPU done. */
        printf("OBSERVED scenes=%u offered_timed=%u raw_timed=%u done=%u gpu_completed=%u drops=%u\n",
            sample.scenes, sample.timed, stats.gpu_frames[(observed_seq - 1) % RT_FRAMES].timed,
            done, gpu_completed, vglRtRingDrops());
        observed = 1;
        vgl_rt_frame_end();
        for (unsigned i = 0; i < 3; ++i) submit_scene();
        vgl_rt_frame_end();
        submit_scene();
        return 0;
    }
    longjmp(leave_watcher, 1); /* End only at the explicit kernel wait boundary. */
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "healthy")) first_count = 1;
    else if (!strcmp(argv[1], "boundary47")) first_count = 47;
    else if (!strcmp(argv[1], "boundary48")) first_count = 48;
    else if (!strcmp(argv[1], "partial60")) first_count = 60;
    else if (!strcmp(argv[1], "untimed")) { first_count = 48; all_untimed = 1; }
    else if (!strcmp(argv[1], "drop300")) { first_count = 300; drops = 1; }
    else if (!strcmp(argv[1], "independent-drop")) { first_count = 256; independent_drop = 1; }
    else return 2;
    vglRtSetup(VGL_RT_GPU_TIMERS, 1, 1);
    check(vgl_rt_flags == VGL_RT_GPU_TIMERS, "SETUP timer flags unchanged");
    for (unsigned i = 0; i < first_count; ++i) submit_scene();
    vgl_rt_frame_end();
    if (independent_drop) {
        for (unsigned i = 0; i < 8; ++i) submit_scene();
    } else if (all_untimed) {
        for (unsigned i = 0; i < 12; ++i) submit_scene();
        vgl_rt_frame_end();
        submit_scene();
    } else if (!drops) submit_scene();
    if (!setjmp(leave_watcher)) rt_watcher(0, NULL);
    check(observed, "LIFECYCLE first published frame observed");
    vglRtGpuFrame recovered;
    const uint32_t final_seq = vglRtLastGpuFrame(&recovered);
    check(final_seq > observed_seq && recovered.scenes == 3 && recovered.timed == 3, "RECOVERY later fully timed frame accepted");
    check(rt_consumed == rt_produced, "RECOVERY recorded ring fully drained");
    check(submissions == query_fence.value, "COUNTER all GPU submissions preserved");
    check(vglRtRingDrops() == (independent_drop ? 8 : (drops ? first_count - RT_SCENES : 0)), "COUNTER no new drops or policy change");
    check(recovered.busy_us > 0, "RECOVERY marker availability only; busy accuracy NOT proven");
    printf("SUMMARY case=%s checks=%u failures=%u notifications=%u submissions=%u recovered_timed=%u\n",
        argv[1], checks, failures, notification_waits, submissions, recovered.timed);
    return failures ? 1 : 0;
}
