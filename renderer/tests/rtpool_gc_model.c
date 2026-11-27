// nxr4a: the vitaGL render target pool (patch 0039, MSC_VGX_RT_POOL) and the garbage collector's end of it (gxm.c garbage_collector, vgl_rtp_release, setup_fbo_render_target_ms, gc_enqueue), the REAL text cut
// out of gxm.c / gc_recovery.h by run_rtpool_gc_test.sh and built over a model of sceGxm and the kernel semaphores. Single thread: the collector job of a frame runs when its frame's swap has queued it.
// What it proves (and what it cannot):
//   - a render target let go in frame N (glFramebufferTexture2D detach / resize: mark_rt_as_dirty) is destroyed by the collector job of swap N+3 (FRAME_PURGE_FREQ - 1 jobs later): the "+3 / +4 pictures
//     after a size step" of 6122.log:1152-1156 and 6032.log:1379-1398 is that arithmetic; sceGxmDestroyRenderTarget runs in that job and nowhere else (pool off);
//   - with the pool on the same step makes no destroy at all, and the step back takes the kept target (a hit: no sceGxmCreateRenderTarget); the 0064 timers see 0 ms of destroy;
//   - the caps (count, KiB) and the drain hold, an exact shape only is handed back, no target is ever handed to two objects or destroyed while one holds it, none leaks (random sequences, ASan/UBSan).
//   It cannot say how long the console's sceGxmDestroyRenderTarget takes or what lock it holds: that is what the 0064 counters read on the console ([gcjob]).
//   rtpool_gc_model <scenario>   (the runner calls each)
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char GLboolean;
typedef int SceUID;
typedef unsigned int SceSize;
typedef enum { SCE_GXM_MULTISAMPLE_NONE = 0, SCE_GXM_MULTISAMPLE_2X = 1, SCE_GXM_MULTISAMPLE_4X = 2 } SceGxmMultisampleMode;
typedef struct SceGxmRenderTarget { int id, alive, inuse, w, h, refs, mode; } SceGxmRenderTarget;
typedef struct { uint16_t width, height, scenesPerFrame; SceGxmMultisampleMode multisampleMode; uint32_t multisampleLocations; void *hostMem; uint32_t hostMemSize; int driverMemBlock; } SceGxmRenderTargetParams;
#define FRAME_PURGE_FREQ 4
#define FRAME_PURGE_RENDERTARGETS_LIST_SIZE 128
#define FRAME_PURGE_LIST_SIZE 16384
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT ((int)0x80028005)
#define vgl_memset memset
#define vgl_free free

