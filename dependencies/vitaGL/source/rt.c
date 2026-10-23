/*
 * rt.c (RT, wave 5 of the Mario Strikers Charged port): what a scene costs, and not paying for what nobody reads.
 *
 * 1. Depth/stencil load and store per scene. With STORE_DEPTH_STENCIL every depth/stencil surface is created with force-load and force-store
 *    enabled (gxm.c init_depth_stencil_buffer): every scene reads the whole surface from memory when it begins and writes it back when it ends,
 *    4 bytes per sample (5 with stencil), whether or not anything reads it. sceGxm does both only on request: without force-load a scene starts
 *    from the surface's background values (depth 1.0, stencil 0, mask 0), without force-store the tiles are not written back (a partial render
 *    still uses the memory as it needs to). This file decides per scene:
 *      - VGL_RT_AUTO_NOLOAD: a scene whose first operation is a glClear that writes depth (and stencil, when the surface has stencil) over the
 *        whole target does not load: the clear overwrites every sample the load would have brought in (misc.c glClear sets vgl_rt_clear_full).
 *      - VGL_RT_HINTS: the application says what it knows (vglRtSceneHint): NOLOAD (nothing in the scene reads depth/stencil that was there
 *        before it began) and NOSTORE (nothing reads this depth/stencil after the scene). A hint is consumed by the next scene_reset() whether or
 *        not that begins a scene, and applies only when the scene begun is on the framebuffer it names.
 *    A scene that does not load gets its mask (the scissor bit kept in the depth surface) rebuilt at its start, as every framebuffer-object
 *    scene already does under STORE_DEPTH_STENCIL; the display needs that too once it can start from the background values. Since such a
 *    scene starts with the background mask (0 everywhere), the full-target quad that clears the mask before the region quad is left out
 *    (tests.c update_scissor_test), exactly as upstream vitaGL does at every scene start in its default build (without STORE_DEPTH_STENCIL).
 *    The colour surface is always loaded and stored: sceGxm has no switch for that.
 * 2. CPU timers (VGL_RT_CPU_TIMERS): sceGxmEndScene, sceGxmBeginScene and the set-up after it (viewport, region clip, mask quads) per scene,
 *    summed per frame.
 * 3. GPU timers (VGL_RT_GPU_TIMERS): every sceGxmEndScene gets a fragment notification of its own (a ring of 64 words of the notification
 *    region, words 1..64; vitaGL uses word 0 for query_fence, which then no longer advances: occlusion queries are not supported with these
 *    timers). A watcher thread waits for each scene's word in order and takes the time it sees it: the scene's GPU completion. Per frame
 *    (scenes between two vglSwapBuffers): busy = sum over scenes of (done - max(submitted, previous done)), span = last done - first
 *    submitted, tail = last done - last submitted. What that misses: work the GPU does on a scene before sceGxmEndScene returns is not seen as
 *    busy (the start is taken at submission); the watcher's wake-up latency is added to each done time (the count of scenes that were already
 *    complete when the watcher looked says how often it lagged).
 */
#include "shared.h"

uint32_t vgl_rt_flags = 0;
uint32_t vgl_aniso_min = 0; // utils/gxm_utils.h vglSetTexMinFilter
void vglSetAnisotropicFilter(uint32_t sceFilter) { vgl_aniso_min = (sceFilter == 2 || sceFilter == 3) ? sceFilter : 0; }
GLuint vgl_rt_hint_fb = 0;
uint32_t vgl_rt_hint = 0;
GLboolean vgl_rt_hint_valid = GL_FALSE;
GLboolean vgl_rt_clear_full = GL_FALSE;
GLboolean vgl_rt_skip_invalidation = GL_FALSE; // tests.c update_scissor_test: the scene began from the background mask (no full-target invalidation quad)
uint32_t vgl_rt_invalidations_skipped = 0;

// cumulative counters (render thread)
uint32_t vgl_rt_scenes = 0, vgl_rt_scenes_display = 0, vgl_rt_scenes_noload = 0, vgl_rt_scenes_noload_auto = 0, vgl_rt_scenes_nostore = 0, vgl_rt_hints_given = 0, vgl_rt_hints_used = 0;
uint64_t vgl_rt_ds_loaded = 0, vgl_rt_ds_stored = 0, vgl_rt_ds_load_skipped = 0, vgl_rt_ds_store_skipped = 0;
uint64_t vgl_rt_cpu_end_us = 0, vgl_rt_cpu_begin_us = 0, vgl_rt_cpu_setup_us = 0;
uint32_t vgl_rt_cpu_timed = 0, vgl_rt_cpu_switch_max_us = 0;
static float vgl_rt_clock_cost_us = 0.0f;

