/*
 * This file is part of vitaGL
 * Copyright 2017, 2018, 2019, 2020 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* 
 * gxm.c:
 * Implementation for setup and cleanup for sceGxm specific stuffs
 */

#include "shared.h"

#define MAX_SCENES_PER_FRAME 8 // Maximum amount of scenes per frame allowed by sceGxm per render target

// FIXME: Since we use our own default uniform buffers circular pool, fragment and vertex buffer rings can likely be reduced in size
uint32_t gxm_param_buf_size = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE; // Param buffer size for sceGxm
static uint32_t gxm_vdm_buf_size = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE; // VDM ring buffer size for sceGxm
static uint32_t gxm_vertex_buf_size = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE; // Vertex ring buffer size for sceGxm
static uint32_t gxm_fragment_buf_size = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE; // Fragment ring buffer size for sceGxm
static uint32_t gxm_usse_buf_size = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE; // Fragment ring buffer size for sceGxm

// Shader Patcher memory configuration
static unsigned int shader_patcher_buffer_size = 1024 * 1024;
static unsigned int shader_patcher_vertex_usse_size = 1024 * 1024;
static unsigned int shader_patcher_fragment_usse_size = 1024 * 1024;

void *context_host_mem[GXM_CONTEXTS_NUM]; // Context host memory memblock starting address
void *vdm_ring_buffer_addr[GXM_CONTEXTS_NUM]; // VDM ring buffer memblock starting address
void *vertex_ring_buffer_addr[GXM_CONTEXTS_NUM]; // vertex ring buffer memblock starting address
void *fragment_ring_buffer_addr[GXM_CONTEXTS_NUM]; // fragment ring buffer memblock starting address
void *fragment_usse_ring_buffer_addr[GXM_CONTEXTS_NUM]; // fragment USSE ring buffer memblock starting address

SceGxmRenderTarget *gxm_render_target; // Display render target
SceGxmColorSurface gxm_color_surfaces[DISPLAY_MAX_BUFFER_COUNT]; // Display color surfaces
uint8_t gxm_display_buffer_count = 3; // Display buffers count
void *gxm_color_surfaces_addr[DISPLAY_MAX_BUFFER_COUNT]; // Display color surfaces memblock starting addresses
SceGxmSyncObject *gxm_sync_objects[DISPLAY_MAX_BUFFER_COUNT]; // Display sync objects
unsigned int gxm_front_buffer_index = 0; // Display front buffer id
unsigned int gxm_back_buffer_index; // Display back buffer id
static void (*vgl_display_cb)(void *framebuf) = NULL; // Additional custom callback used inside display queue callback

static void *gxm_shader_patcher_buffer_addr; // Shader PAtcher buffer memblock starting address
static void *gxm_shader_patcher_vertex_usse_addr; // Shader Patcher vertex USSE memblock starting address
static void *gxm_shader_patcher_fragment_usse_addr; // Shader Patcher fragment USSE memblock starting address

SceGxmDepthStencilSurface gxm_depth_stencil_surface; // Depth/Stencil surfaces setup for sceGxm
// 0042: the display's depth/stencil surface made without memory (vglSetDisplayNoDepth), a real one only while a common dialog is up
static GLboolean vgl_disp_nodepth = GL_FALSE;
static GLboolean vgl_disp_real = GL_FALSE;     // the display surface currently has memory
static uint32_t vgl_disp_quiet = 0;            // swaps since the last one with a dialog
static uint32_t vgl_disp_made = 0, vgl_disp_freed = 0, vgl_disp_failed = 0;
#define VGL_DISP_QUIET_SWAPS 90
// 0042: parameter buffer in main memory (SCE_GXM_INITIALIZE_FLAG_PB_LPDDR) for a normal application: 0 not asked, 1 taken, -1 refused (fell back)
static GLboolean vgl_pb_lpddr_want = GL_FALSE;
// 0049: display queue pending count (0 = vitaGL's own, buffers - 1) and the time sceGxmDisplayQueueAddEntry takes (off by default: no clock read)
static uint32_t vgl_dq_pending_want = 0;
static GLboolean vgl_dq_timing = GL_FALSE;
static uint32_t vgl_dq_last_us = 0, vgl_dq_max_us = 0, vgl_dq_adds = 0, vgl_dq_slow = 0, vgl_dq_pending_used = 0;
static uint64_t vgl_dq_total_us = 0;
int vgl_pb_lpddr_state = 0;
int vgl_pb_lpddr_rc = 0;
uint32_t vgl_pb_lpddr_shrink = 0;

static SceUID shared_fb; // In-use hared framebuffer identifier
static SceSharedFbInfo shared_fb_info; // In-use shared framebuffer info struct
framebuffer *in_use_framebuffer = NULL; // Currently in use framebuffer
framebuffer *old_framebuffer = NULL; // Framebuffer used in last scene
GLboolean dirty_framebuffer = GL_FALSE; // Flag whether current in use framebuffer is invalidated
GLboolean dirty_query = GL_FALSE; // Flag whether occlusion queries needs results
static GLboolean needs_end_scene = GL_FALSE; // Flag for gxm end scene requirement at scene reset
static GLboolean needs_scene_reset = GL_TRUE; // Flag for when a scene reset is required
static GLboolean scene_not_flushed = GL_FALSE; // Flag for when a scene had been forcibly flushed as last operation
// 1.2 robust scene entry (vglSetRobustScenes, default on): see scene_reset
GLboolean vgl_robust_scenes = GL_TRUE;
static GLboolean scene_fail_valid = GL_FALSE; // the last scene entry that failed: for which framebuffer and in which frame
static framebuffer *scene_fail_fb = NULL;
static uint32_t scene_fail_frame = 0;
static uint32_t vgl_scene_failures = 0;
uint32_t vgl_shader_patch_failures = 0; // shared.h patch_vertex_program / patch_fragment_program
uint32_t vgl_patch_calls[3] = {0, 0, 0};
uint64_t vgl_patch_us[3] = {0, 0, 0};
uint32_t vgl_patch_max_us[3] = {0, 0, 0};
// 1.3 wave 10 (0035): the counts, total and longest microseconds of the patcher's vertex and fragment program creates and of sceGxmCreateRenderTarget since the last call (all three arrays of 3), then reset.
void vglPatchTimingTake(uint32_t *calls, uint64_t *us, uint32_t *max_us) {
	for (int i = 0; i < 3; i++) {
		calls[i] = vgl_patch_calls[i];
		us[i] = vgl_patch_us[i];
		max_us[i] = vgl_patch_max_us[i];
		vgl_patch_calls[i] = 0;
		vgl_patch_us[i] = 0;
		vgl_patch_max_us[i] = 0;
	}
}
uint32_t vgl_bt_calls[3] = {0, 0, 0};
uint64_t vgl_bt_us[3] = {0, 0, 0};
uint32_t vgl_bt_max_us[3] = {0, 0, 0};
// 1.3 wave 11 (0040): shader registrations, glLinkProgram calls and first draws of newly linked programs since the last call (counts, total and longest microseconds, three of each), then reset.
void vglBackendTimingTake(uint32_t *calls, uint64_t *us, uint32_t *max_us) {
	for (int i = 0; i < 3; i++) {
		calls[i] = vgl_bt_calls[i];
		us[i] = vgl_bt_us[i];
		max_us[i] = vgl_bt_max_us[i];
		vgl_bt_calls[i] = 0;
		vgl_bt_us[i] = 0;
		vgl_bt_max_us[i] = 0;
	}
}
void vglSetRobustScenes(uint32_t enabled) { vgl_robust_scenes = enabled ? GL_TRUE : GL_FALSE; }
uint32_t vglRobustFailures(uint32_t *scene_begins, uint32_t *shader_patches) {
	if (scene_begins)
		*scene_begins = vgl_scene_failures;
	if (shader_patches)
		*shader_patches = vgl_shader_patch_failures;
	return vgl_scene_failures + vgl_shader_patch_failures;
}

SceGxmContext *gxm_context; // sceGxm context instance
GLenum vgl_error = GL_NO_ERROR; // Error returned by glGetError
SceGxmShaderPatcher *gxm_shader_patcher; // sceGxmShaderPatcher shader patcher instance
GLboolean is_fbo_float = GL_FALSE; // Current framebuffer mode
SceGxmMultisampleMode cur_msaa = SCE_GXM_MULTISAMPLE_NONE; // Current framebuffer's multisample mode (the display's: msaa_mode)

#ifndef STORE_DEPTH_STENCIL
extern GLboolean skip_scissor_invalidation; // Flag whether to skip mask update bit invalidation pass during scissor test region update
#endif

#ifdef HAVE_PROFILING
uint32_t frame_profiler_cnt = 0;
uint32_t ffp_draw_profiler_cnt = 0;
uint32_t ffp_reload_profiler_cnt = 0;
uint32_t shaders_draw_profiler_cnt = 0;
uint32_t ffp_draw_cnt = 0;
uint32_t shaders_draw_cnt = 0;
static uint32_t gpu_stall_cnt = 0;
#endif

extern void _glFramebufferTexture2D(framebuffer *fb, GLenum attachment, texture *tex, GLuint tex_id);

int DISPLAY_WIDTH; // Display width in pixels
int DISPLAY_HEIGHT; // Display height in pixels
int DISPLAY_STRIDE; // Display stride in pixels
int NEW_DISPLAY_WIDTH; // Requested new display width in pixels
int NEW_DISPLAY_HEIGHT; // Requested new display height in pixels
float DISPLAY_WIDTH_FLOAT; // Display width in pixels (float)
float DISPLAY_HEIGHT_FLOAT; // Display height in pixels (float)

GLboolean system_app_mode = GL_FALSE; // Flag for system app mode usage
static GLboolean gxm_initialized = GL_FALSE; // Current sceGxm state
GLboolean is_rendering_display = GL_TRUE; // Flag for when drawing without fbo is being performed

float *legacy_pool = NULL; // Mempool for GL1 immediate draw pipeline
float *legacy_pool_ptr = NULL; // Current address for vertices population for GL1 immediate draw pipeline
#ifndef SKIP_ERROR_HANDLING
float *legacy_pool_end = NULL; // Address of the end of the GL1 immediate draw pipeline vertex pool
#endif
uint32_t vgl_framecount = 0; // Current frame number since application started

void *frame_purge_list[FRAME_PURGE_FREQ][FRAME_PURGE_LIST_SIZE]; // Purge list for internal elements
void *frame_rt_purge_list[FRAME_PURGE_FREQ][FRAME_PURGE_RENDERTARGETS_LIST_SIZE]; // Purge list for rendertargets
int frame_purge_idx = 0; // Index for currently populatable purge list
int frame_elem_purge_idx = 0; // Index for currently populatable purge list element
int frame_rt_purge_idx = 0; // Index for currently populatable purge list rendertarget
static int frame_purge_clean_idx = 1;
SceUID gc_mutex[2] = {-1, -1};
#include <psp2/kernel/error.h>
// 0064 (collector timers, always on, two clock reads per collector job and one per swap): how long a collector job takes, how long its sceGxmDestroyRenderTarget calls take, and how long the render thread
// waited in gc_enqueue for a credit (the collector is behind by FRAME_PURGE_FREQ jobs). The maxima are per read (vglGetGcJobStats takes and clears them); each has one writer thread.
static uint32_t vgl_gcj_jobs = 0, vgl_gcj_destroys = 0, vgl_gcj_destroy_total_us = 0, vgl_gcj_wait_total_us = 0;
static uint32_t vgl_gcj_job_max_us = 0, vgl_gcj_destroy_max_us = 0, vgl_gcj_wait_max_us = 0;
static inline void vgl_gcj_max(uint32_t *slot, uint32_t us) {
	if (us > __atomic_load_n(slot, __ATOMIC_RELAXED))
		__atomic_store_n(slot, us, __ATOMIC_RELAXED);
}
#include "utils/gc_recovery.h"
static int gc_thread_priority = 0x10000100;
static int gc_thread_affinity = 0;
static uint8_t gxm_display_rt_size = 1; // Number of scenes per frame to use for the display rendertarget
static uint8_t gxm_fbo_rt_size = 1; // Number of scenes per frame to use for framebuffers rendertargets

