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
 * tests.c:
 * Implementation for all drawing tests functions
 */

#include "shared.h"

// Depth Test
GLboolean depth_test_state = GL_FALSE; // Current state for GL_DEPTH_TEST
SceGxmDepthFunc depth_func = SCE_GXM_DEPTH_FUNC_LESS; // Current in-use depth test func
GLenum orig_depth_test; // Original depth test state (used for depth test invalidation)
GLdouble depth_value = 1.0f; // Current depth test clear value
GLboolean depth_mask_state = GL_TRUE; // Current state for glDepthMask
GLboolean dirty_scissor_state = GL_FALSE; // Flag for when scissor test update must be performed

// Scissor Test
scissor_region region; // Current scissor test region setup
GLboolean scissor_test_state = GL_FALSE; // Current state for GL_SCISSOR_TEST
SceGxmFragmentProgram *scissor_test_fragment_program; // Scissor test fragment program
vector4f *scissor_test_vertices = NULL; // Scissor test region vertices
#ifndef STORE_DEPTH_STENCIL
GLboolean skip_scissor_invalidation = GL_TRUE; // Flag whether to skip mask update bit invalidation pass during scissor test region update
#endif

// 1.3 MASK_KEEP (patch 0026, MSC_VGX_VGL_MASKKEEP; the Galaxy port's vitaGL 0018, written again from its description). update_scissor_test draws the mask
// (the scissor bit of the depth/stencil surface) again from nothing: a full-target quad that clears the bit, then the quad of the region (or of the whole
// target) that sets it. Under STORE_DEPTH_STENCIL it does so at the start of every framebuffer-object scene and at every return to the display, and after any
// scissor or attribute restore, although the surface usually holds that very mask already: it was drawn there before, stored at the scene's end and loaded at
// this one's start. The two quads are deterministic: what they leave depends only on the scissor state, the region and the target (size, display or not),
// never on what the bit held before. So each depth/stencil surface (by its memory) keeps a record of the mask it holds, and a rebuild whose inputs equal the
// record is not drawn. The record is dropped whenever the bit could hold anything else:
//   - a new surface (init_depth_stencil_buffer_ms: the memory is set to 0x80, a different mask, and the address may be an old surface's);
//   - a scene that does not load the surface (it begins from the background mask, 0; RT NOLOAD);
//   - a scene that does not store it (the memory keeps an older mask, or part of this one after a partial render): the scene follows a copy of the record and
//     the surface's own record is dropped;
//   - a clear that writes stencil (vgl_mk_touch; nothing documents what such a clear does to the bit's neighbour, so it is not assumed);
//   - more surfaces than the table holds (the oldest record is reused).
// vglSetMaskKeep(GL_FALSE) (the default) draws every rebuild as before; the records are kept either way, so the switch can change at any time.
GLboolean vgl_mask_keep = GL_FALSE;
uint32_t vgl_mk_stat[4]; // rebuilds drawn, rebuilds kept, records dropped by a clear, scenes begun without a record to use
typedef struct {
	const void *surface; // the surface's depth memory (NULL: unused)
	uint32_t w, h; // the target the mask was drawn for
	int x, y, rw, rh; // the region (sceGxm rows from the top) when on
	int gly; // the region's GL y (rows from the bottom): the tile clip of a framebuffer object is given in it (MASK_CLIP, patch 0055)
	uint8_t on, disp, valid;
} vgl_mk_rec;
#define VGL_MK_RECS 16
static vgl_mk_rec vgl_mk_recs[VGL_MK_RECS];
static vgl_mk_rec vgl_mk_scene_copy; // a scene that does not store: the mask as it goes in that scene only
static vgl_mk_rec *vgl_mk_cur = NULL; // the open scene's mask (NULL: unknown, every rebuild drawn)
static uint32_t vgl_mk_next = 0;