// ---- the model: virtual clock (us), render targets, semaphores
static uint32_t g_now_us = 0;
static uint32_t sceKernelGetProcessTimeLow(void) { return g_now_us; }
static int sceKernelDelayThread(unsigned us) { g_now_us += us; return 0; }
static void sceKernelExitProcess(int c) { (void)c; abort(); }
static int sceKernelExitDeleteThread(int c) { return c; }
static SceGxmRenderTarget g_rt[1 << 14];
static int g_rtN, g_created, g_destroyed, g_alive, g_destroy_cost_us = 120000, g_create_cost_us = 50000, g_hold_check = 1;
static unsigned g_drv_kib = 300;
static int sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *p, SceGxmRenderTarget **rt) {
    assert(g_rtN < (int)(sizeof g_rt / sizeof g_rt[0]));
    SceGxmRenderTarget *r = &g_rt[g_rtN];
    r->id = g_rtN++; r->alive = 1; r->inuse = 0; r->w = p->width; r->h = p->height; r->refs = p->scenesPerFrame; r->mode = p->multisampleMode;
    ++g_created; ++g_alive; g_now_us += g_create_cost_us;
    *rt = r;
    return 0;
}
static int sceGxmDestroyRenderTarget(SceGxmRenderTarget *rt) {
    assert(rt->alive && "a render target destroyed twice");
    if (g_hold_check) assert(!rt->inuse && "a render target destroyed while an object holds it");
    rt->alive = 0; --g_alive; ++g_destroyed; g_now_us += g_destroy_cost_us;
    return 0;
}
static int sceGxmGetRenderTargetMemSize(const SceGxmRenderTargetParams *p, unsigned *size) { (void)p; *size = g_drv_kib * 1024u; return 0; }
static int setup_render_target_ms(SceGxmRenderTarget **rt, int w, int h, int refs, SceGxmMultisampleMode mode) {
    SceGxmRenderTargetParams p; memset(&p, 0, sizeof p);
    p.width = (uint16_t)w; p.height = (uint16_t)h; p.scenesPerFrame = (uint16_t)refs; p.multisampleMode = mode; p.driverMemBlock = -1;
    return sceGxmCreateRenderTarget(&p, rt);
}
// the two collector semaphores (the model of the request / credit semaphores of gxm.c): credits start at FRAME_PURGE_FREQ
static SceUID gc_mutex[2] = {10, 11};
static int g_requests, g_credits = FRAME_PURGE_FREQ;
static jmp_buf g_idle;
static int sceKernelWaitSema(SceUID s, int need, unsigned *timeout) {
    (void)need; (void)timeout;
    if (s == gc_mutex[0]) { if (g_requests > 0) { --g_requests; return 0; } longjmp(g_idle, 1); }
    if (g_credits > 0) { --g_credits; return 0; }
    return SCE_KERNEL_ERROR_WAIT_TIMEOUT;   // (a real thread would block: the single-thread model never gets here because every job ends before the next swap)
}
static int sceKernelSignalSema(SceUID s, int n) { if (s == gc_mutex[0]) g_requests += n; else g_credits += n; return 0; }
static uint32_t vgl_gc_pause_ms = 16;
static void vgl_gc_note_legacy(void) {}

void *frame_purge_list[FRAME_PURGE_FREQ][1];
void *frame_rt_purge_list[FRAME_PURGE_FREQ][FRAME_PURGE_RENDERTARGETS_LIST_SIZE];
int frame_purge_idx = 0, frame_elem_purge_idx = 0, frame_rt_purge_idx = 0;
static int frame_purge_clean_idx = 1;
#undef FRAME_PURGE_LIST_SIZE
#define FRAME_PURGE_LIST_SIZE 1
#define mark_rt_as_dirty(x) frame_rt_purge_list[frame_purge_idx][frame_rt_purge_idx++] = x   /* utils/mem_utils.h, the build without shared render targets (checked by the runner) */

#include "counters.inc"       // 0064's counters (gxm.c, before gc_recovery.h)
#include "utils/gc_recovery.h"
#include "pool.inc"           // vgl_rtp_*, setup_fbo_render_target_ms, vgl_rtp_release (gxm.c)
#include "collector.inc"      // garbage_collector (gxm.c)