// 0033 scene census (MSC_VITA_SCENE_COUNT): scenes begun per render target in one picture against the scenesPerFrame the target was created with. A scene past the
// capacity waits for the GPU (sceGxmCreateRenderTarget). Off by default; on, one array scan per scene begin on the render thread.
#define SCN_SLOTS 32
static uint8_t scn_on = 0;
static const void *scn_target[SCN_SLOTS];
static uint16_t scn_count[SCN_SLOTS];
static uint32_t scn_used = 0, scn_display = 0;
static vglSceneCensus scn_c;
static inline void scn_begin(const void *target, int display) {
	if (!scn_on)
		return;
	if (display) {
		++scn_display;
		return;
	}
	for (uint32_t i = 0; i < scn_used; i++) {
		if (scn_target[i] == target) {
			++scn_count[i];
			return;
		}
	}
	if (scn_used < SCN_SLOTS) {
		scn_target[scn_used] = target;
		scn_count[scn_used++] = 1;
	} else {
		++scn_c.slots_full;
	}
}
static inline void scn_frame_end(void) {
	if (!scn_on)
		return;
	uint32_t worst = 0;
	for (uint32_t i = 0; i < scn_used; i++) {
		if (scn_count[i] > worst)
			worst = scn_count[i];
		if (scn_count[i] > gxm_fbo_rt_size)
			++scn_c.over_fbo;
	}
	++scn_c.frames;
	if (worst > scn_c.worst_fbo)
		scn_c.worst_fbo = worst;
	if (scn_used > scn_c.worst_fbo_targets)
		scn_c.worst_fbo_targets = scn_used;
	if (scn_display > scn_c.worst_display)
		scn_c.worst_display = scn_display;
	if (scn_display > gxm_display_rt_size)
		++scn_c.over_display;
	scn_used = 0;
	scn_display = 0;
}
void vglSceneCensusSetup(int on) {
	scn_on = on ? 1 : 0;
}
void vglGetSceneCensus(vglSceneCensus *out, int reset) {
	scn_c.cap_display = gxm_display_rt_size;
	scn_c.cap_fbo = gxm_fbo_rt_size;
	*out = scn_c;
	if (reset) {
		vgl_memset(&scn_c, 0, sizeof(scn_c));
	}
}

// 0034 free guard (see vitaGL.h)
int vgl_fg_on = 0, vgl_fg_flushing = 0;
#define FG_RING 256
#define FG_SITES 32
#define FG_EVENTS 32
static struct { uint8_t kind; uint32_t id; uint32_t due; } fg_ring[FG_RING];
static uint32_t fg_ring_n = 0;
static uint32_t fg_hold = 0;
static int fg_ref_only = 0;   // 0047: hold only the deletes that would free a block a scene drew into recently (or the bound framebuffer's colour); every other delete is immediate as before
static uintptr_t fg_site[FG_SITES];
static uint8_t fg_site_kind[FG_SITES];
static vglFreeGuardEvent fg_events[FG_EVENTS];
static uint32_t fg_ev_head = 0, fg_ev_tail = 0;
static vglFreeGuardStats fg_stats;

void vglSetFreeGuard(int presents) {
	fg_hold = presents > 0 ? (uint32_t)presents : 0;
	vgl_fg_on = fg_hold ? 1 : 0;
}
void vglSetFreeGuardReferencedOnly(int on) {
	fg_ref_only = on ? 1 : 0;
}
void vglGetFreeGuardStats(vglFreeGuardStats *out) {
	*out = fg_stats;
}
int vglFreeGuardTake(vglFreeGuardEvent *out) {
	if (fg_ev_tail == fg_ev_head)
		return 0;
	*out = fg_events[fg_ev_tail % FG_EVENTS];
	++fg_ev_tail;
	return 1;
}
static void fg_report(int kind, uint32_t id, uint32_t reason, uint32_t age, void *caller) {
	for (uint32_t i = 0; i < fg_stats.sites; i++)
		if (fg_site[i] == (uintptr_t)caller && fg_site_kind[i] == kind)
			return;
	if (fg_stats.sites < FG_SITES) {
		fg_site[fg_stats.sites] = (uintptr_t)caller;
		fg_site_kind[fg_stats.sites++] = (uint8_t)kind;
	}
	if (fg_ev_head - fg_ev_tail < FG_EVENTS) {
		vglFreeGuardEvent *e = &fg_events[fg_ev_head++ % FG_EVENTS];
		e->kind = kind; e->id = id; e->reason = reason; e->age = age; e->caller = (uint32_t)(uintptr_t)caller; e->frame = vgl_framecount;
	}
}
// Called by glDeleteTextures / glDeleteFramebuffers with the guard on. 1: the names were taken (deleted later); 0: do it now (ring full).
int vgl_fg_delete(int kind, GLsizei n, const GLuint *ids, void *caller) {
	if (n <= 0 || !ids)
		return 0;
	if (fg_ring_n + (uint32_t)n > FG_RING) {
		++fg_stats.ring_full;
		return 0;
	}
	if (fg_ref_only) { // 0047: nothing in this call was drawn into lately: delete now, exactly as without the guard (a call with one such name holds all of its names)
		int any = 0;
		for (GLsizei j = 0; j < n && !any; j++) {
			const GLuint id = ids[j];
			texture *tex = NULL;
			if (kind == 0) {
				if (id > 0 && id < TEXTURES_NUM && texture_slots[id].status == TEX_VALID) {
					tex = &texture_slots[id];
					// (0050: the bound framebuffer's colour alone does not hold a delete here: nothing may have been drawn into it; only the scene-begin stamp below does)
				}
			} else {
				framebuffer *fb = (framebuffer *)id;
				if (fb && fb->active) {
					tex = fb->tex;
				}
			}
			if (tex && tex->fg_rt_frame && vgl_framecount + 1 - tex->fg_rt_frame <= FRAME_PURGE_FREQ) any = 1;
		}
		if (!any)
			return 0;
	}
	for (GLsizei j = 0; j < n; j++) {
		const GLuint id = ids[j];
		uint32_t reason = 0, age = 0;
		texture *tex = NULL;
		if (kind == 0) {
			if (id > 0 && id < TEXTURES_NUM && texture_slots[id].status == TEX_VALID) {
				tex = &texture_slots[id];
				if (active_write_fb && active_write_fb->tex == tex) reason |= 2;
				if (active_read_fb && active_read_fb->tex == tex) reason |= 2;
			}
		} else {
			framebuffer *fb = (framebuffer *)id;
			if (fb && fb->active) {
				tex = fb->tex;
				if (fb == active_write_fb || fb == active_read_fb) reason |= 2;
			}
		}
		if (tex && tex->fg_rt_frame && vgl_framecount + 1 - tex->fg_rt_frame <= FRAME_PURGE_FREQ) {
			reason |= 1;
			age = vgl_framecount + 1 - tex->fg_rt_frame;
		}
		++fg_stats.deletes;
		if (reason) {
			++fg_stats.referenced;
			fg_report(kind, id, reason, age, caller);
		}
		fg_ring[fg_ring_n].kind = (uint8_t)kind;
		fg_ring[fg_ring_n].id = id;
		fg_ring[fg_ring_n++].due = vgl_framecount + fg_hold;
		++fg_stats.postponed;
	}
	return 1;
}
// vglSwapBuffers: the deletes that are due
static void fg_flush(void) {
	if ((uint32_t)frame_elem_purge_idx > fg_stats.purge_peak)
		fg_stats.purge_peak = (uint32_t)frame_elem_purge_idx;
	if ((uint32_t)frame_rt_purge_idx > fg_stats.rt_purge_peak)
		fg_stats.rt_purge_peak = (uint32_t)frame_rt_purge_idx;
	if (!fg_ring_n)
		return;
	vgl_fg_flushing = 1;
	uint32_t keep = 0;
	for (uint32_t i = 0; i < fg_ring_n; i++) {
		if ((int32_t)(fg_ring[i].due - vgl_framecount) <= 0) {
			GLuint id = fg_ring[i].id;
			if (fg_ring[i].kind == 0)
				glDeleteTextures(1, &id);
			else
				glDeleteFramebuffers(1, &id);
		} else {
			fg_ring[keep++] = fg_ring[i];
		}
	}
	fg_ring_n = keep;
	vgl_fg_flushing = 0;
}
// 0050: the held deletes sit outside the collector, so the allocation recovery (0036) could not reclaim them while memory is full. It calls this after its GPU finish (nothing is
// drawing any more): every held delete is done now and the collector cycle that follows frees them.
void vgl_fg_flush_all(void) {
	if (!fg_ring_n || vgl_fg_flushing)
		return;
	vgl_fg_flushing = 1;
	for (uint32_t i = 0; i < fg_ring_n; i++) {
		GLuint id = fg_ring[i].id;
		if (fg_ring[i].kind == 0)
			glDeleteTextures(1, &id);
		else
			glDeleteFramebuffers(1, &id);
	}
	fg_stats.recovery_flushed += fg_ring_n;
	fg_ring_n = 0;
	vgl_fg_flushing = 0;
}
#ifdef HAVE_PTHREAD
pthread_t gc_thread;
#else
SceUID gc_thread;
#endif

#ifndef SKIP_SPLASHSCREEN
extern SceUID splash_mutex[2];
extern GLboolean is_splashscreen_active;
#endif

#ifdef HAVE_CPU_TRACER
int sceRazorCpuSync();
#endif

#ifdef HAVE_RAZOR
#define RAZOR_BUF_SIZE (1024 * 1024) // Size in bytes for a live metrics data buffer
#define UPDATE_RATIO 30 // Number of frames between two live metrics updates
#ifndef HAVE_DEVKIT
#define RAZOR_CAPTURE_MOD_PATH "ur0:data/librazorcapture_es4.suprx"
SceUID razor_modid;
#endif

typedef union {
	SceRazorGpuLiveEntryJob *job;
	SceRazorGpuLiveEntryParameterBuffer *pbuf;
	SceRazorGpuLiveEntryFrame *frame;
	uintptr_t ptr;
} SceRazorGpuResult;

uint8_t *razor_buf[DISPLAY_MAX_BUFFER_COUNT]; // Buffers used to store live metrics data
razor_results razor_metrics;

GLboolean has_razor_live = GL_FALSE; // Flag for live metrics support with sceRazor
#endif

static inline __attribute__((always_inline)) int setup_render_target_ms(SceGxmRenderTarget **rt, int w, int h, int refs, SceGxmMultisampleMode mode) {
	SceGxmRenderTargetParams renderTargetParams;
	vgl_memset(&renderTargetParams, 0, sizeof(SceGxmRenderTargetParams));
	renderTargetParams.width = w ? w : 1;
	renderTargetParams.height = h ? h : 1;
	renderTargetParams.scenesPerFrame = refs;
	renderTargetParams.multisampleMode = mode;
	renderTargetParams.driverMemBlock = -1;
	uint64_t t0 = sceKernelGetProcessTimeWide();
	int r = sceGxmCreateRenderTarget(&renderTargetParams, rt);
	vgl_patch_note(2, t0);
	return r;
}

static inline __attribute__((always_inline)) int setup_render_target(SceGxmRenderTarget **rt, int w, int h, int refs) {
	return setup_render_target_ms(rt, w, h, refs, msaa_mode);
}

// 0039 (MSC_VGX_RT_POOL): a framebuffer object's render target is kept when the object lets it go and handed to the next object that asks for the same (width, height, scenes per frame,
// samples), instead of sceGxmDestroyRenderTarget there and sceGxmCreateRenderTarget here (40-95 ms an object on the console, estimate). Only the driver's object is kept: the colour and depth
// memory follow their own rules. A target is pooled by the garbage collector, which runs it only when the GPU is done with the frames that used it. Off (vglSetRenderTargetPool(0, 0)): no
// target is tracked or kept and nothing here runs but one compare. Exact sizes only (rounding up to 64 is not done: whether a larger target serves a smaller scene is not known).
#define VGL_RTP_SLOTS 48
typedef struct {
	SceGxmRenderTarget *rt;
	int w, h, refs;
	SceGxmMultisampleMode mode;
	uint32_t kib; // the driver's memory for it (sceGxmGetRenderTargetMemSize), at least 1
	uint8_t state; // 0 free, 1 handed to a framebuffer object, 2 kept
} vgl_rtp_slot;
static vgl_rtp_slot vgl_rtp[VGL_RTP_SLOTS];
static volatile int vgl_rtp_lock = 0;
static volatile uint32_t vgl_rtp_max_count = 0, vgl_rtp_max_kib = 0, vgl_rtp_any = 0;
static uint32_t vgl_rtp_stat[6] = {0, 0, 0, 0, 0, 0}; // hits, creates (missed or not pooled), kept, let go (over the cap or off), kept now, KiB kept now
#define VGL_RTP_LOCK() while (__sync_lock_test_and_set(&vgl_rtp_lock, 1)) { sceKernelDelayThread(100); } /* yield: a higher-priority spinner must let a lower-priority holder run (fix16) */
#define VGL_RTP_UNLOCK() __sync_lock_release(&vgl_rtp_lock)