void vglSetMaskKeep(GLboolean on) {
	vgl_mask_keep = on;
}
void vglGetMaskKeepStats(uint32_t *drawn, uint32_t *kept) {
	if (drawn)
		*drawn = vgl_mk_stat[0];
	if (kept)
		*kept = vgl_mk_stat[1];
}
// 1.3 MASK_CLIP (patch 0055, MSC_VGX_VGL_MASKCLIP): the rebuild of a scissor mask under an enabled scissor draws a full-target quad that clears the bit (then the region's quad that sets
// it). When the record says what the surface holds (the mask of an earlier region: ones inside it, zeros everywhere else) the clear needs to reach only that region: the bit is clear
// outside it already. The clear quad is drawn with the tile region clip set to the old region (the clip rounds outward to whole tiles; a tile's extra pixels are zero already), then the
// clip goes back to the whole target for the set quad as before. Same mask after the rebuild; the fill of the clear falls from the whole target to the old region's tiles (a held scene's
// crowd atlas, 990x1139, rebuilt about 20 times a picture). The record is the one MASK_KEEP keeps (valid only as MASK_KEEP trusts it); no record: the whole-target clear as before.
// vglSetMaskClip(GL_FALSE) (the default) draws every clear over the whole target.
GLboolean vgl_mask_clip = GL_FALSE;
uint32_t vgl_mc_stat[2]; // clears drawn over the old region only, kilopixels of whole-target clear fill that left out
void vglSetMaskClip(GLboolean on) {
	vgl_mask_clip = on;
}
void vglGetMaskClipStats(uint32_t *narrowed, uint32_t *kpx_saved) {
	if (narrowed)
		*narrowed = vgl_mc_stat[0];
	if (kpx_saved)
		*kpx_saved = vgl_mc_stat[1];
}
static vgl_mk_rec *vgl_mk_find(const void *surface) {
	for (int i = 0; i < VGL_MK_RECS; i++) {
		if (vgl_mk_recs[i].surface == surface)
			return &vgl_mk_recs[i];
	}
	return NULL;
}
// gxm.c init_depth_stencil_buffer_ms: `surface` is new memory for a depth/stencil surface
void vgl_mk_forget(const void *surface) {
	vgl_mk_rec *r = vgl_mk_find(surface);
	if (r) {
		r->valid = 0;
		r->surface = NULL;
	}
	if (vgl_mk_cur && vgl_mk_cur->surface == surface)
		vgl_mk_cur = NULL;
}
// gxm.c scene_reset, after sceGxmBeginScene: the scene's depth/stencil surface (NULL: none) and its load and store modes as the scene runs with them
void vgl_mk_scene_begin(SceGxmDepthStencilSurface *ds) {
	vgl_mk_cur = NULL;
	if (!ds || !ds->depthData) {
		vgl_mk_stat[3]++;
		return;
	}
	const GLboolean loads = sceGxmDepthStencilSurfaceGetForceLoadMode(ds) == SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED;
	const GLboolean stores = sceGxmDepthStencilSurfaceGetForceStoreMode(ds) == SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED;
	vgl_mk_rec *r = vgl_mk_find(ds->depthData);
	if (!r) {
		r = &vgl_mk_recs[vgl_mk_next];
		vgl_mk_next = (vgl_mk_next + 1) % VGL_MK_RECS;
		r->surface = ds->depthData;
		r->valid = 0;
	}
	if (!loads)
		r->valid = 0;
	if (!r->valid)
		vgl_mk_stat[3]++;
	if (stores) {
		vgl_mk_cur = r;
	} else {
		vgl_mk_scene_copy = *r;
		vgl_mk_scene_copy.surface = NULL;
		r->valid = 0;
		vgl_mk_cur = &vgl_mk_scene_copy;
	}
}
// misc.c glClear: a clear that writes stencil in the open scene
void vgl_mk_touch(void) {
	if (vgl_mk_cur && vgl_mk_cur->valid) {
		vgl_mk_cur->valid = 0;
		vgl_mk_stat[2]++;
	}
}