// per-frame (render thread): written at vglSwapBuffers
#define RT_FRAMES 128
static uint32_t rt_cpu_frame_switch_us = 0, rt_cpu_frame_scenes = 0, rt_cpu_frame_noload = 0, rt_cpu_frame_nostore = 0;
static uint64_t rt_cpu_frame_skipped_bytes = 0;
static vglRtCpuFrame rt_cpu_frames[RT_FRAMES];
static volatile uint32_t rt_cpu_frame_head = 0;

// GPU timing
#define RT_SCENES 256
#define RT_SLOTS 64
typedef struct {
	uint32_t frame;
	uint32_t submit_us;
	uint32_t done_us;
	uint8_t timed;
} rt_scene_rec;
static rt_scene_rec rt_ring[RT_SCENES];
static volatile uint32_t rt_produced = 0; // scenes recorded
static volatile uint32_t rt_consumed = 0; // scenes the watcher has finished with
static volatile uint32_t rt_watcher_idle = 0;
static volatile unsigned int *rt_slots = NULL;
static SceUID rt_sema = -1, rt_thread = -1;
static uint32_t rt_frame = 0; // render thread: frames (vglSwapBuffers) so far
static vglRtGpuFrame rt_gpu_frames[RT_FRAMES];
static volatile uint32_t rt_gpu_frame_head = 0;
static volatile uint32_t rt_gpu_scenes_timed = 0, rt_gpu_scenes_untimed = 0, rt_gpu_scenes_late = 0;

static inline uint32_t rt_now(void) { return sceKernelGetProcessTimeLow(); }

uint32_t vgl_rt_ds_bytes(uint32_t w, uint32_t h, GLboolean stencil) {
	uint32_t n = VGL_ALIGN(w, SCE_GXM_TILE_SIZEX) * VGL_ALIGN(h, SCE_GXM_TILE_SIZEY);
	if (msaa_mode == SCE_GXM_MULTISAMPLE_2X)
		n *= 2;
	else if (msaa_mode == SCE_GXM_MULTISAMPLE_4X)
		n *= 4;
	return n * (stencil ? 5 : 4);
}

void vglRtSceneHint(GLuint fb, uint32_t hints) {
	vgl_rt_hint_fb = fb;
	vgl_rt_hint = hints;
	vgl_rt_hint_valid = GL_TRUE;
	vgl_rt_hints_given++;
}