// count 0: off (every kept target is destroyed at once); else at most `count` kept targets and `kib` KiB of driver memory (0: no KiB cap)
void vglSetRenderTargetPool(uint32_t count, uint32_t kib) {
	SceGxmRenderTarget *gone[VGL_RTP_SLOTS];
	int n = 0;
	VGL_RTP_LOCK()
	__atomic_store_n(&vgl_rtp_max_count, count > VGL_RTP_SLOTS ? VGL_RTP_SLOTS : count, __ATOMIC_RELEASE);
	vgl_rtp_max_kib = kib;
	if (!vgl_rtp_max_count) {
		for (int i = 0; i < VGL_RTP_SLOTS; i++)
			if (vgl_rtp[i].state == 2) {
				gone[n++] = vgl_rtp[i].rt;
				vgl_rtp[i].state = 0;
			}
		vgl_rtp_stat[3] += n;
		vgl_rtp_stat[4] = 0;
		vgl_rtp_stat[5] = 0;
	} else {
		for (int i = 0; i < VGL_RTP_SLOTS && (vgl_rtp_stat[4] > vgl_rtp_max_count || (vgl_rtp_max_kib && vgl_rtp_stat[5] > vgl_rtp_max_kib)); i++)
			if (vgl_rtp[i].state == 2) {
				gone[n++] = vgl_rtp[i].rt;
				vgl_rtp[i].state = 0;
				vgl_rtp_stat[3]++;
				vgl_rtp_stat[4]--;
				vgl_rtp_stat[5] -= vgl_rtp[i].kib;
			}
	}
	VGL_RTP_UNLOCK();
	for (int i = 0; i < n; i++)
		sceGxmDestroyRenderTarget(gone[i]);
}
void vglGetRenderTargetPoolStats(uint32_t *hits, uint32_t *creates, uint32_t *kept, uint32_t *let_go, uint32_t *kept_now, uint32_t *kib_now) {
	uint32_t s[6];
	VGL_RTP_LOCK()
	for (int i = 0; i < 6; i++)
		s[i] = vgl_rtp_stat[i];
	VGL_RTP_UNLOCK();
	if (hits) *hits = s[0];
	if (creates) *creates = s[1];
	if (kept) *kept = s[2];
	if (let_go) *let_go = s[3];
	if (kept_now) *kept_now = s[4];
	if (kib_now) *kib_now = s[5];
}

// 0064: the collector's figures since the last call (the three maxima are of the time since the last call and are cleared): jobs and destroys so far, the longest job, the longest single
// sceGxmDestroyRenderTarget, the longest wait of the render thread for a collector credit, and the totals of the destroys' and of the waits' time (microseconds, wrapping at 2^32).
void vglGetGcJobStats(uint32_t *jobs, uint32_t *destroys, uint32_t *job_max_us, uint32_t *destroy_max_us, uint32_t *wait_max_us, uint32_t *destroy_total_us, uint32_t *wait_total_us) {
	if (jobs) *jobs = __atomic_load_n(&vgl_gcj_jobs, __ATOMIC_RELAXED);
	if (destroys) *destroys = __atomic_load_n(&vgl_gcj_destroys, __ATOMIC_RELAXED);
	if (job_max_us) *job_max_us = __atomic_exchange_n(&vgl_gcj_job_max_us, 0, __ATOMIC_RELAXED);
	if (destroy_max_us) *destroy_max_us = __atomic_exchange_n(&vgl_gcj_destroy_max_us, 0, __ATOMIC_RELAXED);
	if (wait_max_us) *wait_max_us = __atomic_exchange_n(&vgl_gcj_wait_max_us, 0, __ATOMIC_RELAXED);
	if (destroy_total_us) *destroy_total_us = __atomic_load_n(&vgl_gcj_destroy_total_us, __ATOMIC_RELAXED);
	if (wait_total_us) *wait_total_us = __atomic_load_n(&vgl_gcj_wait_total_us, __ATOMIC_RELAXED);
}

// a framebuffer object's render target: a kept one of the same shape, else a new one (tracked so that the collector can keep it when it is let go)
static int setup_fbo_render_target_ms(SceGxmRenderTarget **rt, int w, int h, int refs, SceGxmMultisampleMode mode) {
	if (!__atomic_load_n(&vgl_rtp_max_count, __ATOMIC_ACQUIRE))
		return setup_render_target_ms(rt, w, h, refs, mode);
	VGL_RTP_LOCK()
	for (int i = 0; i < VGL_RTP_SLOTS; i++) {
		vgl_rtp_slot *s = &vgl_rtp[i];
		if (s->state == 2 && s->w == w && s->h == h && s->refs == refs && s->mode == mode) {
			s->state = 1;
			*rt = s->rt;
			vgl_rtp_stat[0]++;
			vgl_rtp_stat[4]--;
			vgl_rtp_stat[5] -= s->kib;
			VGL_RTP_UNLOCK();
			return 0;
		}
	}
	VGL_RTP_UNLOCK();
	int r = setup_render_target_ms(rt, w, h, refs, mode);
	if (r)
		return r;
	SceGxmRenderTargetParams params;
	vgl_memset(&params, 0, sizeof(params));
	params.width = w ? w : 1;
	params.height = h ? h : 1;
	params.scenesPerFrame = refs;
	params.multisampleMode = mode;
	params.driverMemBlock = -1;
	unsigned int drv = 0;
	if (sceGxmGetRenderTargetMemSize(&params, &drv))
		drv = 0;
	VGL_RTP_LOCK()
	vgl_rtp_stat[1]++;
	for (int i = 0; i < VGL_RTP_SLOTS; i++)
		if (vgl_rtp[i].state == 0) {
			vgl_rtp[i].rt = *rt;
			vgl_rtp[i].w = w;
			vgl_rtp[i].h = h;
			vgl_rtp[i].refs = refs;
			vgl_rtp[i].mode = mode;
			vgl_rtp[i].kib = drv ? (drv + 1023) / 1024 : 1;
			vgl_rtp[i].state = 1;
			__atomic_store_n(&vgl_rtp_any, 1, __ATOMIC_RELEASE);
			break;
		}
	VGL_RTP_UNLOCK();
	return 0;
}

// the garbage collector's end of a framebuffer object's render target (the GPU is done with it): kept when the pool has room, else destroyed
static void vgl_rtp_release(SceGxmRenderTarget *rt) {
	if (__atomic_load_n(&vgl_rtp_any, __ATOMIC_ACQUIRE)) {
		VGL_RTP_LOCK()
		for (int i = 0; i < VGL_RTP_SLOTS; i++) {
			vgl_rtp_slot *s = &vgl_rtp[i];
			if (s->state != 1 || s->rt != rt)
				continue;
			if (vgl_rtp_stat[4] < vgl_rtp_max_count && (!vgl_rtp_max_kib || vgl_rtp_stat[5] + s->kib <= vgl_rtp_max_kib)) {
				s->state = 2;
				vgl_rtp_stat[2]++;
				vgl_rtp_stat[4]++;
				vgl_rtp_stat[5] += s->kib;
				VGL_RTP_UNLOCK();
				return;
			}
			s->state = 0;
			vgl_rtp_stat[3]++;
			break;
		}
		VGL_RTP_UNLOCK();
	}
	const uint32_t gcj_t0 = sceKernelGetProcessTimeLow(); // 0064
	sceGxmDestroyRenderTarget(rt);
	{
		const uint32_t gcj_us = sceKernelGetProcessTimeLow() - gcj_t0;
		__atomic_add_fetch(&vgl_gcj_destroys, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&vgl_gcj_destroy_total_us, gcj_us, __ATOMIC_RELAXED);
		vgl_gcj_max(&vgl_gcj_destroy_max_us, gcj_us);
	}
}

// 0063 (vglFramebufferDualRT): a framebuffer object with dual_rt keeps a second render target (target2). A render target holds the scene slots (scenesPerFrame of them) and not the colour or depth
// memory: those are passed to sceGxmBeginScene with every scene. The pictures of even frame number begin their scenes on ->target, those of odd frame number on ->target2, so two pictures in
// flight never share the slots of one target (16 slots, not 8, for the object). Nothing else changes: same surfaces, same draws, the GPU runs the scenes of one context in order. Off (dual_rt 0, the
// library's default): the pointer picked is always ->target. A second target that cannot be made falls back to ->target (the object is then as it was).
static uint32_t vgl_dual_made = 0, vgl_dual_kib = 0, vgl_dual_odd = 0, vgl_dual_fail = 0;
#ifndef HAVE_SHARED_RENDERTARGETS
static void vgl_dual_make(framebuffer *fb) {
	if (fb->target2 || !fb->target)
		return;
	if (setup_fbo_render_target_ms(&fb->target2, fb->width, fb->height, gxm_fbo_rt_size, FB_MSAA(fb))) {
		fb->target2 = NULL;
		vgl_dual_fail++;
		return;
	}
	SceGxmRenderTargetParams params;
	vgl_memset(&params, 0, sizeof(params));
	params.width = fb->width ? fb->width : 1;
	params.height = fb->height ? fb->height : 1;
	params.scenesPerFrame = gxm_fbo_rt_size;
	params.multisampleMode = FB_MSAA(fb);
	params.driverMemBlock = -1;
	unsigned int drv = 0;
	if (!sceGxmGetRenderTargetMemSize(&params, &drv))
		vgl_dual_kib += (drv + 1023) / 1024;
	vgl_dual_made++;
}
#endif
static inline SceGxmRenderTarget *vgl_dual_pick(framebuffer *fb, uint32_t frame) {
#ifndef HAVE_SHARED_RENDERTARGETS
	if (fb->dual_rt && (frame & 1)) {
		if (!fb->target2)
			vgl_dual_make(fb);
		if (fb->target2) {
			vgl_dual_odd++;
			return fb->target2;
		}
	}
#endif
	return fb->target;
}
void vglGetDualRTStats(uint32_t *made, uint32_t *kib, uint32_t *odd_scenes, uint32_t *failed) {
	if (made) *made = vgl_dual_made;
	if (kib) *kib = vgl_dual_kib;
	if (odd_scenes) *odd_scenes = vgl_dual_odd;
	if (failed) *failed = vgl_dual_fail;
}

#ifdef HAVE_SHARED_RENDERTARGETS
#define MAX_RENDER_TARGETS_NUM 47 // Maximum amount of dedicated render targets usable for fbos
#define MAX_SHARED_RT_SIZE 256 // Maximum  width value in pixels for shared rendertargets usage
render_target rt_list[MAX_RENDER_TARGETS_NUM];

render_target *get_free_render_target(int w, int h) {
	int i;
	for (i = 0; i < MAX_RENDER_TARGETS_NUM; i++) {
		if (rt_list[i].rt != NULL) {
			if (w == rt_list[i].w && h == rt_list[i].h && rt_list[i].ref_count < rt_list[i].max_refs) {
				rt_list[i].ref_count++;
				return &rt_list[i];
			}
		} else {
			rt_list[i].max_refs = w > MAX_SHARED_RT_SIZE ? 1 : MAX_SCENES_PER_FRAME;
			int r = setup_render_target(&rt_list[i].rt, w, h, rt_list[i].max_refs);
#ifdef LOG_ERRORS
			if (r)
				vgl_log("%s:%d Failed to create a shared rendertarget of size %dx%d (%s).\n", __FILE__, __LINE__, w, h, get_gxm_error_literal(r));
#endif
			rt_list[i].w = w;
			rt_list[i].h = h;
			rt_list[i].ref_count = 1;
			return &rt_list[i];
		}
	}
#ifdef RECYCLE_RENDERTARGETS
	vgl_log("%s:%d Out of rendertargets handles: Recycling an old rendertarget.\n", __FILE__, __LINE__);
	uint32_t oldest_framecount = 0xFFFFFFFF;
	render_target *r = NULL;
	for (i = 0; i < MAX_RENDER_TARGETS_NUM; i++) {
		if (rt_list[i].last_frame < oldest_framecount) {
			oldest_framecount = rt_list[i].last_frame;
			r = &rt_list[i];
		}
	}
	sceGxmFinish(gxm_context);
	sceGxmDestroyRenderTarget(r->rt);
	r->max_refs = w > MAX_SHARED_RT_SIZE ? 1 : MAX_SCENES_PER_FRAME;
	int res = setup_render_target(&r->rt, w, h, r->max_refs);
#ifdef LOG_ERRORS
	if (res)
		vgl_log("%s:%d Failed to create a shared rendertarget of size %dx%d (%s).\n", __FILE__, __LINE__, w, h, get_gxm_error_literal(res));
#endif
	r->w = w;
	r->h = h;
	r->ref_count = 1;
	return r;
#else
#ifdef LOG_ERRORS
	vgl_log("%s:%d Failed to create a shared rendertarget of size %dx%d (Out of rendertargets handles).\n", __FILE__, __LINE__, w, h);
#endif
	return NULL;
#endif
}