/* MASKBG BEGIN */
// 1.3 MASK_BG (patch 0029; the Galaxy port's MASK_KEEP of its patch 0018 and, for the display's scene, its 0022; written again from their description): a scene that does not load
// its depth/stencil surface (the RT hint: VGL_RT_NOLOAD) begins from the surface's background mask, which is clear. With the scissor test off, update_scissor_test then draws a quad
// over the whole target that sets the mask everywhere. The background mask can be made "set" before the scene is begun (it is part of the surface the scene is begun with), and the
// quad is left out. With the scissor test on the background stays clear and only the region quad is drawn, as before. The value is set for EVERY unloaded scene (set or clear),
// whatever the switch says, so a surface that had it set while the switch was on is clear again when the switch is off. One use: the first update_scissor_test of the scene takes
// the mark; a later update (the scissor changed) draws as before.
// vglSetMaskBackground(mode): 0 (the default) leaves every quad drawn; bit 0 framebuffer-object scenes, bit 1 the display's scene (the present's).
int vgl_mask_bg = 0;
uint32_t vgl_mb_stat[2]; // unloaded scenes begun with the background mask set, quads left out
static const SceGxmDepthStencilSurface *vgl_mb_ds = NULL; // the surface of the scene that began with the mask set (NULL: none, or already taken)
void vglSetMaskBackground(int mode) {
	vgl_mask_bg = mode & 3;
}
void vglGetMaskBackgroundStats(uint32_t *armed, uint32_t *kept) {
	if (armed)
		*armed = vgl_mb_stat[0];
	if (kept)
		*kept = vgl_mb_stat[1];
}
// gxm.c scene_reset, right before sceGxmBeginScene, for the scene's depth/stencil surface and what the RT hint decided for it
void vgl_mb_scene_begin(SceGxmDepthStencilSurface *ds, uint32_t rt_applied) {
	vgl_mb_ds = NULL;
	if (!ds || !(rt_applied & VGL_RT_NOLOAD))
		return;
	const GLboolean set = (vgl_mask_bg & (is_rendering_display ? 2 : 1)) && !scissor_test_state;
	vglDepthStencilSurfaceSetBackgroundMask(ds, set ? 1 : 0);
	if (set) {
		vgl_mb_ds = ds;
		vgl_mb_stat[0]++;
	}
}
/* MASKBG END */

// Stencil Test
uint8_t stencil_mask_front = 0xFF; // Current in use mask for stencil test on front
uint8_t stencil_mask_back = 0xFF; // Current in use mask for stencil test on back
uint8_t stencil_mask_front_write = 0xFF; // Current in use mask for write stencil test on front
uint8_t stencil_mask_back_write = 0xFF; // Current in use mask for write stencil test on back
uint8_t stencil_ref_front = 0; // Current in use reference for stencil test on front
uint8_t stencil_ref_back = 0; // Current in use reference for stencil test on back
SceGxmStencilOp stencil_fail_front = SCE_GXM_STENCIL_OP_KEEP; // Current in use stencil operation when stencil test fails for front
SceGxmStencilOp depth_fail_front = SCE_GXM_STENCIL_OP_KEEP; // Current in use stencil operation when depth test fails for front
SceGxmStencilOp depth_pass_front = SCE_GXM_STENCIL_OP_KEEP; // Current in use stencil operation when depth test passes for front
SceGxmStencilOp stencil_fail_back = SCE_GXM_STENCIL_OP_KEEP; // Current in use stencil operation when stencil test fails for back
SceGxmStencilOp depth_fail_back = SCE_GXM_STENCIL_OP_KEEP; // Current in use stencil operation when depth test fails for back
SceGxmStencilOp depth_pass_back = SCE_GXM_STENCIL_OP_KEEP; // Current in use stencil operation when depth test passes for back
SceGxmStencilFunc stencil_func_front = SCE_GXM_STENCIL_FUNC_ALWAYS; // Current in use stencil function on front
SceGxmStencilFunc stencil_func_back = SCE_GXM_STENCIL_FUNC_ALWAYS; // Current in use stencil function on back
GLboolean stencil_test_state = GL_FALSE; // Current state for GL_STENCIL_TEST
GLint stencil_value = 0; // Current stencil test clear value

// Alpha Test
GLenum alpha_func = GL_ALWAYS; // Current in-use alpha test mode
GLfloat vgl_alpha_ref = 0.0f; // Current in use alpha test reference value
int alpha_op = ALWAYS; // Current in use alpha test operation
GLboolean alpha_test_state = GL_FALSE; // Current state for GL_ALPHA_TEST

inline __attribute__((always_inline)) void change_depth_write(SceGxmDepthWriteMode mode) {
	VGL_LC_SYNC();
	// Change depth write mode for both front and back primitives
	sceGxmSetFrontDepthWriteEnable(gxm_context, mode);
	sceGxmSetBackDepthWriteEnable(gxm_context, mode);
}