// gxm.c scene_reset, just before sceGxmBeginScene: the load/store modes of the surface the scene will use. Returns the hints applied.
uint32_t vgl_rt_scene_begin(SceGxmDepthStencilSurface *ds, uint32_t w, uint32_t h) {
	uint32_t hints = 0;
	if (vgl_rt_hint_valid && (framebuffer *)vgl_rt_hint_fb == active_write_fb && (vgl_rt_flags & VGL_RT_HINTS)) {
		hints = vgl_rt_hint;
		vgl_rt_hints_used++;
	}
	GLboolean autoload = GL_FALSE;
	if (vgl_rt_clear_full && (vgl_rt_flags & VGL_RT_AUTO_NOLOAD) && !(hints & VGL_RT_NOLOAD)) {
		hints |= VGL_RT_NOLOAD;
		autoload = GL_TRUE;
	}
	vgl_rt_scenes++;
	rt_cpu_frame_scenes++;
	if (!active_write_fb)
		vgl_rt_scenes_display++;
	if (!ds)
		return hints;
	const uint32_t bytes = vgl_rt_ds_bytes(w, h, ds->stencilData != NULL);
	if (vgl_rt_flags & (VGL_RT_HINTS | VGL_RT_AUTO_NOLOAD)) {
		// set both modes explicitly for every scene (the surface keeps them until the next scene on it)
		sceGxmDepthStencilSurfaceSetForceLoadMode(ds, (hints & VGL_RT_NOLOAD) ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
		sceGxmDepthStencilSurfaceSetForceStoreMode(ds, (hints & VGL_RT_NOSTORE) ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
	}
	const GLboolean loads = sceGxmDepthStencilSurfaceGetForceLoadMode(ds) == SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED;
	const GLboolean stores = sceGxmDepthStencilSurfaceGetForceStoreMode(ds) == SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED;
	if (loads) {
		vgl_rt_ds_loaded += bytes;
	} else {
		vgl_rt_ds_load_skipped += bytes;
		rt_cpu_frame_skipped_bytes += bytes;
		vgl_rt_scenes_noload++;
		rt_cpu_frame_noload++;
		if (autoload)
			vgl_rt_scenes_noload_auto++;
	}
	if (stores) {
		vgl_rt_ds_stored += bytes;
	} else {
		vgl_rt_ds_store_skipped += bytes;
		rt_cpu_frame_skipped_bytes += bytes;
		vgl_rt_scenes_nostore++;
		rt_cpu_frame_nostore++;
	}
	return loads ? (hints & ~VGL_RT_NOLOAD) : (hints | VGL_RT_NOLOAD);
}

void vgl_rt_cpu_switch(uint32_t t_end, uint32_t t_begin, uint32_t t_setup) {
	vgl_rt_cpu_end_us += t_end;
	vgl_rt_cpu_begin_us += t_begin;
	vgl_rt_cpu_setup_us += t_setup;
	vgl_rt_cpu_timed++;
	const uint32_t all = t_end + t_begin + t_setup;
	rt_cpu_frame_switch_us += all;
	if (all > vgl_rt_cpu_switch_max_us)
		vgl_rt_cpu_switch_max_us = all;
}

uint32_t vgl_rt_now(void) { return rt_now(); }

// gxm.c scene_end: sceGxmEndScene with this scene's own fragment notification (GPU timers on)
void vgl_rt_end_scene(SceGxmContext *ctx) {
	const uint32_t seq = rt_produced;
	rt_scene_rec *r = &rt_ring[seq % RT_SCENES];
	r->frame = rt_frame;
	r->done_us = 0;
	// The watcher must not fall a whole slot ring behind: a slot word is rewritten RT_SLOTS scenes later (a wait for the old value would never end).
	if (rt_slots && seq - rt_consumed < RT_SLOTS - 16) {
		SceGxmNotification n;
		n.address = &rt_slots[seq % RT_SLOTS];
		n.value = seq + 1;
		*n.address = 0;
		sceGxmEndScene(ctx, NULL, &n);
		r->timed = 1;
	} else {
		sceGxmEndScene(ctx, NULL, &query_fence);
		r->timed = 0;
	}
	r->submit_us = rt_now();
	__sync_synchronize();
	rt_produced = seq + 1;
	if (rt_watcher_idle) {
		rt_watcher_idle = 0;
		sceKernelSignalSema(rt_sema, 1);
	}
}

// gxm.c vglSwapBuffers, after the frame's last scene was ended
void vgl_rt_frame_end(void) {
	vglRtCpuFrame *f = &rt_cpu_frames[rt_cpu_frame_head % RT_FRAMES];
	f->switch_us = rt_cpu_frame_switch_us;
	f->scenes = rt_cpu_frame_scenes;
	f->noload = rt_cpu_frame_noload;
	f->nostore = rt_cpu_frame_nostore;
	f->skipped_kib = (uint32_t)(rt_cpu_frame_skipped_bytes >> 10);
	__sync_synchronize();
	rt_cpu_frame_head++;
	rt_cpu_frame_switch_us = rt_cpu_frame_scenes = rt_cpu_frame_noload = rt_cpu_frame_nostore = 0;
	rt_cpu_frame_skipped_bytes = 0;
	rt_frame++;
}

static int rt_watcher(SceSize args, void *argp) {
	uint32_t cur = 0xFFFFFFFF, first_submit = 0, last_submit = 0, last_done = 0, prev_done = 0, busy = 0, scenes = 0, timed = 0;
	GLboolean have_prev = GL_FALSE;
	for (;;) {
		uint32_t produced = rt_produced;
		if (rt_consumed == produced) {
			rt_watcher_idle = 1;
			__sync_synchronize();
			if (rt_consumed == rt_produced) {
				sceKernelWaitSema(rt_sema, 1, NULL);
			}
			rt_watcher_idle = 0;
			continue;
		}
		__sync_synchronize();
		const uint32_t seq = rt_consumed;
		rt_scene_rec *r = &rt_ring[seq % RT_SCENES];
		if (r->frame != cur) {
			if (cur != 0xFFFFFFFF && scenes) {
				vglRtGpuFrame *f = &rt_gpu_frames[rt_gpu_frame_head % RT_FRAMES];
				f->busy_us = busy;
				f->span_us = timed ? last_done - first_submit : 0;
				f->tail_us = timed ? last_done - last_submit : 0;
				f->scenes = scenes;
				f->timed = timed;
				__sync_synchronize();
				rt_gpu_frame_head++;
			}
			cur = r->frame;
			first_submit = r->submit_us;
			busy = scenes = timed = 0;
		}
		scenes++;
		last_submit = r->submit_us;
		if (r->timed) {
			SceGxmNotification n;
			n.address = &rt_slots[seq % RT_SLOTS];
			n.value = seq + 1;
			if (*n.address != n.value) {
				sceGxmNotificationWait(&n);
			} else {
				rt_gpu_scenes_late++;
			}
			const uint32_t done = rt_now();
			r->done_us = done;
			uint32_t start = r->submit_us;
			if (have_prev && (int32_t)(prev_done - start) > 0)
				start = prev_done;
			if ((int32_t)(done - start) > 0)
				busy += done - start;
			prev_done = done;
			have_prev = GL_TRUE;
			last_done = done;
			timed++;
			rt_gpu_scenes_timed++;
		} else {
			rt_gpu_scenes_untimed++;
		}
		__sync_synchronize();
		rt_consumed = seq + 1;
	}
	return 0;
}

void vglRtSetup(uint32_t flags, int watcher_priority, int watcher_affinity) {
	vgl_rt_flags = flags;
	if (flags & VGL_RT_CPU_TIMERS) {
		const uint32_t t0 = rt_now();
		uint32_t x = 0;
		for (int i = 0; i < 200; i++)
			x += rt_now();
		vgl_rt_clock_cost_us = (float)(rt_now() - t0) / 201.0f;
		(void)x;
	}
	if ((flags & VGL_RT_GPU_TIMERS) && rt_thread < 0) {
		rt_slots = (volatile unsigned int *)sceGxmGetNotificationRegion() + 1;
		for (int i = 0; i < RT_SLOTS; i++)
			rt_slots[i] = 0;
		rt_sema = sceKernelCreateSema("vgl_rt_sema", 0, 0, 0x7FFFFFFF, NULL);
		rt_thread = sceKernelCreateThread("vgl_rt_gpu", rt_watcher, watcher_priority ? watcher_priority : 0x10000100, 0x2000, 0, watcher_affinity, NULL);
		if (rt_sema < 0 || rt_thread < 0 || sceKernelStartThread(rt_thread, 0, NULL) < 0) {
			vgl_log("%s:%d GPU timer thread not started (0x%08X, 0x%08X).\n", __FILE__, __LINE__, rt_sema, rt_thread);
			rt_slots = NULL;
			vgl_rt_flags &= ~VGL_RT_GPU_TIMERS;
		}
	} else if (!(flags & VGL_RT_GPU_TIMERS)) {
		rt_slots = NULL;
	}
}

// The newest frame the GPU watcher has finished (VGL_RT_GPU_TIMERS): 0 when there is none yet, else the count of frames finished so far
// (the caller sees a new frame when it grows). Cheap: one record, no copy of the rings (the renderer's resolution governor reads it per frame).
uint32_t vglRtLastGpuFrame(vglRtGpuFrame *out) {
	const uint32_t h = rt_gpu_frame_head;
	if (!h)
		return 0;
	__sync_synchronize();
	*out = rt_gpu_frames[(h - 1) % RT_FRAMES];
	return h;
}

void vglRtGetStats(vglRtStats *s) {
	s->version = 1;
	s->flags = vgl_rt_flags;
	s->scenes = vgl_rt_scenes;
	s->scenes_display = vgl_rt_scenes_display;
	s->scenes_noload = vgl_rt_scenes_noload;
	s->scenes_noload_auto = vgl_rt_scenes_noload_auto;
	s->scenes_nostore = vgl_rt_scenes_nostore;
	s->hints_given = vgl_rt_hints_given;
	s->hints_used = vgl_rt_hints_used;
	s->ds_loaded = vgl_rt_ds_loaded;
	s->ds_stored = vgl_rt_ds_stored;
	s->ds_load_skipped = vgl_rt_ds_load_skipped;
	s->ds_store_skipped = vgl_rt_ds_store_skipped;
	s->cpu_end_us = vgl_rt_cpu_end_us;
	s->cpu_begin_us = vgl_rt_cpu_begin_us;
	s->cpu_setup_us = vgl_rt_cpu_setup_us;
	s->cpu_timed = vgl_rt_cpu_timed;
	s->cpu_switch_max_us = vgl_rt_cpu_switch_max_us;
	s->clock_cost_us = vgl_rt_clock_cost_us;
	s->mask_invalidations_skipped = vgl_rt_invalidations_skipped;
	s->gpu_scenes_timed = rt_gpu_scenes_timed;
	s->gpu_scenes_untimed = rt_gpu_scenes_untimed;
	s->gpu_scenes_late = rt_gpu_scenes_late;
	s->gpu_pending = rt_produced - rt_consumed;
	s->cpu_frame_head = rt_cpu_frame_head;
	s->gpu_frame_head = rt_gpu_frame_head;
	__sync_synchronize();
	vgl_fast_memcpy(s->cpu_frames, rt_cpu_frames, sizeof(rt_cpu_frames));
	vgl_fast_memcpy(s->gpu_frames, rt_gpu_frames, sizeof(rt_gpu_frames));
}