// ---- a framebuffer object's life, as the driver calls it: acquire (setup_fbo_render_target_ms), hold, let go (mark_rt_as_dirty at detach or resize, then the collector)
static SceGxmRenderTarget *acquire(int w, int h, int refs, int mode) {
    SceGxmRenderTarget *rt = NULL;
    assert(setup_fbo_render_target_ms(&rt, w, h, refs, (SceGxmMultisampleMode)mode) == 0);
    assert(rt->alive && !rt->inuse && "a render target handed to a second object");
    assert(rt->w == w && rt->h == h && rt->refs == refs && rt->mode == mode && "a render target of another shape handed out");
    rt->inuse = 1;
    return rt;
}
static void release(SceGxmRenderTarget *rt) { rt->inuse = 0; mark_rt_as_dirty(rt); }   // (the collector gets it three jobs later)
// the end of a frame: vglSwapBuffers' gc_enqueue, then the job runs (and gives its credit back)
static int g_job_destroys[1024], g_job;
static void swap_end(void) {
    const int before = g_destroyed;
    assert(gc_enqueue(NULL) == 1);
    if (!setjmp(g_idle)) garbage_collector(0, NULL);
    assert(g_requests == 0);
    if (g_job < 1024) g_job_destroys[g_job] = g_destroyed - before;
    ++g_job;
}
static void frames(int n) { for (int i = 0; i < n; ++i) swap_end(); }
static int fails;
#define CHECK(c, what) do { if (!(c)) { ++fails; printf("FAIL %s\n", what); } else printf("ok   %s\n", what); } while (0)
static void reset(void) { vglSetRenderTargetPool(0, 0); memset(vgl_rtp, 0, sizeof vgl_rtp); memset(vgl_rtp_stat, 0, sizeof vgl_rtp_stat); vgl_gcj_jobs = vgl_gcj_destroys = vgl_gcj_destroy_total_us = vgl_gcj_wait_total_us = vgl_gcj_job_max_us = vgl_gcj_destroy_max_us = vgl_gcj_wait_max_us = 0; memset(g_rt, 0, sizeof g_rt); g_rtN = g_created = g_destroyed = g_alive = 0; g_job = 0; memset(g_job_destroys, 0, sizeof g_job_destroys); g_now_us = 0; g_requests = 0; g_credits = FRAME_PURGE_FREQ; memset(frame_rt_purge_list, 0, sizeof frame_rt_purge_list); frame_rt_purge_idx = 0; }