inline __attribute__((always_inline)) void change_depth_func() {
	VGL_LC_SYNC();
	// Setting depth function for both front and back primitives
	sceGxmSetFrontDepthFunc(gxm_context, depth_test_state ? depth_func : SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetBackDepthFunc(gxm_context, depth_test_state ? depth_func : SCE_GXM_DEPTH_FUNC_ALWAYS);

	// Calling an update for the depth write mode
	change_depth_write((depth_mask_state && depth_test_state) ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
}

inline __attribute__((always_inline)) void invalidate_depth_test() {
	// Invalidating current depth test state
	orig_depth_test = depth_test_state;
	depth_test_state = GL_FALSE;

	// Invoking a depth function update
	change_depth_func();
}

inline __attribute__((always_inline)) void validate_depth_test() {
	// Restoring original depth test state
	depth_test_state = orig_depth_test;

	// Invoking a depth function update
	change_depth_func();
}

inline __attribute__((always_inline)) void invalidate_viewport() {
	VGL_LC_SYNC();
	// Invalidating current viewport
	if (is_rendering_display) {
		vglSetViewport(gxm_context, fullscreen_x_port, fullscreen_x_scale, fullscreen_y_port, fullscreen_y_scale, fullscreen_z_port, fullscreen_z_scale);
	} else {
		skip_viewport_override = GL_TRUE;
		glViewport(0, 0, in_use_framebuffer->width, in_use_framebuffer->height);
	}
}

inline __attribute__((always_inline)) void validate_viewport() {
	VGL_LC_SYNC();
	// Restoring original viewport
	if (is_rendering_display) {
		vglSetViewport(gxm_context, x_port, x_scale, y_port, y_scale, z_port, z_scale);
	} else {
		skip_viewport_override = GL_TRUE;
		glViewport(gl_viewport.x, gl_viewport.y, gl_viewport.w, gl_viewport.h);
	}
}

inline __attribute__((always_inline)) void refresh_stencil_settings() {
	VGL_LC_SYNC();
	if (stencil_test_state) {
		// Setting stencil function for both front and back primitives
		sceGxmSetFrontStencilFunc(gxm_context,
			stencil_func_front,
			stencil_fail_front,
			depth_fail_front,
			depth_pass_front,
			stencil_mask_front, stencil_mask_front_write);
		sceGxmSetBackStencilFunc(gxm_context,
			stencil_func_back,
			stencil_fail_back,
			depth_fail_back,
			depth_pass_back,
			stencil_mask_back, stencil_mask_back_write);

		// Setting stencil ref for both front and back primitives
		sceGxmSetFrontStencilRef(gxm_context, stencil_ref_front);
		sceGxmSetBackStencilRef(gxm_context, stencil_ref_back);
	} else {
		sceGxmSetFrontStencilFunc(gxm_context,
			SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			0, 0);
		sceGxmSetBackStencilFunc(gxm_context,
			SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			0, 0);
	}
}

inline __attribute__((always_inline)) GLboolean change_stencil_config(SceGxmStencilOp *cfg, GLenum new) {
	// Translating openGL stencil operation value to sceGxm one
	switch (new) {
	case GL_KEEP:
		*cfg = SCE_GXM_STENCIL_OP_KEEP;
		break;
	case GL_ZERO:
		*cfg = SCE_GXM_STENCIL_OP_ZERO;
		break;
	case GL_REPLACE:
		*cfg = SCE_GXM_STENCIL_OP_REPLACE;
		break;
	case GL_INCR:
		*cfg = SCE_GXM_STENCIL_OP_INCR;
		break;
	case GL_INCR_WRAP:
		*cfg = SCE_GXM_STENCIL_OP_INCR_WRAP;
		break;
	case GL_DECR:
		*cfg = SCE_GXM_STENCIL_OP_DECR;
		break;
	case GL_DECR_WRAP:
		*cfg = SCE_GXM_STENCIL_OP_DECR_WRAP;
		break;
	case GL_INVERT:
		*cfg = SCE_GXM_STENCIL_OP_INVERT;
		break;
	default:
		return GL_FALSE;
	}
	return GL_TRUE;
}

inline __attribute__((always_inline)) GLboolean change_stencil_func_config(SceGxmStencilFunc *cfg, GLenum new) {
	// Translating openGL stencil function to sceGxm one
	switch (new) {
	case GL_NEVER:
		*cfg = SCE_GXM_STENCIL_FUNC_NEVER;
		break;
	case GL_LESS:
		*cfg = SCE_GXM_STENCIL_FUNC_LESS;
		break;
	case GL_LEQUAL:
		*cfg = SCE_GXM_STENCIL_FUNC_LESS_EQUAL;
		break;
	case GL_GREATER:
		*cfg = SCE_GXM_STENCIL_FUNC_GREATER;
		break;
	case GL_GEQUAL:
		*cfg = SCE_GXM_STENCIL_FUNC_GREATER_EQUAL;
		break;
	case GL_EQUAL:
		*cfg = SCE_GXM_STENCIL_FUNC_EQUAL;
		break;
	case GL_NOTEQUAL:
		*cfg = SCE_GXM_STENCIL_FUNC_NOT_EQUAL;
		break;
	case GL_ALWAYS:
		*cfg = SCE_GXM_STENCIL_FUNC_ALWAYS;
		break;
	default:
		return GL_FALSE;
	}
	return GL_TRUE;
}

inline __attribute__((always_inline)) void update_alpha_test_settings() {
	ffp_dirty_frag = GL_TRUE;

	// Translating openGL alpha test operation to internal one
	if (alpha_test_state) {
		switch (alpha_func) {
		case GL_EQUAL:
			alpha_op = EQUAL;
			break;
		case GL_LEQUAL:
			alpha_op = LESS_EQUAL;
			break;
		case GL_GEQUAL:
			alpha_op = GREATER_EQUAL;
			break;
		case GL_LESS:
			alpha_op = LESS;
			break;
		case GL_GREATER:
			alpha_op = GREATER;
			break;
		case GL_NOTEQUAL:
			alpha_op = NOT_EQUAL;
			break;
		case GL_NEVER:
			alpha_op = NEVER;
			break;
		default:
			alpha_op = ALWAYS;
			break;
		}
	} else {
		alpha_op = ALWAYS;
	}
}

void update_scissor_test() {
	const float scissor_depth = 1.0f;

	// Setting current vertex program to clear screen one and fragment program to scissor test one
	sceGxmSetVertexProgram(gxm_context, clear_vertex_program_patched);
	sceGxmSetFragmentProgram(gxm_context, scissor_test_fragment_program);

	// Invalidating viewport and culling
	invalidate_viewport();
	sceGxmSetCullMode(gxm_context, SCE_GXM_CULL_NONE);
	
	uint32_t active_w, active_h;
	if (is_rendering_display) {
		active_w = DISPLAY_WIDTH;
		active_h = DISPLAY_HEIGHT;
	} else {
		active_w = in_use_framebuffer->width;
		active_h = in_use_framebuffer->height;		
	}
	
	// Converting openGL scissor test region to sceGxm one
	region.x = region.gl_x < 0 ? 0 : region.gl_x;
	region.w = region.gl_w;
	region.h = region.gl_h;
	region.y = active_h - region.gl_y - region.gl_h;
	
	// Optimizing region
	if (region.y < 0)
		region.y = 0;
	if (region.x + region.w > active_w)
		region.w = active_w - region.x;
	if (region.y + region.h > active_h)
		region.h = active_h - region.y;

#ifndef DISABLE_TILE_CLIPPER
	// Invalidating internal tile based region clip
	sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, active_w - 1, active_h - 1);
#endif

	// MASK_KEEP (patch 0026): the surface holds this very mask already: neither quad is drawn (every state set and restore around them stays as it was)
	const GLboolean mk_same = vgl_mk_cur && vgl_mk_cur->valid && vgl_mk_cur->on == (scissor_test_state ? 1 : 0) && vgl_mk_cur->disp == (is_rendering_display ? 1 : 0) &&
		vgl_mk_cur->w == active_w && vgl_mk_cur->h == active_h &&
		(!scissor_test_state || (vgl_mk_cur->x == region.x && vgl_mk_cur->y == region.y && vgl_mk_cur->rw == region.w && vgl_mk_cur->rh == region.h));
	if (vgl_mask_keep && mk_same && !vgl_rt_skip_invalidation) {
		vgl_mk_stat[1]++;
		if (scissor_test_state)
			vector4f_convert_to_local_space(scissor_test_vertices, region.x, region.y, region.w, region.h);
		goto mask_done;
	}
	vgl_mk_stat[0]++;
	// MASK_CLIP (patch 0055): the region the surface's mask holds now, if the record says so (read before the record is replaced below)
	GLboolean mc_have = GL_FALSE;
	int mc_x = 0, mc_y = 0, mc_w = 0, mc_h = 0;
	if (vgl_mask_clip && scissor_test_state && vgl_mk_cur && vgl_mk_cur->valid && vgl_mk_cur->on && vgl_mk_cur->disp == (is_rendering_display ? 1 : 0) &&
		vgl_mk_cur->w == active_w && vgl_mk_cur->h == active_h && vgl_mk_cur->rw > 0 && vgl_mk_cur->rh > 0 && vgl_mk_cur->x >= 0 && vgl_mk_cur->y >= 0 &&
		vgl_mk_cur->x + vgl_mk_cur->rw <= (int)active_w && vgl_mk_cur->y + vgl_mk_cur->rh <= (int)active_h) {
		mc_have = GL_TRUE;
		mc_x = vgl_mk_cur->x;
		mc_w = vgl_mk_cur->rw;
		mc_h = vgl_mk_cur->rh;
#ifndef HAVE_UNFLIPPED_FBOS
		mc_y = is_rendering_display ? vgl_mk_cur->y : vgl_mk_cur->gly; // (the tile clip of the final update below: a framebuffer object's in GL rows)
#else
		mc_y = vgl_mk_cur->y;
#endif
		if (mc_y < 0 || mc_y >= (int)active_h || mc_h <= 0)
			mc_have = GL_FALSE;
		else if (mc_y + mc_h > (int)active_h)
			mc_h = (int)active_h - mc_y;
	}
	if (vgl_mk_cur) {
		vgl_mk_cur->on = scissor_test_state ? 1 : 0;
		vgl_mk_cur->disp = is_rendering_display ? 1 : 0;
		vgl_mk_cur->w = active_w;
		vgl_mk_cur->h = active_h;
		vgl_mk_cur->x = region.x;
		vgl_mk_cur->y = region.y;
		vgl_mk_cur->rw = region.w;
		vgl_mk_cur->rh = region.h;
		// int26 (Carbon #4): if GL y was out-of-bounds (< 0 or >= active_h) or extended past active_h, invalidate FBO clip height mismatch by marking gly = -1 so next rebuild falls back to whole-target clear safely.
		vgl_mk_cur->gly = (region.gl_y >= 0 && region.gl_y + region.h <= (int)active_h) ? region.gl_y : -1;
		vgl_mk_cur->valid = 1;
	}
	
#ifdef STORE_DEPTH_STENCIL
	if (vgl_rt_skip_invalidation) {
		// RT (rt.c): the scene did not load its depth/stencil surface: the mask is the background's (0) everywhere, nothing to invalidate
		vgl_rt_skip_invalidation = GL_FALSE;
		vgl_rt_invalidations_skipped++;
		if (scissor_test_state) {
			vector4f_convert_to_local_space(scissor_test_vertices, region.x, region.y, region.w, region.h);
		} else if (vgl_mb_ds) {
			// MASK_BG (patch 0029): the scene began with the background mask set (vgl_mb_scene_begin: scissor test off, nothing loaded), which is what the quad below would make
			vgl_mb_ds = NULL;
			vgl_mb_stat[1]++;
			goto mask_done;
		}
	} else if (scissor_test_state) {
#else
	if (skip_scissor_invalidation) {
		skip_scissor_invalidation = GL_FALSE;
		if (scissor_test_state) {
			// Calculating scissor test region vertices
			vector4f_convert_to_local_space(scissor_test_vertices, region.x, region.y, region.w, region.h);
		}
	} else if (scissor_test_state) {
#endif
		// Calculating scissor test region vertices
		vector4f_convert_to_local_space(scissor_test_vertices, region.x, region.y, region.w, region.h);

#ifndef DISABLE_TILE_CLIPPER
		// MASK_CLIP (patch 0055): the clear reaches only the region the mask holds now (the tile clip rounds outward; the bit is clear outside the region already)
		if (mc_have) {
			sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE, mc_x, mc_y, mc_x + mc_w - 1, mc_y + mc_h - 1);
			vgl_mc_stat[0]++;
			vgl_mc_stat[1] += ((uint32_t)active_w * active_h - (uint32_t)mc_w * mc_h) / 1000;
		}
#endif
		sceGxmSetPrecomputedVertexState(gxm_context, &scissor_clear_vertex_state);

		// Cleaning stencil surface mask update bit on the whole screen
		sceGxmSetFrontStencilFunc(gxm_context,
			SCE_GXM_STENCIL_FUNC_NEVER,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			0, 0);
		sceGxmSetBackStencilFunc(gxm_context,
			SCE_GXM_STENCIL_FUNC_NEVER,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP,
			0, 0);
		sceGxmDrawPrecomputed(gxm_context, &clear_draw_state);

		sceGxmSetPrecomputedVertexState(gxm_context, NULL);
#ifndef DISABLE_TILE_CLIPPER
		if (mc_have)
			sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, active_w - 1, active_h - 1); // (the set quad below is the new region: no clip of the old one)
#endif
	}

	// Setting stencil surface mask update bit on the scissor test region
	sceGxmSetFrontStencilFunc(gxm_context,
		SCE_GXM_STENCIL_FUNC_ALWAYS,
		SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP,
		0, 0);
	sceGxmSetBackStencilFunc(gxm_context,
		SCE_GXM_STENCIL_FUNC_ALWAYS,
		SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP,
		0, 0);

	void *vertex_buffer;
	sceGxmReserveVertexDefaultUniformBuffer(gxm_context, &vertex_buffer);
	vglSetUniformData((uint8_t *)vertex_buffer + clear_position_offset, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 4, scissor_test_state ? &scissor_test_vertices->x : &clear_vertices->x, SCE_GXM_PARAMETER_TYPE_F32);
	vglSetUniformData((uint8_t *)vertex_buffer + clear_depth_offset, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 1, &scissor_depth, SCE_GXM_PARAMETER_TYPE_F32);

	sceGxmDrawPrecomputed(gxm_context, &clear_draw_state);

mask_done:
	// Restoring viewport and culling
	validate_viewport();
	change_cull_mode();

#ifndef DISABLE_TILE_CLIPPER
	// Reducing GPU workload by performing tile granularity clipping
	if (scissor_test_state) {
#ifndef HAVE_UNFLIPPED_FBOS
		if (!is_rendering_display) {
			uint32_t y = region.gl_y > 0 ? region.gl_y : 0;
			sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE, region.x, y, region.x + region.w - 1, y + region.h - 1);
		} else
#endif
			sceGxmSetRegionClip(gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE, region.x, region.y, region.x + region.w - 1, region.y + region.h - 1);
	}