void __mark_rt_as_dirty(render_target *rt) {
	rt->ref_count--;
	if (!rt->ref_count) {
		_mark_rt_as_dirty(rt->rt);
		rt->rt = NULL;
	}
}
#endif

// sceGxmShaderPatcher custom allocator
static void *shader_patcher_host_alloc_cb(void *user_data, unsigned int size) {
	return vglMalloc(size);
}

// sceGxmShaderPatcher custom deallocator
static void shader_patcher_host_free_cb(void *user_data, void *mem) {
	vgl_free(mem);
}

// sceDisplay callback
extern uint64_t NpDiagDisplayTrace(unsigned stage, uint64_t token, void *framebuffer,
                                 uint32_t interval, int32_t result)
#if defined(__APPLE__)
    __attribute__((weak_import));
#else
    __attribute__((weak));
#endif

static void display_trace(unsigned stage, uint64_t token, void *framebuffer, uint32_t interval, int result) {
	if (token && NpDiagDisplayTrace)
		NpDiagDisplayTrace(stage, token, framebuffer, interval, result);
}

static void display_wait(uint32_t interval, uint64_t token, void *framebuffer) {
	display_trace(3, token, framebuffer, interval, 0);
	const int result = sceDisplayWaitVblankStartMulti(interval);
	display_trace(4, token, framebuffer, interval, result);
}

