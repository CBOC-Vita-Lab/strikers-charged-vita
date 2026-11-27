// 1.2 vitaGL GPU timer ring guard (dependencies/vitaGL/source/rt.c: vgl_rt_end_scene, the watcher, vglRtLastGpuFrame).
// The test builds the driver's own GPU-timer code (extracted from rt.c by run_vitagl_rt_ring_test.sh, not a copy) over a model of sceGxm, and forces the
// interleaving that overwrote a record: the render thread submits scenes while the watcher is blocked in sceGxmNotificationWait on the oldest one.
//   vitagl_rt_ring <scenes submitted while the watcher waits> <guard 1|0>
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char GLboolean;
typedef int SceUID;
typedef unsigned SceSize;
typedef struct { int unused; } SceGxmContext;
typedef struct { volatile unsigned int *address; uint32_t value; } SceGxmNotification;
typedef struct { uint32_t busy_us, span_us, tail_us; uint16_t scenes, timed; uint32_t pixel_us, tiling_us, display_us; } vglRtGpuFrame;
typedef struct { uint32_t switch_us; uint16_t scenes, noload, nostore, pad; uint32_t skipped_kib; } vglRtCpuFrame;
#define GL_FALSE 0
#define GL_TRUE 1
#define VGL_RT_CPU_TIMERS 4
#define VGL_RT_GPU_TIMERS 8

static unsigned int notifications[65];
static SceGxmNotification query_fence = { &notifications[0], 0 };
static unsigned submitted, gpu_completed, waits, inject_count, stopped;
static int drain;
static uint32_t held_submit_us, held_frame;   // the record the watcher is waiting on, as it was when the wait began
static uint32_t tick;
static jmp_buf stop_watcher;
static uint32_t vgl_rt_flags;
static float vgl_rt_clock_cost_us;

static uint32_t sceKernelGetProcessTimeLow(void) { return ++tick; }
static void *sceGxmGetNotificationRegion(void) { return notifications; }
static int sceKernelCreateSema(const char *s, int a, int b, int c, void *d) { return 1; }
/* patch 0023 (vglRtWaitScenesDone): the event flag calls rt.c now makes; no waiter in the model */
typedef unsigned int SceUInt;
#define SCE_EVENT_WAITMULTIPLE 0x1000
#define SCE_EVENT_WAITOR 1
#define SCE_EVENT_WAITCLEAR_PAT 0x20
static int sceKernelCreateEventFlag(const char *name, unsigned attr, unsigned pat, void *opt) { return 2; }
static int sceKernelSetEventFlag(int id, unsigned pat) { return 0; }
static int sceKernelWaitEventFlag(int id, unsigned pat, unsigned mode, unsigned *res, SceUInt *timeout) { return (int)0x80028005; }
static int sceKernelCreateThread(const char *s, int (*f)(SceSize, void *), int a, int b, int c, int d, void *e) { return 1; }
static int sceKernelStartThread(int t, int n, void *p) { return 0; }
static int sceKernelSignalSema(int s, int n) { return 0; }
static int sceKernelWaitSema(int s, int n, void *p) { stopped = 1; longjmp(stop_watcher, 1); }
static int sceGxmEndScene(SceGxmContext *c, void *v, const SceGxmNotification *n) { ++submitted; return 0; }
static void sceGxmNotificationWait(const SceGxmNotification *n);
#define vgl_log(...) ((void)0)

#define vgl_fast_memcpy memcpy
#include "rt_kinds.h"   // 0043 (the per-kind table of the watcher; a header of plain C, from the vitaGL sources: -I)
typedef struct { uint64_t busy_us, tiling_us, pixel_us; uint32_t scenes; uint16_t w, h; uint8_t display, used, pad[2]; } vglRtSceneKind;   // (vitaGL.h)
#include "rt_gpu_fixture.inc"

static void submit_scene(void) {
    ++query_fence.value;
    vgl_rt_end_scene(NULL);
}

static void sceGxmNotificationWait(const SceGxmNotification *n) {
    if (drain) {
        *n->address = n->value;
        gpu_completed = query_fence.value;
        return;
    }
    if (++waits == 2) {
        stopped = 2;
        longjmp(stop_watcher, 1);
    }
    // The render thread runs while the watcher waits; only the first scene completes, later GPU work stays in flight.
    held_submit_us = rt_ring[0].submit_us;
    held_frame = rt_ring[0].frame;
    for (unsigned i = 0; i < inject_count; ++i) submit_scene();
    *n->address = n->value;
    gpu_completed = 1;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    inject_count = (unsigned)strtoul(argv[1], NULL, 10);
    const int guard = atoi(argv[2]);
    vglRtSetRingGuard(guard);
    vglRtSetup(VGL_RT_GPU_TIMERS, 1, 1);
    submit_scene();
    if (!setjmp(stop_watcher)) rt_watcher(0, NULL);
    printf("guard=%d injected=%u submitted=%u produced=%u consumed=%u gpu_completed=%u published_fence=%u drops=%u\n",
           guard, inject_count, submitted, rt_produced, rt_consumed, gpu_completed, notifications[0], vglRtRingDrops());
    fflush(stdout);
    assert(stopped != 0);
    assert(submitted == inject_count + 1);
    if (guard) {
        // the record the watcher holds is intact, and no more than a ring of records is outstanding
        assert(rt_ring[0].submit_us == held_submit_us && rt_ring[0].frame == held_frame);
        assert(notifications[0] <= gpu_completed);
        assert(rt_produced - rt_consumed <= RT_SCENES);
        assert(vglRtRingDrops() == (inject_count >= RT_SCENES ? inject_count + 1 - RT_SCENES : 0));
        // the frame the dropped scenes belong to is not offered as a measurement; without drops it is
        rt_gpu_frame_head = 1;
        rt_gpu_frames[0].scenes = 1;
        rt_gpu_frames[0].timed = 1;
        rt_gpu_frame_ids[0] = rt_frame;
        vglRtGpuFrame sample;
        assert(vglRtLastGpuFrame(&sample) == 1);
        assert(sample.timed == (vglRtRingDrops() ? 0 : 1));
        // a frame after the drops is measured again
        rt_frame++;
        rt_gpu_frame_head = 2;
        rt_gpu_frames[1].scenes = 1;
        rt_gpu_frames[1].timed = 1;
        rt_gpu_frame_ids[1] = rt_frame;
        assert(vglRtLastGpuFrame(&sample) == 2 && sample.timed == 1);
        // the watcher catches up; recording resumes and nothing is lost
        const uint32_t drops = vglRtRingDrops();
        drain = 1;
        gpu_completed = query_fence.value;
        notifications[0] = gpu_completed;
        if (!setjmp(stop_watcher)) rt_watcher(0, NULL);
        assert(rt_consumed == rt_produced);
        const uint32_t before = rt_produced;
        submit_scene();
        assert(rt_produced == before + 1);
        if (!setjmp(stop_watcher)) rt_watcher(0, NULL);
        assert(rt_consumed == rt_produced);
        assert(vglRtRingDrops() == drops);
        printf("PASS: ring drains and resumes recording; drops=%u\n", drops);
    } else if (inject_count >= RT_SCENES) {
        // the 1.1 behaviour: the scene that wraps the ring is written over the record the watcher is still using (its submit time, then read for the
        // frame's busy time, is the wrapping scene's)
        assert(rt_ring[0].submit_us != held_submit_us);
        printf("control: the unguarded ring wrote over the record the watcher held (as in 1.1)\n");
    } else {
        assert(notifications[0] <= gpu_completed);
    }
    return 0;
}