#endif

	// Restoring original stencil test settings
	refresh_stencil_settings();

	vglRestoreVertexUniformBuffer();
	dirty_scissor_state = GL_FALSE;
}

void reset_scissor_test_region(void) {
	// Setting scissor test region to default values
	region.x = region.y = region.gl_x = region.gl_y = 0;
	region.w = region.gl_w = DISPLAY_WIDTH;
	region.h = region.gl_h = DISPLAY_HEIGHT;
}

/*
 * ------------------------------
 * - IMPLEMENTATION STARTS HERE -
 * ------------------------------
 */

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	// Error handling
	if ((width < 0) || (height < 0)) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	region.gl_x = x;
	region.gl_y = y;
	region.gl_w = width;
	region.gl_h = height;

	if (scissor_test_state) {
		dirty_scissor_state = GL_TRUE;
	}
}

void glDepthFunc(GLenum func) {
	THREAD_SAFE()

	// Properly translating openGL function to sceGxm one
	switch (func) {
	case GL_NEVER:
		depth_func = SCE_GXM_DEPTH_FUNC_NEVER;
		break;
	case GL_LESS:
		depth_func = SCE_GXM_DEPTH_FUNC_LESS;
		break;
	case GL_EQUAL:
		depth_func = SCE_GXM_DEPTH_FUNC_EQUAL;
		break;
	case GL_LEQUAL:
		depth_func = SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
		break;
	case GL_GREATER:
		depth_func = SCE_GXM_DEPTH_FUNC_GREATER;
		break;
	case GL_NOTEQUAL:
		depth_func = SCE_GXM_DEPTH_FUNC_NOT_EQUAL;
		break;
	case GL_GEQUAL:
		depth_func = SCE_GXM_DEPTH_FUNC_GREATER_EQUAL;
		break;
	case GL_ALWAYS:
		depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, func);
	}

	// Updating in use depth function
	change_depth_func();
}