static void display_queue_callback(const void *callbackData) {
	// Populating sceDisplay framebuffer parameters
	SceDisplayFrameBuf display_fb;
	const struct display_queue_callback_data *cb_data = callbackData;
	const uint64_t trace_token = NpDiagDisplayTrace ? NpDiagDisplayTrace(0, 0, cb_data->addr, 0, 0) : 0;
	vgl_memset(&display_fb, 0, sizeof(SceDisplayFrameBuf));
	display_fb.size = sizeof(SceDisplayFrameBuf);
	display_fb.base = cb_data->addr;
	display_fb.pitch = DISPLAY_STRIDE;
	display_fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	display_fb.width = DISPLAY_WIDTH;
	display_fb.height = DISPLAY_HEIGHT;

#ifdef HAVE_DEBUG_INTERFACE
	// Drawing lightweighted debugger info
	vgl_debugger_draw(cb_data->addr);
#endif

	if (vgl_display_cb)
		vgl_display_cb(cb_data->addr);

	// Setting sceDisplay framebuffer
	display_trace(1, trace_token, cb_data->addr, 0, 0);
	const int display_result = sceDisplaySetFrameBuf(&display_fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
	display_trace(2, trace_token, cb_data->addr, 0, display_result);

	// Performing VSync if enabled
	if (vsync_interval)
		display_wait(vsync_interval, trace_token, cb_data->addr);
	display_trace(5, trace_token, cb_data->addr, 0, 0);
}

// Garbage collector
#if defined(HAVE_PTHREAD) && !defined(HAVE_SINGLE_THREADED_GC)
void garbage_collector(void *arg) {
#else
int garbage_collector(unsigned int args, void *arg) {
#endif
#ifndef HAVE_SINGLE_THREADED_GC
	for (;;) {
		// Waiting for garbage collection request
		int wait_result = sceKernelWaitSema(gc_mutex[0], 1, NULL);
		if (wait_result < 0) {
			gc_failure("worker wait", wait_result);
			break;
		}
#endif
		const uint32_t gcj_t0 = sceKernelGetProcessTimeLow(); // 0064
		// Purging all elements marked for deletion
		for (int i = 0; i < FRAME_PURGE_LIST_SIZE; i++) {
			if (frame_purge_list[frame_purge_clean_idx][i]) {
				vgl_free(frame_purge_list[frame_purge_clean_idx][i]);
				frame_purge_list[frame_purge_clean_idx][i] = NULL;
			} else
				break;
		}
		for (int i = 0; i < FRAME_PURGE_RENDERTARGETS_LIST_SIZE; i++) {
			if (frame_rt_purge_list[frame_purge_clean_idx][i]) {
				vgl_rtp_release(frame_rt_purge_list[frame_purge_clean_idx][i]); // 0039 (destroys it unless MSC_VGX_RT_POOL keeps it)
				frame_rt_purge_list[frame_purge_clean_idx][i] = NULL;
			} else
				break;
		}
		frame_purge_clean_idx = (frame_purge_clean_idx + 1) % FRAME_PURGE_FREQ;
		frame_purge_idx = (frame_purge_idx + 1) % FRAME_PURGE_FREQ;
		frame_elem_purge_idx = 0;
		frame_rt_purge_idx = 0;
		{ // 0064
			const uint32_t gcj_us = sceKernelGetProcessTimeLow() - gcj_t0;
			__atomic_add_fetch(&vgl_gcj_jobs, 1, __ATOMIC_RELAXED);
			vgl_gcj_max(&vgl_gcj_job_max_us, gcj_us);
		}
#ifndef HAVE_SINGLE_THREADED_GC
		gc_publish_completion();
	}
#ifndef HAVE_PTHREAD
	return sceKernelExitDeleteThread(0);
#endif
#endif
}

GLboolean start_shader_compiler(void) {
	shark_set_allocators(vglMalloc, vglFree);
	is_shark_online = shark_init(NULL) >= 0;

	// If standard path failed to init we try to init it with ScePiglet path
	if (!is_shark_online) {
		is_shark_online = shark_init("ur0:data/external/libshacccg.suprx") >= 0;
#ifdef LOG_ERRORS
		if (!is_shark_online)
			vgl_log("%s:%d Fatal error: SceShaccCg not found.\n", __FILE__, __LINE__);
#endif
	}

	return is_shark_online;
}

void init_gxm(void) {
	if (gxm_initialized)
		return;

#ifdef HAVE_RAZOR
	// Initializing sceRazor debugger
#ifdef HAVE_DEVKIT
	sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_HUD);
	sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
#else
	razor_modid = sceKernelLoadStartModule(RAZOR_CAPTURE_MOD_PATH, 0, NULL, 0, NULL, NULL);
#endif
#ifdef HAVE_DEVKIT
	for (int i = 0; i < DISPLAY_MAX_BUFFER_COUNT; i++) {
		razor_buf[i] = vglMemalign(8, RAZOR_BUF_SIZE);
	}
#endif

	sceRazorGpuCaptureEnableSalvage("ux0:data/vitagl_gpucrash.sgx");
#endif

	// Initializing runtime shader compiler
	if (start_shader_compiler()) {
#if defined(HAVE_SHARK_LOG) || defined(LOG_ERRORS)
		shark_install_log_cb(shark_log_cb);
		shark_set_warnings_level(SHARK_WARN_HIGH);
#endif
	}

#ifndef HAVE_SINGLE_THREADED_GC
	// Initializing garbage collector
	gc_mutex[0] = sceKernelCreateSema("vitaGL GC Sema Push", 0, 0, FRAME_PURGE_FREQ, NULL);
	gc_mutex[1] = sceKernelCreateSema("vitaGL GC Sema Pull", 0, FRAME_PURGE_FREQ, FRAME_PURGE_FREQ, NULL);
	int gc_start_result = gc_mutex[0] < 0 ? gc_mutex[0] : gc_mutex[1];
	if (gc_start_result >= 0) {
#ifdef HAVE_PTHREAD
		gc_start_result = -pthread_create(&gc_thread, NULL, garbage_collector, NULL);
		if (gc_start_result == 0)
			pthread_setaffinity_np(gc_thread, 4, &gc_thread_affinity);
#else
		gc_thread = sceKernelCreateThread("vitaGL Garbage Collector", &garbage_collector, gc_thread_priority, 0x10000, 0, gc_thread_affinity, NULL);
		gc_start_result = gc_thread < 0 ? gc_thread : sceKernelStartThread(gc_thread, 0, NULL);
#endif
	}
	gc_mark_ready(gc_start_result);
#endif

	// Checking if the running application is a system one
	SceAppMgrBudgetInfo info;
	info.size = sizeof(SceAppMgrBudgetInfo);
	if (!sceAppMgrGetBudgetInfo(&info)) {
		system_app_mode = GL_TRUE;
		gxm_display_buffer_count = 2; // Forcing double buffering in system app mode
		if (msaa_mode == SCE_GXM_MULTISAMPLE_NONE) // FIXME: For some reasons, disabling MSAA makes the shader patcher not able to compile fragment programs in sysapp mode...
			msaa_mode = SCE_GXM_MULTISAMPLE_2X;
	}

	// Initializing sceGxm init parameters
	SceGxmInitializeParams gxm_init_params;
	vgl_memset(&gxm_init_params, 0, sizeof(SceGxmInitializeParams));
	gxm_init_params.flags = SCE_GXM_INITIALIZE_FLAG_EXTENDED_FORMAT;
	if (vgl_pb_lpddr_want && !system_app_mode)
		gxm_init_params.flags |= SCE_GXM_INITIALIZE_FLAG_PB_LPDDR; // 0042 vglSetParamBufferLpddr
	if (system_app_mode) {
		gxm_init_params.flags |= (SCE_GXM_INITIALIZE_FLAG_PB_LPDDR | SCE_GXM_INITIALIZE_FLAG_SHARED_SYNC | SCE_GXM_INITIALIZE_FLAG_SHAREDPB_CREATE | SCE_GXM_INITIALIZE_FLAG_SHAREDPB_OPEN);
	}
	gxm_init_params.displayQueueMaxPendingCount = vgl_dq_pending_want ? vgl_dq_pending_want : gxm_display_buffer_count - 1;
	vgl_dq_pending_used = gxm_init_params.displayQueueMaxPendingCount;
	gxm_init_params.displayQueueCallback = display_queue_callback;
	gxm_init_params.displayQueueCallbackDataSize = sizeof(struct display_queue_callback_data);
	gxm_init_params.parameterBufferSize = gxm_param_buf_size;

	// Initializing sceGxm
	int gxm_init_rc = sceGxmVshInitialize(&gxm_init_params);
	if (gxm_init_rc < 0 && vgl_pb_lpddr_want && !system_app_mode) {
		// 0042: the firmware refused the parameter buffer in main memory for a normal application: go on without it, and tell the caller its
		// video memory pool request (made for a parameter buffer outside video memory) is 'size' too big
		vgl_pb_lpddr_state = -1;
		vgl_pb_lpddr_rc = gxm_init_rc;
		vgl_pb_lpddr_shrink = gxm_param_buf_size;
		gxm_init_params.flags &= ~SCE_GXM_INITIALIZE_FLAG_PB_LPDDR;
		(void)sceGxmVshInitialize(&gxm_init_params);
	} else if (vgl_pb_lpddr_want && !system_app_mode) {
		vgl_pb_lpddr_state = 1;
	}
	gxm_initialized = GL_TRUE;

#ifdef HAVE_DEVKIT
	sceRazorGpuLiveSetMetricsGroup(SCE_RAZOR_GPU_LIVE_METRICS_GROUP_PBUFFER_USAGE);
	has_razor_live = !sceRazorGpuLiveStart();
#endif
}

void init_gxm_context(SceGxmContext **ctx, uint8_t ctx_slot) {
	// Allocating VDM ring buffer
	vdm_ring_buffer_addr[ctx_slot] = gpu_alloc_mapped_aligned_for_gpu(4096, gxm_vdm_buf_size);

	// Allocating vertex ring buffer
	vertex_ring_buffer_addr[ctx_slot] = gpu_alloc_mapped_aligned_for_gpu(4096, gxm_vertex_buf_size);

	// Allocating fragment ring buffer
	fragment_ring_buffer_addr[ctx_slot] = gpu_alloc_mapped_aligned_for_gpu(4096, gxm_fragment_buf_size);

	// Allocating fragment USSE ring buffer
	unsigned int fragment_usse_offset;
	fragment_usse_ring_buffer_addr[ctx_slot] = gpu_fragment_usse_alloc_mapped(gxm_usse_buf_size, &fragment_usse_offset);

	// Setting sceGxm context parameters
	SceGxmContextParams gxm_context_params;
	vgl_memset(&gxm_context_params, 0, sizeof(SceGxmContextParams));
	gxm_context_params.hostMem = context_host_mem[ctx_slot] = vglMalloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
	gxm_context_params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
	gxm_context_params.vdmRingBufferMem = vdm_ring_buffer_addr[ctx_slot];
	gxm_context_params.vdmRingBufferMemSize = gxm_vdm_buf_size;
	gxm_context_params.vertexRingBufferMem = vertex_ring_buffer_addr[ctx_slot];
	gxm_context_params.vertexRingBufferMemSize = gxm_vertex_buf_size;
	gxm_context_params.fragmentRingBufferMem = fragment_ring_buffer_addr[ctx_slot];
	gxm_context_params.fragmentRingBufferMemSize = gxm_fragment_buf_size;
	gxm_context_params.fragmentUsseRingBufferMem = fragment_usse_ring_buffer_addr[ctx_slot];
	gxm_context_params.fragmentUsseRingBufferMemSize = gxm_usse_buf_size;
	gxm_context_params.fragmentUsseRingBufferOffset = fragment_usse_offset;

	// Initializing sceGxm context
	sceGxmCreateContext(&gxm_context_params, ctx);
#ifdef DISABLE_W_CLAMPING
	sceGxmSetWClampEnable(**ctx, SCE_GXM_WCLAMP_MODE_DISABLED);
#endif

	// Initializing circular pool for uniform buffers
	vglSetupUniformCircularPool();
}

void create_display_render_target(void) {
	// Creating render target for the display
	setup_render_target(&gxm_render_target, DISPLAY_WIDTH, DISPLAY_HEIGHT, gxm_display_rt_size);
}

void init_display_color_surfaces(GLboolean is_swap) {
	// Getting access to the shared framebuffer on system app mode
	while (system_app_mode) {
		shared_fb = sceSharedFbOpen(1);
		vgl_memset(&shared_fb_info, 0, sizeof(SceSharedFbInfo));
		sceSharedFbGetInfo(shared_fb, &shared_fb_info);
		if (shared_fb_info.index == 1)
			sceSharedFbClose(shared_fb);
		else {
			sceGxmMapMemory(shared_fb_info.fb_base, shared_fb_info.fb_size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
			gxm_color_surfaces_addr[0] = shared_fb_info.fb_base;
			gxm_color_surfaces_addr[1] = shared_fb_info.fb_base2;
			break;
		}
	}

	for (int i = 0; i < gxm_display_buffer_count; i++) {
		// Allocating color surface memblock
		if (!system_app_mode) {
			gxm_color_surfaces_addr[i] = gpu_alloc_mapped_aligned_for_gpu(4096, VGL_ALIGN(4 * DISPLAY_STRIDE * DISPLAY_HEIGHT, 1 * 1024 * 1024));
			vgl_memset(gxm_color_surfaces_addr[i], 0, 4 * DISPLAY_STRIDE * DISPLAY_HEIGHT);
		}

		// Initializing allocated color surface
		sceGxmColorSurfaceInit(&gxm_color_surfaces[i],
			SCE_GXM_COLOR_FORMAT_A8B8G8R8,
			SCE_GXM_COLOR_SURFACE_LINEAR,
			msaa_mode == SCE_GXM_MULTISAMPLE_NONE ? SCE_GXM_COLOR_SURFACE_SCALE_NONE : SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE,
			SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
			DISPLAY_WIDTH,
			DISPLAY_HEIGHT,
			DISPLAY_STRIDE,
			gxm_color_surfaces_addr[i]);

		// Creating a display sync object for the allocated color surface
		if (!is_swap)
			sceGxmSyncObjectCreate(&gxm_sync_objects[i]);
	}
}

// 1.2: false when the surface could not be made (no memory); `surface` is then untouched and nothing allocated is kept. (With vglSetRobustScenes(0) the
// allocations are not checked, as before.)
GLboolean init_depth_stencil_buffer_ms(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil, SceGxmMultisampleMode mode) {
	// Calculating sizes for depth and stencil surfaces
	unsigned int depth_stencil_width = VGL_ALIGN(w, SCE_GXM_TILE_SIZEX);
#ifndef DEPTH_STENCIL_HACK
	unsigned int depth_stencil_height = VGL_ALIGN(h, SCE_GXM_TILE_SIZEY);
	unsigned int depth_stencil_samples = depth_stencil_width * depth_stencil_height;
	if (mode == SCE_GXM_MULTISAMPLE_2X)
		depth_stencil_samples *= 2;
	else if (mode == SCE_GXM_MULTISAMPLE_4X)
		depth_stencil_samples *= 4;

	// Allocating depth surface
	void *depth_buffer = gpu_alloc_mapped_aligned_for_gpu(SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, 4 * depth_stencil_samples);
	if (!depth_buffer && vgl_robust_scenes) {
		vgl_log("%s:%d Failed to allocate the %u byte depth surface of a %ux%u framebuffer.\n", __FILE__, __LINE__, 4 * depth_stencil_samples, w, h);
		return GL_FALSE;
	}

#ifdef STORE_DEPTH_STENCIL
	// Initializing mask update bit to 1
	vgl_memset(depth_buffer, 0x80, 4 * depth_stencil_samples);
#endif
	vgl_mk_forget(depth_buffer); // 1.3 MASK_KEEP (tests.c): new memory, whatever an old surface at this address held
#endif

	// Allocating stencil surface
#ifndef DEPTH_STENCIL_HACK
	void *stencil_buffer = has_stencil ? gpu_alloc_mapped_aligned_for_gpu(SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, depth_stencil_samples) : NULL;
	if (has_stencil && !stencil_buffer && vgl_robust_scenes) {
		vgl_log("%s:%d Failed to allocate the %u byte stencil surface of a %ux%u framebuffer.\n", __FILE__, __LINE__, depth_stencil_samples, w, h);
		mark_as_dirty(depth_buffer); // (never used by a scene: the collector frees it)
		return GL_FALSE;
	}
#endif

	// Initializing depth and stencil surfaces
	vglDepthStencilSurfaceInit(surface,
		has_stencil ? SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8 : SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M,
		SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR,
		mode == SCE_GXM_MULTISAMPLE_4X ? depth_stencil_width * 2 : depth_stencil_width,
#ifdef DEPTH_STENCIL_HACK
		// Vita's GPU can run without actual depth/stencil memory as far as no partial rendering is hit
		NULL, NULL);
#else
		depth_buffer, stencil_buffer);
#endif

#ifdef STORE_DEPTH_STENCIL
	sceGxmDepthStencilSurfaceSetForceLoadMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
#endif
	return GL_TRUE;
}

GLboolean init_depth_stencil_buffer(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil) {
	return init_depth_stencil_buffer_ms(w, h, surface, has_stencil, msaa_mode);
}

// 0042: a display depth/stencil surface that points at no memory (the scenes begun on it load and store nothing: rt.c vgl_rt_scene_begin)
static void init_display_depth_null(void) {
	vglDepthStencilSurfaceInit(&gxm_depth_stencil_surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR,
		VGL_ALIGN(DISPLAY_WIDTH, SCE_GXM_TILE_SIZEX), NULL, NULL);
	sceGxmDepthStencilSurfaceSetForceLoadMode(&gxm_depth_stencil_surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
	vgl_disp_real = GL_FALSE;
}

void init_display_depth_stencil_surfaces(void) {
	if (vgl_disp_nodepth) {
		init_display_depth_null();
		return;
	}
	init_depth_stencil_buffer(DISPLAY_WIDTH, DISPLAY_HEIGHT, &gxm_depth_stencil_surface, GL_TRUE);
	vgl_disp_real = GL_TRUE;
}

// 0042: called from vglSwapBuffers. With a dialog up the surface gets memory (once); some swaps after the last dialog it is given back.
static void display_depth_for_dialog(GLboolean dialog) {
	if (!vgl_disp_nodepth)
		return;
	if (dialog) {
		vgl_disp_quiet = 0;
		if (!vgl_disp_real) {
			SceGxmDepthStencilSurface real;
			vgl_memset(&real, 0, sizeof(real));
			if (init_depth_stencil_buffer(DISPLAY_WIDTH, DISPLAY_HEIGHT, &real, GL_TRUE)) {
				gxm_depth_stencil_surface = real;
				vgl_disp_real = GL_TRUE;
				vgl_disp_made++;
			} else {
				vgl_disp_failed++; // (the dialog is updated with no depth memory, as before there was a switch: it may show nothing)
			}
		}
		return;
	}
	if (vgl_disp_real && ++vgl_disp_quiet >= VGL_DISP_QUIET_SWAPS) {
		mark_depth_stencil_as_dirty(&gxm_depth_stencil_surface);
		init_display_depth_null();
		vgl_disp_freed++;
		vgl_disp_quiet = 0;
	}
}

void vglSetDisplayNoDepth(GLint on) {
	vgl_disp_nodepth = on ? GL_TRUE : GL_FALSE; // (before vglInit*)
}

void vglDisplayDepthStats(uint32_t *mode, uint32_t *real, uint32_t *made, uint32_t *freed, uint32_t *failed) {
	if (mode) *mode = vgl_disp_nodepth;
	if (real) *real = vgl_disp_real;
	if (made) *made = vgl_disp_made;
	if (freed) *freed = vgl_disp_freed;
	if (failed) *failed = vgl_disp_failed;
}

void start_shader_patcher(void) {
	// Allocating Shader Patcher buffer
	gxm_shader_patcher_buffer_addr = gpu_alloc_mapped_aligned_for_gpu(4, shader_patcher_buffer_size);

	// Allocating Shader Patcher vertex USSE buffer
	unsigned int shader_patcher_vertex_usse_offset;
	gxm_shader_patcher_vertex_usse_addr = gpu_vertex_usse_alloc_mapped(shader_patcher_vertex_usse_size, &shader_patcher_vertex_usse_offset);

	// Allocating Shader Patcher fragment USSE buffer
	unsigned int shader_patcher_fragment_usse_offset;
	gxm_shader_patcher_fragment_usse_addr = gpu_fragment_usse_alloc_mapped(shader_patcher_fragment_usse_size, &shader_patcher_fragment_usse_offset);

	// Populating shader patcher parameters
	SceGxmShaderPatcherParams shader_patcher_params;
	vgl_memset(&shader_patcher_params, 0, sizeof(SceGxmShaderPatcherParams));
	shader_patcher_params.userData = NULL;
	shader_patcher_params.hostAllocCallback = shader_patcher_host_alloc_cb;
	shader_patcher_params.hostFreeCallback = shader_patcher_host_free_cb;
	shader_patcher_params.bufferAllocCallback = NULL;
	shader_patcher_params.bufferFreeCallback = NULL;
	shader_patcher_params.bufferMem = gxm_shader_patcher_buffer_addr;
	shader_patcher_params.bufferMemSize = shader_patcher_buffer_size;
	shader_patcher_params.vertexUsseAllocCallback = NULL;
	shader_patcher_params.vertexUsseFreeCallback = NULL;
	shader_patcher_params.vertexUsseMem = gxm_shader_patcher_vertex_usse_addr;
	shader_patcher_params.vertexUsseMemSize = shader_patcher_vertex_usse_size;
	shader_patcher_params.vertexUsseOffset = shader_patcher_vertex_usse_offset;
	shader_patcher_params.fragmentUsseAllocCallback = NULL;
	shader_patcher_params.fragmentUsseFreeCallback = NULL;
	shader_patcher_params.fragmentUsseMem = gxm_shader_patcher_fragment_usse_addr;
	shader_patcher_params.fragmentUsseMemSize = shader_patcher_fragment_usse_size;
	shader_patcher_params.fragmentUsseOffset = shader_patcher_fragment_usse_offset;

	// Creating shader patcher instance
	sceGxmShaderPatcherCreate(&shader_patcher_params, &gxm_shader_patcher);
	vglVertexProgramCacheReset();
#ifdef HAVE_PROFILING
	sceClibMemset(&vgl_vcache_stats, 0, sizeof(vgl_vertex_program_cache_stats));
#endif
}

static inline __attribute__((always_inline)) void scene_end(void) {
	VGL_LC_SYNC(); // (LAZY_CLEAR: the state of the clear is restored inside the scene it belongs to)
	// Ends current gxm scene
	VGL_STREAM_SCENE_SUBMIT() // 0060: the cached stream arena is written back before the GPU can read the scene's data
	query_fence.value++;
	if (vgl_rt_flags & VGL_RT_GPU_TIMERS) // RT: the scene's own fragment notification for the GPU timer (rt.c)
		vgl_rt_end_scene(gxm_context);
	else
		sceGxmEndScene(gxm_context, NULL, &query_fence);
	vgl_unif_gen++; // (vglSetDrawCaches: a program's copy of its uniforms is named only in the scene it was made for)
	if (system_app_mode && vsync_interval)
		sceDisplayWaitVblankStartMulti(vsync_interval);
}

// 1.2: a scene entry that could not be completed (no memory for the framebuffer's depth/stencil surface or render target, or sceGxmBeginScene refused).
// There is no open scene afterwards, which is the state vglSwapBuffers leaves: the previous scene (if any) was ended, needs_scene_reset makes the next
// attempt start a scene from nothing, and the callers (draws, clears, blits) skip themselves on the false it returns. The same framebuffer is not tried
// again before the next frame: an allocation that failed has already been through the allocator's recovery (a GPU finish and delayed retries), which
// costs seconds, and a draw call after draw call must not each pay for that.
void vglFlushStreamArena(void) {
	VGL_STREAM_SCENE_SUBMIT()
}

static GLboolean scene_reset_failed(void) {
	scene_fail_valid = GL_TRUE;
	scene_fail_fb = active_write_fb;
	scene_fail_frame = vgl_framecount;
	needs_end_scene = GL_FALSE;
	needs_scene_reset = GL_TRUE;
	vgl_scene_failures++;
	vgl_rt_hint_valid = GL_FALSE; // (the operation that the hint was for is skipped)
	vgl_rt_clear_full = GL_FALSE;
	return GL_FALSE;
}

GLboolean scene_reset(void) {
#ifdef LAZY_CLEAR
	// 1.2 LAZY_CLEAR: the sceGxm state a clear left in place is restored before anything else uses or ends the scene. A clear that goes on in the same scene with no
	// scissor update to make keeps it (the next clear needs the same state again).
	if (vgl_lc_pending && (!vgl_lc_in_clear || in_use_framebuffer != active_write_fb || needs_scene_reset || dirty_framebuffer || dirty_query || dirty_scissor_state))
		vgl_lc_flush();
#endif
	if (vgl_robust_scenes && scene_fail_valid && scene_fail_frame == vgl_framecount && scene_fail_fb == active_write_fb &&
		(in_use_framebuffer != active_write_fb || needs_scene_reset || dirty_framebuffer || dirty_query)) {
		vgl_rt_hint_valid = GL_FALSE;
		vgl_rt_clear_full = GL_FALSE;
		return GL_FALSE;
	}
	if (in_use_framebuffer != active_write_fb || needs_scene_reset || dirty_framebuffer || dirty_query) {
		// RT: timers of the scene change (rt.c)
		const GLboolean rt_time = (vgl_rt_flags & VGL_RT_CPU_TIMERS) != 0;
		uint32_t rt_t0 = rt_time ? vgl_rt_now() : 0, rt_t1 = rt_t0, rt_t2 = rt_t0;
		uint32_t rt_applied = 0;
		scene_not_flushed = GL_TRUE;
		dirty_framebuffer = GL_FALSE;
		dirty_query = GL_FALSE;
		needs_scene_reset = GL_FALSE;
		in_use_framebuffer = active_write_fb;
		is_fbo_float = in_use_framebuffer ? in_use_framebuffer->is_float : GL_FALSE;
		cur_msaa = in_use_framebuffer ? FB_MSAA(in_use_framebuffer) : msaa_mode;

		// Ending drawing scene
		if (needs_end_scene) {
			scene_end();
			if (rt_time)
				rt_t1 = vgl_rt_now();
		} else {
			if (legacy_pool_size) {
				legacy_pool = (float *)gpu_alloc_mapped_temp(legacy_pool_size);
				legacy_pool_ptr = legacy_pool;
#ifndef SKIP_ERROR_HANDLING
				legacy_pool_end = (float *)((uint8_t *)legacy_pool + legacy_pool_size);
#endif
			}
			needs_end_scene = GL_TRUE;
		}

#ifndef SKIP_SPLASHSCREEN	
		// If we're running the boot splashscreen, we need to terminate it first
		if (is_splashscreen_active) {
			sceKernelSignalSema(splash_mutex[0], 1);
			sceKernelWaitSema(splash_mutex[1], 1, NULL);
			is_splashscreen_active = GL_FALSE;
			clear_splashscreen();
			sceKernelDeleteSema(splash_mutex[0]);
			sceKernelDeleteSema(splash_mutex[1]);
		}
#endif

		// Starting drawing scene
		is_rendering_display = !active_write_fb;
		if (is_rendering_display) { // Default framebuffer is used
			if (system_app_mode) {
				sceSharedFbBegin(shared_fb, &shared_fb_info);
				shared_fb_info.vsync = vsync_interval;
				gxm_back_buffer_index = (shared_fb_info.index + 1) % 2;
			}
			rt_applied = vgl_rt_scene_begin(&gxm_depth_stencil_surface, DISPLAY_WIDTH, DISPLAY_HEIGHT);
			scn_begin(gxm_render_target, 1);
			vgl_mb_scene_begin(&gxm_depth_stencil_surface, rt_applied); // 1.3 MASK_BG (tests.c)
			const uint32_t rt_bd0 = rt_time ? vgl_rt_now() : 0; // 0065: only the sceGxmBeginScene call below
			int r = sceGxmBeginScene(gxm_context, 0, gxm_render_target,
				NULL, NULL,
				gxm_sync_objects[gxm_back_buffer_index],
				&gxm_color_surfaces[gxm_back_buffer_index],
				&gxm_depth_stencil_surface);
			if (rt_time)
				vgl_rt_begin_split(0, vgl_rt_now() - rt_bd0);
#ifdef LOG_ERRORS
			if (r) {
				vgl_log("%s:%d Scene reset failed due to sceGxmBeginScene erroring (%s) on display.\n", __FILE__, __LINE__, get_gxm_error_literal(r));
			}
#endif
			if (r && vgl_robust_scenes)
				return scene_reset_failed();
		} else {
			// 0062: an object whose colour texture was detached (vglSetFramebufferDetachKeep) has nothing to draw into: no scene, whether or not the robust scenes are on (nxr6b: with them off the
			// code logged and fell through to sceGxmBeginScene with the colour surface of the freed texture; a scene that cannot start is the only answer, and it is what the robust mode does)
			if (!active_write_fb->tex) {
				vgl_log("%s:%d Scene reset failed: framebuffer 0x%08X has no colour texture.\n", __FILE__, __LINE__, active_write_fb);
				return scene_reset_failed();
			}
			// Double check that colorsurface and target texture are synced
			else if (sceGxmColorSurfaceGetData(&active_write_fb->colorbuffer) != active_write_fb->tex->data) {
				_glFramebufferTexture2D(active_write_fb, GL_COLOR_ATTACHMENT0, active_write_fb->tex, 1);
			}
			
			// If the depthbuffer is dirty, we re-gen it
			if (!(active_write_fb->depthbuffer_state & DEPTHBUFFER_READY) && active_write_fb->nodepth) {
				// 0038: no depth or stencil memory for this framebuffer (vglFramebufferNoDepth): a surface with no data, never loaded or stored
				vglDepthStencilSurfaceInit(&active_write_fb->depthbuffer, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR,
					FB_MSAA(active_write_fb) == SCE_GXM_MULTISAMPLE_4X ? VGL_ALIGN(active_write_fb->width, SCE_GXM_TILE_SIZEX) * 2 : VGL_ALIGN(active_write_fb->width, SCE_GXM_TILE_SIZEX), NULL, NULL);
				sceGxmDepthStencilSurfaceSetForceLoadMode(&active_write_fb->depthbuffer, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
				sceGxmDepthStencilSurfaceSetForceStoreMode(&active_write_fb->depthbuffer, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
				active_write_fb->depthbuffer_ptr = &active_write_fb->depthbuffer;
				active_write_fb->depthbuffer_state |= DEPTHBUFFER_READY;
			} else if (!(active_write_fb->depthbuffer_state & DEPTHBUFFER_READY)) {
				// 0044: a depth surface in the shared block (vglFramebufferDepthShare), when the object fits it and wants no stencil; otherwise its own
				GLboolean depth_shared = GL_FALSE;
				if (active_write_fb->depth_share_w && active_write_fb->depthbuffer_state != DEPTHBUFFER_WANTS_STENCIL && active_write_fb->width <= active_write_fb->depth_share_w && active_write_fb->height <= active_write_fb->depth_share_h)
					depth_shared = vgl_depth_share_get(active_write_fb->depth_share_w, active_write_fb->depth_share_h, FB_MSAA(active_write_fb), &active_write_fb->depthbuffer, active_write_fb);
				active_write_fb->depth_row_w = depth_shared ? active_write_fb->depth_share_w : active_write_fb->width;
				if (!depth_shared && !init_depth_stencil_buffer_ms(active_write_fb->width, active_write_fb->height, &active_write_fb->depthbuffer, active_write_fb->depthbuffer_state == DEPTHBUFFER_WANTS_STENCIL, FB_MSAA(active_write_fb)) && vgl_robust_scenes) {
					vgl_log("%s:%d Scene reset failed: no depth/stencil surface for framebuffer 0x%08X (%dx%d).\n", __FILE__, __LINE__, active_write_fb, active_write_fb->width, active_write_fb->height);
					return scene_reset_failed();
				}
				active_write_fb->depthbuffer_ptr = &active_write_fb->depthbuffer;
				active_write_fb->depthbuffer_state |= DEPTHBUFFER_READY;
			}

			// If a rendertarget is not bound to the in use framebuffer, we get one for it
			if (!active_write_fb->target) {
#ifdef HAVE_SHARED_RENDERTARGETS
				active_write_fb->target = (SceGxmRenderTarget *)get_free_render_target(active_write_fb->width, active_write_fb->height);
#else
				int r = setup_fbo_render_target_ms(&active_write_fb->target, active_write_fb->width, active_write_fb->height, gxm_fbo_rt_size, FB_MSAA(active_write_fb));
#ifdef LOG_ERRORS
				if (r) {
					vgl_log("%s:%d Failed to create a rendertarget of size %dx%d for framebuffer 0x%08X (%s).\n", __FILE__, __LINE__, active_write_fb->width, active_write_fb->height, active_write_fb, get_gxm_error_literal(r));
				}
#endif
				// 1.3: the result decides, not the pointer: nothing promises what a failed call leaves there (Vita3K leaves a pointer to the block it has just freed)
				if (r)
					active_write_fb->target = NULL;
				else if (active_write_fb->dual_rt)
					vgl_dual_make(active_write_fb); // 0063: both targets together, at the moment that already pays for the first one
#endif
			}
#ifdef RECYCLE_RENDERTARGETS
			else {
				render_target *fbo_rt = (render_target *)active_write_fb->target;
				if (active_write_fb->width != fbo_rt->w || active_write_fb->height != fbo_rt->h) {
					vgl_log("%s:%d Attempting to use a recycled rendertarget. Re-allocating it.\n", __FILE__, __LINE__);
					active_write_fb->target = (SceGxmRenderTarget *)get_free_render_target(active_write_fb->width, active_write_fb->height);
				}
			}
#endif
			if (!active_write_fb->target && vgl_robust_scenes) {
				vgl_log("%s:%d Scene reset failed: no render target for framebuffer 0x%08X (%dx%d).\n", __FILE__, __LINE__, active_write_fb, active_write_fb->width, active_write_fb->height);
				return scene_reset_failed();
			}
			if (active_write_fb->depth_share_w && active_write_fb->depthbuffer_ptr && active_write_fb->depthbuffer_ptr->depthData)
				vgl_depth_share_scene(active_write_fb->depthbuffer_ptr->depthData, active_write_fb); // 0044
			rt_applied = vgl_rt_scene_begin(active_write_fb->depthbuffer_ptr, active_write_fb->width, active_write_fb->height);
			SceGxmRenderTarget *scene_rt = vgl_dual_pick(active_write_fb, vgl_framecount); // 0063 (the target of this picture's parity; ->target unless dual_rt)
			scn_begin(scene_rt, 0);
#ifndef TEXTURES_SPEEDHACK
			if (active_write_fb->tex)
				active_write_fb->tex->last_frame = vgl_framecount; // int28 (Carbon #2): keep FBO-written textures in 4-frame deferred GC even if never sampled yet
#endif
			if (vgl_fg_on && active_write_fb->tex)
				active_write_fb->tex->fg_rt_frame = vgl_framecount + 1; // 0034
			vgl_mb_scene_begin(active_write_fb->depthbuffer_ptr, rt_applied); // 1.3 MASK_BG (tests.c)
#ifdef HAVE_SHARED_RENDERTARGETS
			render_target *fbo_rt = (render_target *)active_write_fb->target;
#ifdef RECYCLE_RENDERTARGETS
			fbo_rt->last_frame = vgl_framecount;
#endif
			const uint32_t rt_bf0 = rt_time ? vgl_rt_now() : 0; // 0065: only the sceGxmBeginScene call below
			int r = sceGxmBeginScene(gxm_context, 0, fbo_rt->rt,
					NULL, NULL, NULL,
					&active_write_fb->colorbuffer,
					active_write_fb->depthbuffer_ptr);
#else
			const uint32_t rt_bf0 = rt_time ? vgl_rt_now() : 0; // 0065: only the sceGxmBeginScene call below
			int r = sceGxmBeginScene(gxm_context, 0, scene_rt,
					NULL, NULL, NULL,
					&active_write_fb->colorbuffer,
					active_write_fb->depthbuffer_ptr);
#endif
			if (rt_time)
				vgl_rt_begin_split(1, vgl_rt_now() - rt_bf0);
#ifdef LOG_ERRORS
			if (r)
				vgl_log("%s:%d Scene reset failed due to sceGxmBeginScene erroring (%s) on framebuffer 0x%08X.\n", __FILE__, __LINE__, get_gxm_error_literal(r), active_write_fb);
#endif
			if (r && vgl_robust_scenes)
				return scene_reset_failed();
		}

		if (rt_time)
			rt_t2 = vgl_rt_now();
		// 1.3 MASK_KEEP (tests.c): what the scene's depth/stencil surface brings in, with the load and store modes it begun with
		vgl_mk_scene_begin(is_rendering_display ? &gxm_depth_stencil_surface : active_write_fb->depthbuffer_ptr);
		// RT: a surface that was not loaded holds the background mask (0): rebuild the mask as every framebuffer object scene does
		if (rt_applied & VGL_RT_NOLOAD) {
			dirty_scissor_state = GL_TRUE;
			vgl_rt_skip_invalidation = GL_TRUE; // (the mask starts at the background value 0: as upstream's path without STORE_DEPTH_STENCIL)
		}

		// Setting back current viewport if enabled cause sceGxm will reset it at sceGxmEndScene call
		if (old_framebuffer != in_use_framebuffer) {
			dirty_scissor_state = GL_TRUE;
			old_framebuffer = in_use_framebuffer;
			glViewport(gl_viewport.x, gl_viewport.y, gl_viewport.w, gl_viewport.h);
#ifndef HAVE_UNFLIPPED_FBOS
			change_cull_mode();
#endif
		} else {
			vglSetViewport(gxm_context, x_port, x_scale, y_port, y_scale, z_port, z_scale);
		}

#ifndef DISABLE_TILE_CLIPPER
		if (scissor_test_state) {
			sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE, region.x, region.y, region.x + region.w - 1, region.y + region.h - 1);
		}
#endif
#ifdef STORE_DEPTH_STENCIL
		if (in_use_framebuffer) {
			dirty_scissor_state = GL_TRUE;
		}
#else
		dirty_scissor_state = GL_TRUE;
		skip_scissor_invalidation = GL_TRUE;
#endif
		if (dirty_scissor_state) {
			update_scissor_test();
		}
		if (rt_time) {
			const uint32_t rt_t3 = vgl_rt_now();
			vgl_rt_cpu_switch(rt_t1 - rt_t0, rt_t2 - rt_t1, rt_t3 - rt_t2);
		}
	}
	vgl_rt_hint_valid = GL_FALSE; // RT: a hint is for the operation that follows it, whether or not that began a scene
	vgl_rt_clear_full = GL_FALSE;
	
	if (dirty_scissor_state) {
		update_scissor_test();
	}
	return GL_TRUE;
}

/*
 * ------------------------------
 * - IMPLEMENTATION STARTS HERE -
 * ------------------------------
 */

void vglSetupGarbageCollector(int priority, int affinity) {
	gc_thread_priority = priority;
	gc_thread_affinity = affinity;
}

void vglSetParamBufferSize(uint32_t size) {
	gxm_param_buf_size = size;
}

// 0049: sceGxm's display queue limit (displayQueueMaxPendingCount) in place of buffers - 1; 0 = vitaGL's own. Must be called before vglInit* (and after the buffer count is set).
void vglSetDisplayQueuePending(GLint n) {
	vgl_dq_pending_want = n > 0 ? (n > 8 ? 8 : (uint32_t)n) : 0;
}

// 0049: time sceGxmDisplayQueueAddEntry in vglSwapBuffers (when on). Stats: the last call, the longest, calls, calls of 2 ms or more, the limit given to sceGxm, the total us.
void vglSetDisplayQueueTiming(GLint on) {
	vgl_dq_timing = on ? GL_TRUE : GL_FALSE;
}
void vglDisplayQueueStats(uint32_t *last_us, uint32_t *max_us, uint32_t *adds, uint32_t *slow, uint32_t *pending, uint64_t *total_us) {
	if (last_us) *last_us = vgl_dq_last_us;
	if (max_us) *max_us = vgl_dq_max_us;
	if (adds) *adds = vgl_dq_adds;
	if (slow) *slow = vgl_dq_slow;
	if (pending) *pending = vgl_dq_pending_used;
	if (total_us) *total_us = vgl_dq_total_us;
}

// 0042: ask for the parameter buffer in main memory (SCE_GXM_INITIALIZE_FLAG_PB_LPDDR) in a normal application; before vglInit*. State: vglParamBufferLpddrState.
void vglSetParamBufferLpddr(GLint on) {
	vgl_pb_lpddr_want = on ? GL_TRUE : GL_FALSE;
}

// 0: not asked, 1: taken, -1: refused (sceGxm returned *rc and the buffer is in video memory), *shrink = the bytes a caller's video memory request was too big by
int vglParamBufferLpddrState(int *rc, uint32_t *shrink) {
	if (rc) *rc = vgl_pb_lpddr_rc;
	if (shrink) *shrink = vgl_pb_lpddr_shrink;
	return vgl_pb_lpddr_state;
}

void vglSetVDMBufferSize(uint32_t size) {
	gxm_vdm_buf_size = size;
}

void vglSetVertexBufferSize(uint32_t size) {
	gxm_vertex_buf_size = size;
}

void vglSetFragmentBufferSize(uint32_t size) {
	gxm_fragment_buf_size = size;
}

void vglSetUSSEBufferSize(uint32_t size) {
	gxm_usse_buf_size = size;
}

void vglUseTripleBuffering(GLboolean usage) {
	gxm_display_buffer_count = usage ? 3 : 2;
}

void vglSetDisplayBufferCount(int count) {
#ifndef SKIP_ERROR_HANDLING
	if (count < 0) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, count)
	}
#endif
	gxm_display_buffer_count = count;
}

void vglSwapBuffers(GLboolean has_commondialog) {
	THREAD_SAFE()

#ifndef SKIP_SPLASHSCREEN
	// Ignore display swaps during splashscreen since we handle them in the splashscreen thread
	if (is_splashscreen_active)
		return;
#endif

#ifdef HAVE_PROFILING
	// Show profiling results once every 30 frames to not clog CPU
	uint32_t tick = sceKernelGetProcessTimeLow();
	static uint32_t frame_start_profiler_cnt = 0;
	frame_profiler_cnt += tick - frame_start_profiler_cnt;
	if ((vgl_framecount % 30) == 0) {
		vgl_log("-----------------------------------------\n");
		vgl_log("Last 30 frames took %ums to be processed.\n", frame_profiler_cnt / 1000);
		vgl_log("%ums spent processing %u fixed-function pipeline non-immediate draw calls.\n", ffp_draw_profiler_cnt / 1000, ffp_draw_cnt);
		vgl_log("%ums spent setting up fixed-function pipeline states.\n", ffp_reload_profiler_cnt / 1000);
		vgl_log("%ums spent processing %u shaders pipeline draw calls.\n", shaders_draw_profiler_cnt / 1000, shaders_draw_cnt);
		vgl_log("%ums spent waiting for GPU to process frames.\n", gpu_stall_cnt / 1000);
		vgl_log("-----------------------------------------\n");
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
		vgl_log("Vertex layout cache stats: %u hits, %u misses.\n", vgl_vcache_stats.layout_hits, vgl_vcache_stats.layout_misses);
#endif
		vgl_log("Vertex shaders cache stats: %u calls, %u cache hits (%u MRU, %u table), %u cache misses, %u cache evictions.\n", vgl_vcache_stats.calls_num, vgl_vcache_stats.hits, vgl_vcache_stats.mru_hits, vgl_vcache_stats.table_hits, vgl_vcache_stats.gen_calls, vgl_vcache_stats.evictions);
		vgl_log("%uus spent inside vertex shaders patching (%uus MRU, %uus table, %uus cache gens).\n", vgl_vcache_stats.total_us, vgl_vcache_stats.mru_us, vgl_vcache_stats.table_us, vgl_vcache_stats.gen_us);
		vgl_log("-----------------------------------------\n");
		frame_profiler_cnt = 0;
		ffp_draw_profiler_cnt = 0;
		ffp_reload_profiler_cnt = 0;
		shaders_draw_profiler_cnt = 0;
		shaders_draw_cnt = 0;
		ffp_draw_cnt = 0;
		gpu_stall_cnt = 0;
		sceClibMemset(&vgl_vcache_stats, 0, sizeof(vgl_vertex_program_cache_stats));
	}
	frame_start_profiler_cnt = tick;
#endif

	scn_frame_end();
	if (vgl_fg_on)
		fg_flush(); // 0034
	vgl_framecount++;
#if !defined(DISABLE_CIRCULAR_POOL) && !defined(CIRCULAR_POOL_SPEEDHACK)
#ifdef HAVE_DEBUG_INTERFACE
	vgl_circular_pool_frame_peak = (uint32_t)circular_data_pool_ptr[vgl_circular_idx] - (uint32_t)circular_data_pool[vgl_circular_idx];
	if (vgl_circular_pool_frame_peak > vgl_circular_pool_global_peak) {
		vgl_circular_pool_global_peak = vgl_circular_pool_frame_peak;
	}
#endif
#ifdef LOG_ERRORS
	if (circular_data_pool_ptr[vgl_circular_idx] > circular_data_pool_limit[vgl_circular_idx]) {
		vgl_log("%s:%d Circular pool overrun on frame %u (Total of %u bytes). This can cause performance issues. Consider increasing its size with vglSetCircularPoolSize.\n", __FILE__, __LINE__, vgl_framecount - 1, circular_data_pool_ptr[vgl_circular_idx] - circular_data_pool_limit[vgl_circular_idx]);
	}
#endif
	vgl_glb_frame_end();
	vgl_circular_idx = vgl_framecount % gxm_display_buffer_count;
	circular_data_pool_ptr[vgl_circular_idx] = circular_data_pool[vgl_circular_idx];
	vgl_stream_next_slice();
#endif

	// Marking uniform values as dirty at each frame end just to be safe
	dirty_frag_unifs = 0xFFFFFFFF;
	dirty_vert_unifs = 0xFFFF;
	dirty_shader_frag_unifs = GL_TRUE;
	dirty_shader_vert_unifs = GL_TRUE;
	needs_end_scene = GL_FALSE;

	if (!needs_scene_reset)
		scene_end();
	vgl_rt_frame_end(); // RT: per-frame scene counters and timers (rt.c)

	display_depth_for_dialog(vgl_has_cdlg_support && has_commondialog ? GL_TRUE : GL_FALSE); // 0042
	if (vgl_has_cdlg_support && has_commondialog) {
		// Performing commondialog update
		SceCommonDialogUpdateParam updateParam;
		vgl_memset(&updateParam, 0, sizeof(updateParam));
		updateParam.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
		updateParam.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
		updateParam.renderTarget.width = DISPLAY_WIDTH;
		updateParam.renderTarget.height = DISPLAY_HEIGHT;
		updateParam.renderTarget.strideInPixels = DISPLAY_STRIDE;
		updateParam.renderTarget.colorSurfaceData = gxm_color_surfaces_addr[gxm_back_buffer_index];
		updateParam.renderTarget.depthSurfaceData = gxm_depth_stencil_surface.depthData;
		updateParam.displaySyncObject = gxm_sync_objects[gxm_back_buffer_index];
		sceCommonDialogUpdate(&updateParam);
	}

	if (system_app_mode)
		sceSharedFbEnd(shared_fb);
	else {
#ifdef HAVE_RAZOR
		sceGxmPadHeartbeat(&gxm_color_surfaces[gxm_back_buffer_index], gxm_sync_objects[gxm_back_buffer_index]);
#ifdef HAVE_DEVKIT
		if (has_razor_live) {
			SceRazorGpuLiveResultInfo razor_res;
			sceRazorGpuLiveSetBuffer(razor_buf[gxm_back_buffer_index], RAZOR_BUF_SIZE, &razor_res);

			if (razor_res.result_data) {
				if (!razor_res.overflow_count) {
					vgl_memset(&razor_metrics, 0, sizeof(razor_results));
					SceUID pid = sceKernelGetProcessId();
					SceRazorGpuResult r;
					r.ptr = (uintptr_t)razor_res.result_data;

					// Analyzing the collected jobs
					for (uint32_t i = 0; i < razor_res.entry_count; i++) {
						switch (r.job->header.entry_type) {
						case SCE_RAZOR_LIVE_TRACE_METRIC_ENTRY_TYPE_JOB:
							if ((pid == r.job->process_id) && (r.job->type != SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FIRMWARE)) {
								if (razor_metrics.scene_count < r.job->scene_index + 1)
									razor_metrics.scene_count = r.job->scene_index + 1;
								switch (r.job->type) {
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_VERTEX0:
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_VERTEX1:
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_VERTEX2:
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_VERTEX3:
									razor_metrics.vertex_job_count++;
									razor_metrics.vertex_job_time += r.job->end_time - r.job->start_time;
									if (r.job->scene_index < RAZOR_MAX_SCENES_NUM) {
										razor_metrics.scenes[r.job->scene_index].vertex_duration += r.job->end_time - r.job->start_time;
									}
									break;
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FRAGMENT0:
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FRAGMENT1:
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FRAGMENT2:
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FRAGMENT3:
									razor_metrics.fragment_job_count++;
									razor_metrics.fragment_job_time += r.job->end_time - r.job->start_time;
									if (r.job->scene_index < RAZOR_MAX_SCENES_NUM) {
										razor_metrics.scenes[r.job->scene_index].fragment_duration += r.job->end_time - r.job->start_time;
									}
									break;
								}
								switch (r.job->type) {
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_VERTEX1:
									razor_metrics.usse_vertex_processing_percent += r.job->job_values.vertex_values_type1.usse_vertex_processing_percent;
									break;
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_VERTEX2:
									razor_metrics.vdm_primitives_input_num += r.job->job_values.vertex_values_type2.vdm_primitives_input_num;
									razor_metrics.mte_primitives_output_num += r.job->job_values.vertex_values_type2.mte_primitives_output_num;
									razor_metrics.vdm_vertices_input_num += r.job->job_values.vertex_values_type2.vdm_vertices_input_num;
									razor_metrics.mte_vertices_output_num += r.job->job_values.vertex_values_type2.mte_vertices_output_num;
									break;
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_VERTEX3:
									razor_metrics.tiling_accelerated_mem_writes += r.job->job_values.vertex_values_type3.tiling_accelerated_mem_writes;
									break;
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FRAGMENT1:
									razor_metrics.usse_fragment_processing_percent += r.job->job_values.fragment_values_type1.usse_fragment_processing_percent;
									razor_metrics.usse_dependent_texture_reads_percent += r.job->job_values.fragment_values_type1.usse_dependent_texture_reads_percent;
									razor_metrics.usse_non_dependent_texture_reads_percent += r.job->job_values.fragment_values_type1.usse_non_dependent_texture_reads_percent;
									break;
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FRAGMENT2:
									razor_metrics.rasterized_pixels_before_hsr_num += r.job->job_values.fragment_values_type2.rasterized_pixels_before_hsr_num;
									razor_metrics.rasterized_output_pixels_num += r.job->job_values.fragment_values_type2.rasterized_output_pixels_num;
									razor_metrics.rasterized_output_samples_num += r.job->job_values.fragment_values_type2.rasterized_output_samples_num;
									break;
								case SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FRAGMENT3:
									razor_metrics.isp_parameter_fetches_mem_reads += r.job->job_values.fragment_values_type3.isp_parameter_fetches_mem_reads;
									break;
								}
							} else if (r.job->type == SCE_RAZOR_LIVE_TRACE_METRIC_JOB_TYPE_FIRMWARE) {
								razor_metrics.firmware_job_count++;
								razor_metrics.firmware_job_time += r.job->end_time - r.job->start_time;
							}
							break;
						case SCE_RAZOR_LIVE_TRACE_METRIC_ENTRY_TYPE_PARAMETER_BUFFER:
							vgl_fast_memcpy(&razor_metrics.peak_usage_value, &r.pbuf->peak_usage_value, 6);
							break;
						case SCE_RAZOR_LIVE_TRACE_METRIC_ENTRY_TYPE_FRAME:
							vgl_fast_memcpy(&razor_metrics.frame_start_time, &r.frame->start_time, 20);
							break;
						default:
							break;
						}
						r.ptr += r.job->header.entry_size;
					}
				} else {
					vgl_log("%s:%d Razor Live Metrics overflow detected (%d entries). Consider increasing RAZOR_BUF_SIZE.\n", __FILE__, __LINE__, razor_res.overflow_count);
				}
			}
		}
#endif
#endif
		struct display_queue_callback_data queue_cb_data;
		queue_cb_data.addr = gxm_color_surfaces_addr[gxm_back_buffer_index];
#ifdef HAVE_PROFILING
		tick = sceKernelGetProcessTimeLow();
#endif
		if (vgl_dq_timing) {
			const uint32_t dq_t0 = sceKernelGetProcessTimeLow();
			sceGxmDisplayQueueAddEntry(gxm_sync_objects[gxm_front_buffer_index], gxm_sync_objects[gxm_back_buffer_index], &queue_cb_data);
			const uint32_t dq_us = sceKernelGetProcessTimeLow() - dq_t0;
			vgl_dq_last_us = dq_us;
			vgl_dq_total_us += dq_us;
			vgl_dq_adds++;
			if (dq_us > vgl_dq_max_us)
				vgl_dq_max_us = dq_us;
			if (dq_us >= 2000)
				vgl_dq_slow++;
		} else
		sceGxmDisplayQueueAddEntry(gxm_sync_objects[gxm_front_buffer_index], gxm_sync_objects[gxm_back_buffer_index], &queue_cb_data);
#ifdef HAVE_PROFILING
		gpu_stall_cnt += sceKernelGetProcessTimeLow() - tick;
#endif
#ifdef HAVE_CPU_TRACER
		sceRazorCpuSync();
#endif
		gxm_front_buffer_index = gxm_back_buffer_index;
		gxm_back_buffer_index = (gxm_back_buffer_index + 1) % gxm_display_buffer_count;
	}
	needs_scene_reset = GL_TRUE;
	
	// Perform resolution change if there's one pending
	if (NEW_DISPLAY_WIDTH) {
		sceGxmFinish(gxm_context);
		sceGxmDestroyRenderTarget(gxm_render_target);
		if (!system_app_mode) {
			for (int i = 0; i < gxm_display_buffer_count; i++) {
				vgl_free(gxm_color_surfaces_addr[i]);
			}
		}
		DISPLAY_WIDTH = NEW_DISPLAY_WIDTH;
		DISPLAY_HEIGHT = NEW_DISPLAY_HEIGHT;
		DISPLAY_WIDTH_FLOAT = DISPLAY_WIDTH * 1.0f;
		DISPLAY_HEIGHT_FLOAT = DISPLAY_HEIGHT * 1.0f;
		DISPLAY_STRIDE = VGL_ALIGN(DISPLAY_WIDTH, 64);
		vector4f_convert_to_local_space(clear_vertices, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
		update_scissor_test_uniforms();
		create_display_render_target();
		init_display_color_surfaces(GL_TRUE);
		NEW_DISPLAY_WIDTH = 0;
	}

	// Starting garbage collector job
#ifdef HAVE_SINGLE_THREADED_GC
	garbage_collector(0, NULL);
#else
	gc_enqueue(NULL);
#endif
}

void glFinish(void) {
	THREAD_SAFE()

	// Waiting for GPU to finish drawing jobs
	if (scene_not_flushed) {
#ifdef VGL_FINISH_NOSCENE
		// 0052 (upstream 74c5a4a "Fix a potential deadlock in glFinish", adapted): the open scene is ended and the GPU is waited for with no scene open; the next call
		// that needs a scene starts it through needs_scene_reset. The old text ended the scene, began a new one and called sceGxmFinish inside it.
		if (needs_end_scene) {
			scene_end();
			needs_end_scene = GL_FALSE;
		}
		sceGxmFinish(gxm_context);
		needs_scene_reset = GL_TRUE;
		dirty_frag_unifs = 0xFFFFFFFF;
		dirty_vert_unifs = 0xFFFF;
		dirty_shader_frag_unifs = GL_TRUE;
		dirty_shader_vert_unifs = GL_TRUE;
		scene_not_flushed = GL_FALSE;
#else
		dirty_framebuffer = GL_TRUE;
		scene_reset();
		sceGxmFinish(gxm_context);
		scene_not_flushed = GL_FALSE;
#endif
	}
}

void glReleaseShaderCompiler(void) {
	THREAD_SAFE()

	if (is_shark_online) {
		shark_end();
		is_shark_online = GL_FALSE;
	}
}

void glFlush(void) {
	THREAD_SAFE()
	
	if (scene_not_flushed) {
		dirty_framebuffer = GL_TRUE;
		scene_reset();
		scene_not_flushed = GL_FALSE;
	}
}

void vglSetDisplayCallback(void (*cb)(void *framebuf)) {
	vgl_display_cb = cb;
}

void vglSetupShaderPatcher(uint32_t buffer_mem_size, uint32_t vertex_usse_mem_size, uint32_t fragment_usse_mem_size) {
	shader_patcher_buffer_size = buffer_mem_size;
	shader_patcher_vertex_usse_size = vertex_usse_mem_size;
	shader_patcher_fragment_usse_size = fragment_usse_mem_size;
}

GLboolean vglFramebufferDualRT(GLuint fb_id, GLint on) {
	framebuffer *fb = (framebuffer *)fb_id;
	if (!fb || !fb->active)
		return GL_FALSE;
#ifdef HAVE_SHARED_RENDERTARGETS
	return GL_FALSE;
#else
	fb->dual_rt = on ? 1 : 0;
	if (!on)
		VGL_FB_DROP_RT2(fb);
	return GL_TRUE;
#endif
}
void vglSetupRenderTargetScenesNum(uint8_t size_main, uint8_t size_fbos) {
#ifndef SKIP_ERROR_HANDLING
	if (size_main > MAX_SCENES_PER_FRAME) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, size_main)
	} else if (size_fbos > MAX_SCENES_PER_FRAME) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, size_fbos)
	}
#endif
	gxm_display_rt_size = size_main;
	gxm_fbo_rt_size = size_fbos;
}