int main(int argc, char **argv) {
    const char *sc = argc > 1 ? argv[1] : "all";
    const int all = !strcmp(sc, "all");
    if (all || !strcmp(sc, "step")) {   // S1 / S2: a size step in frame 5 (728x512 -> 768x544) and the step back in frame 15
        for (int pool = 0; pool <= 1; ++pool) {
            reset(); vglSetRenderTargetPool(pool ? 16 : 0, 8192);
            SceGxmRenderTarget *a = acquire(728, 512, 3, 0);
            frames(5);
            release(a);                                  // frame 5: the detach (the old object's target goes to the purge list of frame 5)
            SceGxmRenderTarget *b = acquire(768, 544, 3, 0);   // (new target: sceGxmCreateRenderTarget, the "rt1/51 ms")
            frames(10);
            release(b);                                  // frame 15: back to the first size
            SceGxmRenderTarget *a2 = acquire(728, 512, 3, 0);
            frames(10);
            uint32_t jobs, destroys, jmax, dmax, wmax, dtot, wtot;
            // (this call clears the maxima: what the console's [gcjob] line reads)
            vglGetGcJobStats(&jobs, &destroys, &jmax, &dmax, &wmax, &dtot, &wtot);
            if (!pool) {
                int when = -1; for (int j = 0; j < 40; ++j) if (g_job_destroys[j] && when < 0) when = j;
                char w[160]; snprintf(w, sizeof w, "pool off: the target let go in frame 5 is destroyed by the job of swap %d (5 + FRAME_PURGE_FREQ - 1)", when);
                CHECK(when == 5 + FRAME_PURGE_FREQ - 1, w);
                CHECK(g_destroyed == 2 && g_created == 3, "pool off: two destroys (both steps) and three creates (the step back makes the first size again)");
                CHECK(dmax == (uint32_t)g_destroy_cost_us && destroys == 2 && dtot == 2u * (uint32_t)g_destroy_cost_us, "pool off: the 0064 timers read the destroy (max, count, total)");
                CHECK(jmax >= (uint32_t)g_destroy_cost_us, "pool off: the longest collector job holds the destroy");
            } else {
                CHECK(g_destroyed == 0, "pool on: no sceGxmDestroyRenderTarget in any collector job over the step and the step back");
                CHECK(g_created == 2, "pool on: the step back takes the kept target (2 creates, not 3)");
                uint32_t hits, creates, kept, letgo, keptNow, kibNow; vglGetRenderTargetPoolStats(&hits, &creates, &kept, &letgo, &keptNow, &kibNow);
                CHECK(hits == 1 && creates == 2 && kept == 2 && letgo == 0 && keptNow == 1 && kibNow == g_drv_kib, "pool on: counters (1 hit, 2 creates, 2 kept, none let go, 1 kept now)");
                CHECK(dmax == 0 && destroys == 0 && jmax < 1000, "pool on: the 0064 timers read no destroy and no long job");
                CHECK(a2 == a, "pool on: the step back is handed the very target of the first size");
            }
        }
    }
    if (all || !strcmp(sc, "caps")) {
        reset(); vglSetRenderTargetPool(1, 0);   // count cap 1
        SceGxmRenderTarget *r[3]; for (int i = 0; i < 3; ++i) r[i] = acquire(700 + 16 * i, 500, 3, 0);
        for (int i = 0; i < 3; ++i) release(r[i]);
        frames(8);
        CHECK(g_destroyed == 2 && g_alive == 1, "count cap 1: three let go, one kept, two destroyed");
        reset(); g_drv_kib = 3000; vglSetRenderTargetPool(16, 4096);   // KiB cap: 1 of 3000 KiB fits, a second does not
        for (int i = 0; i < 3; ++i) r[i] = acquire(700 + 16 * i, 500, 3, 0);
        for (int i = 0; i < 3; ++i) release(r[i]);
        frames(8);
        CHECK(g_destroyed == 2 && g_alive == 1, "KiB cap 4096 at 3000 KiB each: one kept, two destroyed");
        g_drv_kib = 300;
    }
    if (all || !strcmp(sc, "drain")) {
        reset(); vglSetRenderTargetPool(16, 8192);
        SceGxmRenderTarget *r[4]; for (int i = 0; i < 4; ++i) r[i] = acquire(700 + 16 * i, 500, 3, 0);
        for (int i = 0; i < 3; ++i) release(r[i]);
        frames(8);
        CHECK(g_destroyed == 0 && g_alive == 4, "pool on: three kept, one in use, nothing destroyed");
        vglSetRenderTargetPool(1, 8192);
        CHECK(g_destroyed == 2 && g_alive == 2, "shrinking count cap to 1 evicts two kept targets");
        vglSetRenderTargetPool(0, 0);
        CHECK(g_destroyed == 3 && g_alive == 1, "switching the pool off destroys what it kept (and only that)");
        release(r[3]);
        frames(8);
        CHECK(g_destroyed == 4 && g_alive == 0, "target held across disable is destroyed on release");
        int leaked = 0;
        for (int i = 0; i < VGL_RTP_SLOTS; ++i) if (vgl_rtp[i].state != 0) ++leaked;
        CHECK(leaked == 0, "disable-while-held leaves zero leaked pool slots");
    }
    if (all || !strcmp(sc, "random")) {   // 400 random lives: 5 shapes, up to 6 objects at a time, collector jobs between, ASan and the asserts in the model are the checks
        for (int seed = 1; seed <= 400; ++seed) {
            reset(); srand(seed);
            vglSetRenderTargetPool(seed % 3 == 0 ? 2 : (seed % 3 == 1 ? 16 : 48), seed % 4 == 0 ? 700 : 8192);
            SceGxmRenderTarget *held[6] = {0};
            for (int step = 0; step < 300; ++step) {
                const int k = rand() % 6;
                if (held[k] && rand() % 2) { release(held[k]); held[k] = NULL; }
                else if (!held[k]) { const int s = rand() % 5; held[k] = acquire(640 + 32 * s, 448 + 16 * s, 1 + (rand() % 3), rand() % 2); }
                if (rand() % 3 == 0) swap_end();
            }
            for (int k = 0; k < 6; ++k) if (held[k]) release(held[k]);
            frames(FRAME_PURGE_FREQ + 1);
            vglSetRenderTargetPool(0, 0);
            if (g_alive != 0) { ++fails; printf("FAIL seed %d: %d targets leaked\n", seed, g_alive); break; }
            if (g_created != g_destroyed) { ++fails; printf("FAIL seed %d: %d created, %d destroyed\n", seed, g_created, g_destroyed); break; }
        }
        if (!fails) printf("ok   400 random lives: no double hand-out, no wrong shape, no destroy under an object, no leak after the drain\n");
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