void glClearDepth(GLdouble depth) {
	THREAD_SAFE()

	// Set current in use depth test depth value
	depth_value = depth;
}

void glClearDepthf(GLclampf depth) {
	THREAD_SAFE()

	// Set current in use depth test depth value
	depth_value = depth;
}

void glClearDepthx(GLclampx depth) {
	THREAD_SAFE()

	// Set current in use depth test depth value
	depth_value = (float)depth / 65536.0f;
}

void glDepthMask(GLboolean flag) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	// Error handling
	if (phase == MODEL_CREATION) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	}
#endif

	// Set current in use depth mask and invoking a depth write mode update
	depth_mask_state = flag;
	change_depth_write((depth_mask_state && depth_test_state) ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
}

void glAlphaFunc(GLenum func, GLfloat ref) {
	THREAD_SAFE()

	// Updating in use alpha test parameters
	alpha_func = func;
	vgl_alpha_ref = ref;
	update_alpha_test_settings();
}

void glAlphaFuncx(GLenum func, GLfixed ref) {
	THREAD_SAFE()

	// Updating in use alpha test parameters
	alpha_func = func;
	vgl_alpha_ref = (float)ref / 65536.0f;
	update_alpha_test_settings();
}

void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
	THREAD_SAFE()

	// Properly updating stencil operation settings
	switch (face) {
	case GL_FRONT:
		if (!change_stencil_config(&stencil_fail_front, sfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, sfail)
		}
		if (!change_stencil_config(&depth_fail_front, dpfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dpfail)
		}
		if (!change_stencil_config(&depth_pass_front, dppass)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dppass)
		}
		break;
	case GL_BACK:
		if (!change_stencil_config(&stencil_fail_back, sfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, sfail)
		}
		if (!change_stencil_config(&depth_fail_back, dpfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dpfail)
		}
		if (!change_stencil_config(&depth_pass_back, dppass)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dppass)
		}
		break;
	case GL_FRONT_AND_BACK:
		if (!change_stencil_config(&stencil_fail_front, sfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, sfail)
		}
		if (!change_stencil_config(&stencil_fail_back, sfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, sfail)
		}
		if (!change_stencil_config(&depth_fail_front, dpfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dpfail)
		}
		if (!change_stencil_config(&depth_fail_back, dpfail)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dpfail)
		}
		if (!change_stencil_config(&depth_pass_front, dppass)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dppass)
		}
		if (!change_stencil_config(&depth_pass_back, dppass)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dppass)
		}
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, face)
	}
	refresh_stencil_settings();
}

void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) {
	THREAD_SAFE()

	glStencilOpSeparate(GL_FRONT_AND_BACK, sfail, dpfail, dppass);
}

void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
	THREAD_SAFE()

	// Properly updating stencil test function settings
	switch (face) {
	case GL_FRONT:
		if (!change_stencil_func_config(&stencil_func_front, func)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, func)
		}
		stencil_mask_front = mask;
		stencil_ref_front = ref;
		break;
	case GL_BACK:
		if (!change_stencil_func_config(&stencil_func_back, func)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, func)
		}
		stencil_mask_back = mask;
		stencil_ref_back = ref;
		break;
	case GL_FRONT_AND_BACK:
		if (!change_stencil_func_config(&stencil_func_front, func)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, func)
		}
		if (!change_stencil_func_config(&stencil_func_back, func)) {
			SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, func)
		}
		stencil_mask_front = stencil_mask_back = mask;
		stencil_ref_front = stencil_ref_back = ref;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, face)
	}
	refresh_stencil_settings();
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask) {
	THREAD_SAFE()

	glStencilFuncSeparate(GL_FRONT_AND_BACK, func, ref, mask);
}

void glStencilMaskSeparate(GLenum face, GLuint mask) {
	THREAD_SAFE()

	// Properly updating stencil test mask settings
	switch (face) {
	case GL_FRONT:
		stencil_mask_front_write = mask;
		break;
	case GL_BACK:
		stencil_mask_back_write = mask;
		break;
	case GL_FRONT_AND_BACK:
		stencil_mask_front_write = stencil_mask_back_write = mask;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, face)
	}
	refresh_stencil_settings();
}

void glStencilMask(GLuint mask) {
	THREAD_SAFE()

	glStencilMaskSeparate(GL_FRONT_AND_BACK, mask);
}

void glClearStencil(GLint s) {
	THREAD_SAFE()

	stencil_value = s;
}
