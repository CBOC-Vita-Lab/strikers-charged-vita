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
 * vgl.c:
 * Implementation for custom vitaGL functions
 */
#include "shared.h"
#include "texture_callbacks.h"
#include "vitaGL.h"

static GLboolean vgl_inited = GL_FALSE;

extern int unsafe_allocator_counter;
void *gpu_alloc_mapped_aligned_unsafe_for_cpu(size_t alignment, size_t size);

#ifdef HAVE_SHADER_CACHE
static char shader_cache_root[128] = {};
#endif

#ifdef HAVE_PROFILING
size_t max_vcache_size = 0;
size_t max_fcache_size = 0;
#endif

#ifdef HAVE_SOFTFP_ABI
__attribute__((naked)) void sceGxmSetViewport_sfp(SceGxmContext *context, float xOffset, float xScale, float yOffset, float yScale, float zOffset, float zScale) {
	asm volatile(
		"vmov s0, r1\n"
		"vmov s1, r2\n"
		"vmov s2, r3\n"
		"ldr r1, [sp]\n"
		"ldr r2, [sp, #4]\n"
		"ldr r3, [sp, #8]\n"
		"vmov s3, r1\n"
		"vmov s4, r2\n"
		"vmov s5, r3\n"
		"b sceGxmSetViewport\n");
}
#endif

// Precompiled clear shaders
#include "shaders/precompiled_clear_f.h"
#include "shaders/precompiled_clear_v.h"

// Precompiled blit shaders
#include "shaders/precompiled_blit_f.h"
#include "shaders/precompiled_blit_v.h"

SceGxmShaderPatcherId clear_vertex_id;
SceGxmShaderPatcherId clear_fragment_id;
const SceGxmProgramParameter *clear_position;
const SceGxmProgramParameter *clear_depth;
const SceGxmProgramParameter *clear_color;
SceGxmVertexProgram *clear_vertex_program_patched;
SceGxmFragmentProgram *clear_fragment_program_patched;
SceGxmFragmentProgram *clear_fragment_program_float_patched;

SceGxmShaderPatcherId blit_vertex_id;
SceGxmShaderPatcherId blit_fragment_id;
SceGxmVertexProgram *blit_vertex_program_patched;
SceGxmFragmentProgram *blit_fragment_program_patched;
SceGxmFragmentProgram *blit_fragment_program_float_patched;
const SceGxmProgramParameter *blit_position;
const SceGxmProgramParameter *blit_texcoord;

vector4f *clear_vertices = NULL; // Memblock starting address for clear screen vertices
vector3f *depth_vertices = NULL; // Memblock starting address for depth clear screen vertices
SceGxmPrecomputedVertexState scissor_clear_vertex_state; // Precomputed vertex state for the fullscreen mask clear
SceGxmPrecomputedDraw clear_draw_state; // Precomputed draw state for fullscreen clear draws
SceGxmPrecomputedDraw blit_draw_state; // Precomputed draw state for framebuffer blits
uint32_t clear_position_offset;
uint32_t clear_depth_offset;
uint32_t blit_position_offset;
uint32_t blit_texcoord_offset;
static void *scissor_clear_vertex_state_mem = NULL;
static void *scissor_clear_uniform_buffer = NULL;
static void *clear_draw_state_mem = NULL;
static void *blit_draw_state_mem = NULL;

// sceGxm viewport setup (NOTE: origin is on center screen)
float x_port = 480.0f;
float y_port = 272.0f;
float z_port = 0.5f;
float x_scale = 480.0f;
float y_scale = -272.0f;
float z_scale = 0.5f;

// Fullscreen sceGxm viewport (NOTE: origin is on center screen)
float fullscreen_x_port = 480.0f;
float fullscreen_y_port = 272.0f;
float fullscreen_z_port = 0.5f;
float fullscreen_x_scale = 480.0f;
float fullscreen_y_scale = -272.0f;
float fullscreen_z_scale = 0.5f;

uint32_t vsync_interval = 1; // Current setting for VSync

// Disable color buffer shader
uint16_t *depth_clear_indices = NULL; // Memblock starting address for clear screen indices

// Internal stuffs
SceGxmMultisampleMode msaa_mode = SCE_GXM_MULTISAMPLE_NONE;
extern GLboolean use_vram_for_usse;

uint16_t *default_idx_ptr; // sceGxm mapped progressive indices buffer
uint16_t *default_quads_idx_ptr; // sceGxm mapped progressive indices buffer for quads
uint16_t *default_line_strips_idx_ptr; // sceGxm mapped progressive indices buffer for line strips

// Internal functions
void update_scissor_test_uniforms(void) {
	const float scissor_depth = 1.0f;
	vglSetUniformData((uint8_t *)scissor_clear_uniform_buffer + clear_position_offset, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 4, &clear_vertices->x, SCE_GXM_PARAMETER_TYPE_F32);
	vglSetUniformData((uint8_t *)scissor_clear_uniform_buffer + clear_depth_offset, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 1, &scissor_depth, SCE_GXM_PARAMETER_TYPE_F32);
}

#ifndef DISABLE_CIRCULAR_POOL
#define CIRCULAR_POOL_SIZE_DEF (32 * 1024 * 1024) // Default size in bytes for the circular vertex pool
#ifdef HAVE_SCRATCH_MEMORY
GLboolean vgl_dynamic_wants_scratch = GL_TRUE;
GLboolean vgl_stream_wants_scratch = GL_TRUE;
#endif
#ifndef CIRCULAR_POOL_SPEEDHACK
uint8_t *circular_data_pool[DISPLAY_MAX_BUFFER_COUNT];
uint8_t *circular_data_pool_ptr[DISPLAY_MAX_BUFFER_COUNT];
uint8_t *circular_data_pool_limit[DISPLAY_MAX_BUFFER_COUNT];
int vgl_circular_idx = 0;
#else
static uint8_t *circular_data_pool;
static uint8_t *circular_data_pool_ptr;
static uint8_t *circular_data_pool_limit;
#endif
uint32_t circular_data_pool_size = CIRCULAR_POOL_SIZE_DEF;
// GLB: every reservation is rounded up to 16 bytes, so the next one starts on a 16-byte boundary. Upstream added `size` as it came: an odd-sized U16 index
// upload (2 bytes per index) left the next float vertex data unaligned and the NEON copy took a data abort (strikersVita found it).
uint32_t vgl_glb_pool_overruns = 0, vgl_glb_pool_overrun_bytes = 0, vgl_glb_pool_frame_peak = 0, vgl_glb_pool_peak = 0, vgl_glb_pool_odd = 0;
uint8_t *vgl_reserve_data_pool(uint32_t size) {
	if (size & 3u)
		vgl_glb_pool_odd++; // upstream: the next reservation (float vertex data) started 2 bytes off a 4-byte boundary after an odd-sized U16 index upload (counted: vglGlbStats.pool_odd)
	size = (size + 15u) & ~15u;
#ifndef CIRCULAR_POOL_SPEEDHACK
	uint8_t *res = circular_data_pool_ptr[vgl_circular_idx];
	circular_data_pool_ptr[vgl_circular_idx] += size;
	if (circular_data_pool_ptr[vgl_circular_idx] > circular_data_pool_limit[vgl_circular_idx]) {
		vgl_glb_pool_overruns++;
		vgl_glb_pool_overrun_bytes += size;
		res = (uint8_t *)gpu_alloc_mapped_for_cpu(size);
#ifdef LOG_ERRORS
		if (!res) {
			vgl_log("%s:%d gpu_alloc_mapped_for_cpu failed with a requested size of %u bytes.\n", __FILE__, __LINE__, size);
		}
#endif
		mark_as_dirty(res);
	}
#else
#ifdef LOG_ERRORS
	size_t pool_size = (size_t)circular_data_pool_limit - (size_t)circular_data_pool;
	if (size > pool_size) {
		vgl_log("%s:%d Attempting to alloc %u bytes in the circular pool but the pool is only %u bytes. You must increase its size with vglSetCircularPoolSize.\n", __FILE__, __LINE__, size, pool_size);
	}
#endif
	uint8_t *res = circular_data_pool_ptr;
	circular_data_pool_ptr += size;
	if (circular_data_pool_ptr > circular_data_pool_limit) {
		circular_data_pool_ptr = circular_data_pool + size;
		return circular_data_pool;
	}
#endif
	return res;
}
#endif

/*
 * ------------------------------
 * - IMPLEMENTATION STARTS HERE -
 * ------------------------------
 */

void vglUseVramForUSSE(GLboolean usage) {
	use_vram_for_usse = usage;
}

GLboolean vglInitWithCustomSizes(int pool_size, int width, int height, int ram_pool_size, int cdram_pool_size, int phycont_pool_size, int cdlg_pool_size, SceGxmMultisampleMode msaa) {
	// Check if vitaGL has been already inited
	if (vgl_inited) {
		vgl_log("%s:%d: Suppressed an attempt at initing vitaGL while it's already inited.\n", __FILE__, __LINE__);
		return GL_FALSE;
	}

#ifdef DEBUG_THREAD_SAFENESS
	vgl_is_main_thread = GL_TRUE;
#endif

#if defined(HAVE_SHADER_CACHE) || defined(HAVE_TEX_CACHE)
	char titleid[12] = {0};
	if (sceAppMgrAppParamGetString(0, 12, titleid, sizeof(titleid)) < 0 || !titleid[0])
		strcpy(titleid, "UNKNOWN");
	titleid[sizeof(titleid) - 1] = 0;
#endif
#ifdef HAVE_TEX_CACHE
	sceIoMkdir("ux0:data/vgl_cache", 0777);
	sprintf(vgl_file_cache_path, "ux0:data/vgl_cache/%s", titleid);
	sceIoMkdir(vgl_file_cache_path, 0777);
#endif
	sceIoMkdir("ux0:data/shader_cache", 0777);
	char fname[256];
	sprintf(fname, "ux0:data/shader_cache/v%d", FFP_SHADER_CACHE_MAGIC);
	sceIoMkdir(fname, 0777);
	sprintf(fname, "ux0:data/shader_cache/v%d/v", FFP_SHADER_CACHE_MAGIC);
	sceIoMkdir(fname, 0777);
	sprintf(fname, "ux0:data/shader_cache/v%d/f", FFP_SHADER_CACHE_MAGIC);
	sceIoMkdir(fname, 0777);
#ifdef HAVE_SHADER_CACHE
	if (!shader_cache_root[0])
		sprintf(shader_cache_root, "ux0:data/shader_cache/%s", titleid);
	sceIoMkdir(shader_cache_root, 0777);
	sprintf(vgl_shader_cache_path, "%s/v%d", shader_cache_root, SHADER_CACHE_MAGIC);
	sceIoMkdir(vgl_shader_cache_path, 0777);
	sprintf(fname, "%s/v", vgl_shader_cache_path);
	sceIoMkdir(fname, 0777);
	sprintf(fname, "%s/f", vgl_shader_cache_path);
	sceIoMkdir(fname, 0777);
#endif
	// Check if framebuffer size is valid
	GLboolean res_fallback = GL_FALSE;
	int max_w = width, max_h = height;
	sceDisplayGetMaximumFrameBufResolution(&max_w, &max_h);
	if (width > max_w || height > max_h) {
		width = max_w;
		height = max_h;
		res_fallback = GL_TRUE;
	}

	// Setting our display size
	msaa_mode = msaa;
	DISPLAY_WIDTH = width;
	DISPLAY_HEIGHT = height;
	DISPLAY_WIDTH_FLOAT = width * 1.0f;
	DISPLAY_HEIGHT_FLOAT = height * 1.0f;
	DISPLAY_STRIDE = VGL_ALIGN(DISPLAY_WIDTH, 64);

	// Adjusting default values for internal viewport
	x_port = DISPLAY_WIDTH_FLOAT / 2.0f;
	x_scale = x_port;
	y_scale = -(DISPLAY_HEIGHT_FLOAT / 2.0f);
	y_port = -y_scale;
	fullscreen_x_port = x_port;
	fullscreen_x_scale = x_scale;
	fullscreen_y_port = y_port;
	fullscreen_y_scale = y_scale;

	// Init viewport state
	gl_viewport.x = 0;
	gl_viewport.y = 0;
	gl_viewport.w = DISPLAY_WIDTH;
	gl_viewport.h = DISPLAY_HEIGHT;

	// Initializing sceGxm
	init_gxm();

	// Initializing memory heaps for CDRAM and RAM memory (both standard and physically contiguous)
	vgl_mem_init(ram_pool_size, cdram_pool_size, phycont_pool_size, cdlg_pool_size);

	// Initializing sceGxm context
	init_gxm_context(&gxm_context, VGL_CONTEXT_MAIN);

	// Creating render target for the display
	create_display_render_target();

	// Creating color surfaces for the display
	init_display_color_surfaces(GL_FALSE);

	// Creating depth and stencil surfaces for the display
	init_display_depth_stencil_surfaces();

	// Starting a sceGxmShaderPatcher instance
	start_shader_patcher();

	// Setting up default blending state
	change_blend_mask();

	clear_vertices = gpu_alloc_mapped_for_cpu(sizeof(vector4f));
	depth_clear_indices = gpu_alloc_mapped_for_cpu(4 * sizeof(unsigned short));

	vector4f_convert_to_local_space(clear_vertices, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

	depth_clear_indices[0] = 0;
	depth_clear_indices[1] = 1;
	depth_clear_indices[2] = 2;
	depth_clear_indices[3] = 3;
	
	// Clear shader register
	SceGxmProgram *gxm_program_clear_v = (SceGxmProgram *)&clear_v;
	SceGxmProgram *gxm_program_clear_f = (SceGxmProgram *)&clear_f;
	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_clear_v, &clear_vertex_id);
	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_clear_f, &clear_fragment_id);

	clear_position = sceGxmProgramFindParameterByName(gxm_program_clear_v, "position");
	clear_depth = sceGxmProgramFindParameterByName(gxm_program_clear_v, "u_clear_depth");
	clear_color = sceGxmProgramFindParameterByName(gxm_program_clear_f, "u_clear_color");
	clear_position_offset = sceGxmProgramParameterGetResourceIndex(clear_position) * 4;
	clear_depth_offset = sceGxmProgramParameterGetResourceIndex(clear_depth) * 4;
	{ patch_vertex_program(clear_vertex_id, NULL, 0, NULL, 0, &clear_vertex_program_patched); }
	{ patch_fragment_program(clear_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, msaa_mode, NULL, NULL, &clear_fragment_program_patched); }
	{ patch_fragment_program(clear_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4, msaa_mode, NULL, NULL, &clear_fragment_program_float_patched); }

	// Prepare precomputed data for scissor test region update draws
	scissor_clear_uniform_buffer = gpu_alloc_mapped_for_cpu(sceGxmProgramGetDefaultUniformBufferSize(gxm_program_clear_v));
	update_scissor_test_uniforms();
	scissor_clear_vertex_state_mem = gpu_alloc_mapped_for_cpu(sceGxmGetPrecomputedVertexStateSize(clear_vertex_program_patched));
	sceGxmPrecomputedVertexStateInit(&scissor_clear_vertex_state, clear_vertex_program_patched, scissor_clear_vertex_state_mem);
	sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(&scissor_clear_vertex_state, scissor_clear_uniform_buffer);
	clear_draw_state_mem = gpu_alloc_mapped_for_cpu(sceGxmGetPrecomputedDrawSize(clear_vertex_program_patched));
	sceGxmPrecomputedDrawInit(&clear_draw_state, clear_vertex_program_patched, clear_draw_state_mem);
	sceGxmPrecomputedDrawSetParams(&clear_draw_state, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, depth_clear_indices, 4);

#ifndef SKIP_SPLASHSCREEN
	if (!system_app_mode)
		invoke_splashscreen();
#endif
	
	// Framebuffer blit shader register
	SceGxmProgram *gxm_program_blit_v = (SceGxmProgram *)&blit_v;
	SceGxmProgram *gxm_program_blit_f = (SceGxmProgram *)&blit_f;
	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_blit_v, &blit_vertex_id);
	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, gxm_program_blit_f, &blit_fragment_id);

	blit_position = sceGxmProgramFindParameterByName(gxm_program_blit_v, "position");
	blit_texcoord = sceGxmProgramFindParameterByName(gxm_program_blit_v, "texcoord");
	blit_position_offset = sceGxmProgramParameterGetResourceIndex(blit_position) * 4;
	blit_texcoord_offset = sceGxmProgramParameterGetResourceIndex(blit_texcoord) * 4;
	{ patch_vertex_program(blit_vertex_id, NULL, 0, NULL, 0, &blit_vertex_program_patched); }
	{ patch_fragment_program(blit_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, msaa_mode, NULL, NULL, &blit_fragment_program_patched); }
	{ patch_fragment_program(blit_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4, msaa_mode, NULL, NULL, &blit_fragment_program_float_patched); }
	blit_draw_state_mem = gpu_alloc_mapped_for_cpu(sceGxmGetPrecomputedDrawSize(blit_vertex_program_patched));
	sceGxmPrecomputedDrawInit(&blit_draw_state, blit_vertex_program_patched, blit_draw_state_mem);
	sceGxmPrecomputedDrawSetParams(&blit_draw_state, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, SCE_GXM_INDEX_FORMAT_U16, depth_clear_indices, 4);

	sceGxmSetTwoSidedEnable(gxm_context, SCE_GXM_TWO_SIDED_ENABLED);

	// Scissor Test shader register
	sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(gxm_shader_patcher, &scissor_test_fragment_program);

	scissor_test_vertices = gpu_alloc_mapped_for_cpu(1 * sizeof(vector4f));

	// Init texture units
	for (int i = 0; i < COMBINED_TEXTURE_IMAGE_UNITS_NUM; i++) {
		vgl_memset(&texture_units[i].env_color.r, 0, sizeof(vector4f));
		texture_units[i].env_mode = MODULATE;
		texture_units[i].tex_id[0] = 0;
		texture_units[i].tex_id[1] = 0;
		texture_units[i].tex_id[2] = 0;
		texture_units[i].state = 0;
		texture_units[i].texture_stack_counter = 0;
#ifndef DISABLE_TEXTURE_COMBINER
		texture_units[i].combiner.rgb_func = MODULATE;
		texture_units[i].combiner.a_func = MODULATE;
		texture_units[i].combiner.op_rgb_0 = TEXTURE;
		texture_units[i].combiner.op_rgb_1 = PREVIOUS;
		texture_units[i].combiner.op_rgb_2 = CONSTANT;
		texture_units[i].combiner.op_a_0 = TEXTURE;
		texture_units[i].combiner.op_a_1 = PREVIOUS;
		texture_units[i].combiner.op_a_2 = CONSTANT;
		texture_units[i].combiner.op_mode_rgb_0 = SRC_COLOR;
		texture_units[i].combiner.op_mode_rgb_1 = SRC_COLOR;
		texture_units[i].combiner.op_mode_rgb_2 = SRC_ALPHA;
		texture_units[i].combiner.op_mode_a_0 = SRC_ALPHA;
		texture_units[i].combiner.op_mode_a_1 = SRC_ALPHA;
		texture_units[i].combiner.op_mode_a_2 = SRC_ALPHA;
#endif
		texture_units[i].rgb_scale = 1.0f;
		texture_units[i].a_scale = 1.0f;
	}

	// Init custom shaders
	reset_custom_shaders();

	// Init default vao
	reset_vao(cur_vao);
	
	// Init occlusion queries
	reset_queries();
	
#ifdef HAVE_DLISTS
	// Init display lists
	reset_dlists();
#endif

#ifndef DISABLE_CIRCULAR_POOL
#ifndef CIRCULAR_POOL_SPEEDHACK
	for (int i = 0; i < gxm_display_buffer_count; i++) {
		circular_data_pool[i] = gpu_alloc_mapped_for_cpu(circular_data_pool_size / gxm_display_buffer_count);
#ifdef LOG_ERRORS
		if (vgl_mem_get_type_by_addr(circular_data_pool[i]) == VGL_MEM_VRAM) {
			vgl_log("%s:%d %s: Circular pool #%d spilled into VRAM. This might negatively impact performance.\n", __FILE__, __LINE__, __func__, i);
		}
#endif
		circular_data_pool_ptr[i] = circular_data_pool[i];
		circular_data_pool_limit[i] = (uint8_t *)circular_data_pool[i] + circular_data_pool_size / gxm_display_buffer_count;
	}
#else
	circular_data_pool = gpu_alloc_mapped_for_cpu(circular_data_pool_size);
#ifdef LOG_ERRORS
	if (vgl_mem_get_type_by_addr(circular_data_pool) == VGL_MEM_VRAM) {
		vgl_log("%s:%d %s: Circular pool spilled into VRAM. This might negatively impact performance.\n", __FILE__, __LINE__, __func__);
	}
#endif
	circular_data_pool_ptr = circular_data_pool;
	circular_data_pool_limit = (uint8_t *)circular_data_pool + circular_data_pool_size;
#endif
#ifndef CIRCULAR_POOL_SPEEDHACK
	vgl_stream_init(gxm_display_buffer_count);
#endif
#endif

	// Init constant index buffers
	default_idx_ptr = (uint16_t *)gpu_alloc_mapped_for_cpu(MAX_IDX_NUMBER * sizeof(uint16_t));
	default_quads_idx_ptr = (uint16_t *)gpu_alloc_mapped_for_cpu(MAX_IDX_NUMBER * sizeof(uint16_t));
	default_line_strips_idx_ptr = (uint16_t *)gpu_alloc_mapped_for_cpu(MAX_IDX_NUMBER * sizeof(uint16_t));
	for (int i = 0; i < MAX_IDX_NUMBER / 6; i++) {
		default_idx_ptr[i * 6] = i * 6;
		default_idx_ptr[i * 6 + 1] = i * 6 + 1;
		default_idx_ptr[i * 6 + 2] = i * 6 + 2;
		default_idx_ptr[i * 6 + 3] = i * 6 + 3;
		default_idx_ptr[i * 6 + 4] = i * 6 + 4;
		default_idx_ptr[i * 6 + 5] = i * 6 + 5;
		default_line_strips_idx_ptr[i * 6] = i * 3;
		default_line_strips_idx_ptr[i * 6 + 1] = i * 3 + 1;
		default_line_strips_idx_ptr[i * 6 + 2] = i * 3 + 1;
		default_line_strips_idx_ptr[i * 6 + 3] = i * 3 + 2;
		default_line_strips_idx_ptr[i * 6 + 4] = i * 3 + 2;
		default_line_strips_idx_ptr[i * 6 + 5] = i * 3 + 3;
		default_quads_idx_ptr[i * 6] = i * 4;
		default_quads_idx_ptr[i * 6 + 1] = i * 4 + 1;
		default_quads_idx_ptr[i * 6 + 2] = i * 4 + 3;
		default_quads_idx_ptr[i * 6 + 3] = i * 4 + 1;
		default_quads_idx_ptr[i * 6 + 4] = i * 4 + 2;
		default_quads_idx_ptr[i * 6 + 5] = i * 4 + 3;
	}

	// Init vertex pool for immediate mode support
	legacy_pool_size = pool_size;

	// Initializing lights configs
	for (int i = 0; i < MAX_LIGHTS_NUM; i++) {
		float data[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		vgl_fast_memcpy(&lights_ambients[i].r, &data[0], sizeof(float) * 4);
		data[2] = 1.0f;
		data[3] = 0.0f;
		vgl_fast_memcpy(&lights_positions[i].r, &data[0], sizeof(float) * 4);
		lights_attenuations[i].r = 1.0f;
		lights_attenuations[i].g = 0.0f;
		lights_attenuations[i].b = 0.0f;
		if (i == 0) {
			const float data2[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			vgl_fast_memcpy(&lights_diffuses[i].r, &data2[0], sizeof(float) * 4);
			vgl_fast_memcpy(&lights_speculars[i].r, &data2[0], sizeof(float) * 4);
		} else {
			vgl_memset(&lights_diffuses[i].r, 0, sizeof(float) * 4);
			vgl_memset(&lights_speculars[i].r, 0, sizeof(float) * 4);
		}
	}

	// Init purge lists
	for (int i = 0; i < FRAME_PURGE_FREQ; i++) {
		frame_purge_list[i][0] = NULL;
		frame_rt_purge_list[i][0] = NULL;
	}

	// Init scissor test state
	reset_scissor_test_region();

	// Allocating default texture object
	texture_slots[0].mip_count = 1;
	texture_slots[0].use_mips = GL_FALSE;
	texture_slots[0].min_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
	texture_slots[0].mag_filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
	texture_slots[0].mip_filter = SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
	texture_slots[0].u_mode = SCE_GXM_TEXTURE_ADDR_REPEAT;
	texture_slots[0].v_mode = SCE_GXM_TEXTURE_ADDR_REPEAT;
	texture_slots[0].lod_bias = GL_MAX_TEXTURE_LOD_BIAS;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	// Defaulting textures into using texture on ID 0 and resetting free textures queue
	for (int i = 1; i < TEXTURES_NUM; i++) {
		texture_slots[i].status = TEX_UNUSED;
		texture_slots[i].gxm_tex = texture_slots[0].gxm_tex;
		texture_slots[i].palette_data = NULL;
	}
	
	// Init modelview and projection matrices as well as first stack entries to identity
	matrix4x4_identity(modelview_matrix);
	matrix4x4_identity(projection_matrix);
	matrix4x4_identity(modelview_matrix_stack[0]);
	matrix4x4_identity(projection_matrix_stack[0]);

	// Init texture matrices as well as first stack entries to identity
	for (int i = 0; i < TEXTURE_COORDS_NUM; i++) {
		matrix4x4_identity(texture_matrix[i]);
		matrix4x4_identity(texture_units[i].texture_matrix_stack[0]);
		texture_units[i].texture_stack_counter = 1;
	}

	vgl_inited = GL_TRUE;
	return res_fallback;
}

GLboolean vglInitWithCustomThreshold(int pool_size, int width, int height, int ram_threshold, int cdram_threshold, int phycont_threshold, int cdlg_threshold, SceGxmMultisampleMode msaa) {
	// Initializing sceGxm
	init_gxm();

	// Getting max allocatable CDRAM and RAM memory
	if (system_app_mode) {
		SceAppMgrBudgetInfo info;
		info.size = sizeof(SceAppMgrBudgetInfo);
		sceAppMgrGetBudgetInfo(&info);
		return vglInitWithCustomSizes(pool_size, width, height, info.free_user_rw > ram_threshold ? info.free_user_rw - ram_threshold : info.free_user_rw, 0, 0, 0, msaa);
	} else {
		SceKernelFreeMemorySizeInfo info;
		info.size = sizeof(SceKernelFreeMemorySizeInfo);
		sceKernelGetFreeMemorySize(&info);
		return vglInitWithCustomSizes(pool_size, width, height,
			info.size_user > ram_threshold ? info.size_user - ram_threshold : 0,
			info.size_cdram > cdram_threshold ? info.size_cdram - cdram_threshold : 0,
			info.size_phycont > phycont_threshold ? info.size_phycont - phycont_threshold : 0,
			SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE > cdlg_threshold ? SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE - cdlg_threshold : 0, msaa);
	}
}

GLboolean vglInitExtended(int pool_size, int width, int height, int ram_threshold, SceGxmMultisampleMode msaa) {
	return vglInitWithCustomThreshold(pool_size, width, height, ram_threshold, 0, 0, SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE, msaa);
}

GLboolean vglInit(int pool_size) {
	return vglInitExtended(pool_size, DISPLAY_WIDTH_DEF, DISPLAY_HEIGHT_DEF, 0x1000000, SCE_GXM_MULTISAMPLE_4X);
}

GLboolean vglSwapResolution(int width, int height) {
#ifndef SKIP_ERROR_HANDLING
	// Check if framebuffer size is valid
	int max_w, max_h;
	sceDisplayGetMaximumFrameBufResolution(&max_w, &max_h);
	if (width > max_w || height > max_h) {
		return GL_FALSE;
	}
#endif

	NEW_DISPLAY_WIDTH = width;
	NEW_DISPLAY_HEIGHT = height;
	
	return GL_TRUE;
}

void vglWaitVblankStart(GLboolean enable) {
	vsync_interval = enable ? 1 : 0;
}

size_t vglMemFree(vglMemType type) {
#ifndef SKIP_ERROR_HANDLING
	if (type >= VGL_MEM_ALL)
		return 0;
#endif
	return vgl_mem_get_free_space(type);
}

size_t vglMemTotal(vglMemType type) {
#ifndef SKIP_ERROR_HANDLING
	if (type >= VGL_MEM_ALL)
		return 0;
#endif
	return vgl_mem_get_total_space(type);
}

void *vglAlloc(uint32_t size, vglMemType type) {
#ifndef SKIP_ERROR_HANDLING
	if (type >= VGL_MEM_ALL)
		return NULL;
#endif
	return vgl_malloc(size, type);
}

void *vglForceAlloc(uint32_t size) {
	return gpu_alloc_mapped_for_cpu(size);
}

void *vglMalloc(uint32_t size) {
	// First we try to use newlib mem
	void *res = vgl_malloc(size, VGL_MEM_EXTERNAL);
	if (res)
		return res;

	// If it fails, we try with standard RAM mem pool
	res = vgl_malloc(size, VGL_MEM_RAM);
	if (res)
		return res;

	// If it fails, we try with physically contiguous RAM
	res = vgl_malloc(size, VGL_MEM_PHYCONT);
	if (res)
		return res;

	// If it fails, we try with common dialog mem
	res = vgl_malloc(size, VGL_MEM_BUDGET);
	if (res)
		return res;

	// If it fails, as last resort, we try VRAM
	res = vgl_malloc(size, VGL_MEM_VRAM);

	if (!res) {
		vgl_log("%s:%d: vglMalloc failed allocating %u bytes (Call generated from 0x%08X). Attempting to recover enough memory to go past it.\n", __FILE__, __LINE__, size, __builtin_return_address(0));
		unsafe_allocator_counter = 0;
		res = gpu_alloc_mapped_aligned_unsafe_for_cpu(MEM_ALIGNMENT, size);
#ifdef LOG_ERRORS
		if (!res) {
			vgl_log("%s:%d vglMalloc failed with a requested size of %u bytes.\n", __FILE__, __LINE__, size);
		} else {
			vgl_log("%s:%d vglMalloc successfully allocated the requested memory after forcing %d garbage collection cycles.\n", __FILE__, __LINE__, unsafe_allocator_counter);
		}
#endif
	}

	return res;
}

size_t vglMallocUsableSize(void *ptr) {
	return vgl_malloc_usable_size(ptr);
}

void *vglMemalign(uint32_t alignment, uint32_t size) {
	// First we try to use newlib mem
	void *res = vgl_memalign(alignment, size, VGL_MEM_EXTERNAL);
	if (res)
		return res;

	// If it fails, we try with standard RAM mem pool
	res = vgl_memalign(alignment, size, VGL_MEM_RAM);
	if (res)
		return res;

	// If it fails, we try with physically contiguous RAM
	res = vgl_memalign(alignment, size, VGL_MEM_PHYCONT);
	if (res)
		return res;

	// If it fails, we try with common dialog mem
	res = vgl_memalign(alignment, size, VGL_MEM_BUDGET);
	if (res)
		return res;

	// If it fails, as last resort, we try VRAM
	res = vgl_memalign(alignment, size, VGL_MEM_VRAM);

	if (!res) {
		vgl_log("%s:%d: vglMemalign failed allocating 0x%X bytes with 0x%X alignment.\n", __FILE__, __LINE__, size, alignment);
		unsafe_allocator_counter = 0;
		res = gpu_alloc_mapped_aligned_unsafe_for_cpu(alignment, size);
#ifdef LOG_ERRORS
		if (!res) {
			vgl_log("%s:%d vglMemalign failed with a requested size of %u bytes.\n", __FILE__, __LINE__, size);
		} else {
			vgl_log("%s:%d vglMemalign successfully allocated the requested memory after forcing %d garbage collection cycles.\n", __FILE__, __LINE__, unsafe_allocator_counter);
		}
#endif
	}

	return res;
}

void *vglCalloc(uint32_t nmember, uint32_t size) {
	// First we try to use newlib mem
	void *res = vgl_calloc(nmember, size, VGL_MEM_EXTERNAL);
	if (res)
		return res;

	// If it fails, we try with standard RAM mem pool
	res = vgl_calloc(nmember, size, VGL_MEM_RAM);
	if (res)
		return res;

	// If it fails, we try with physically contiguous RAM
	res = vgl_calloc(nmember, size, VGL_MEM_PHYCONT);
	if (res)
		return res;

	// If it fails, we try with common dialog mem
	res = vgl_calloc(nmember, size, VGL_MEM_BUDGET);
	if (res)
		return res;

	// If it fails, as last resort, we try VRAM
	res = vgl_calloc(nmember, size, VGL_MEM_VRAM);

	if (!res) {
		vgl_log("%s:%d: vglCalloc failed allocating 0x%X blocks of 0x%X bytes.\n", __FILE__, __LINE__, nmember, size);
		unsafe_allocator_counter = 0;
		res = gpu_alloc_mapped_aligned_unsafe_for_cpu(MEM_ALIGNMENT, size * nmember);
		if (!res) {
			vgl_log("%s:%d vglCalloc failed with a requested size of %u bytes.\n", __FILE__, __LINE__, size);
		} else {
			sceClibMemset(res, 0, size * nmember);
			vgl_log("%s:%d vglCalloc successfully allocated the requested memory after forcing %d garbage collection cycles.\n", __FILE__, __LINE__, unsafe_allocator_counter);
		}
	}

	return res;
}

void *vglRealloc(void *ptr, uint32_t size) {
	if (!ptr)
		return vglMalloc(size);

	void *res = vgl_realloc(ptr, size);
	if (res)
		return res;

	res = vglMalloc(size);
	if (res) {
		size_t copy_size = vgl_malloc_usable_size(ptr);
		if (copy_size > size)
			copy_size = size;
		vgl_fast_memcpy(res, ptr, copy_size);
		vglFree(ptr);
	}
#ifndef SKIP_ERROR_HANDLING
	if (!res) {
		vgl_log("%s:%d: vglRealloc failed reallocating 0x%X to 0x%X bytes.\n", __FILE__, __LINE__, ptr, size);
	}
#endif
	return res;
}

void vglFree(void *addr) {
	if (!addr)
		return;

	vgl_free(addr);
}

void vglLazyFree(void *addr) {
	THREAD_SAFE()

	mark_as_dirty(addr);
}

void vglUseExtraMem(GLboolean use) {
	use_extra_mem = use;
}

void vglSetCircularPoolSize(uint32_t size) {
#ifndef DISABLE_CIRCULAR_POOL
	circular_data_pool_size = size;
#endif
}

void vglUseCachedMem(GLboolean use) {
	has_cached_mem = use;
}

void vglSetTextureCacheFrequency(GLuint freq) {
#ifdef HAVE_TEX_CACHE
	vgl_tex_cache_freq = freq;
#endif
}

// ---- GLB: cached stream arena -------------------------------------------------------------------------------------------------------------------------
#if !defined(DISABLE_CIRCULAR_POOL) && !defined(CIRCULAR_POOL_SPEEDHACK)
GLboolean vgl_stream_cached = GL_FALSE, vgl_stream_unflushed = GL_FALSE;
static uint32_t stream_slice_req = 0, stream_slice = 0;
static int stream_mem = 0; // vglStreamMem: 0 cached RAM + write-back, 1 CDRAM (write-combined, nothing to write back), 2 uncached RAM (diagnostic)
static vglCacheFlushFn stream_flush_fn = NULL;
static SceUID stream_uid = -1;
static uint8_t *stream_base[DISPLAY_MAX_BUFFER_COUNT], *stream_ptr[DISPLAY_MAX_BUFFER_COUNT], *stream_flushed[DISPLAY_MAX_BUFFER_COUNT];
static uint32_t stream_frame_peak = 0, stream_peak = 0, stream_overruns = 0, stream_overrun_bytes = 0, stream_flushes = 0, stream_flushed_bytes = 0;

// vglInit*: after the circular pool. One memblock, `count` slices (one per display buffer, like the circular pool).
void vgl_stream_init(int count) {
	if (!stream_slice_req || !count)
		return;
	stream_slice = (stream_slice_req + 0xFFFu) & ~0xFFFu;
	if (stream_mem == 1)
		stream_slice = (stream_slice + 0x3FFFFu) & ~0x3FFFFu; // CDRAM blocks are multiples of 256 KiB
	uint32_t total = stream_slice * count;
	SceKernelMemBlockType type = stream_mem == 1 ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : (stream_mem == 2 ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
	stream_uid = sceKernelAllocMemBlock("vgl_stream", type, total, NULL);
	void *base = NULL;
	if (stream_uid < 0 || sceKernelGetMemBlockBase(stream_uid, &base) < 0 || !base) {
		vgl_log("%s:%d Stream arena of %u bytes (memory kind %d) not available (0x%08X): GL_STREAM_DRAW buffers stay in the circular pool.\n", __FILE__, __LINE__, total, stream_mem, stream_uid);
		stream_uid = -1;
		return;
	}
	sceGxmMapMemory(base, total, SCE_GXM_MEMORY_ATTRIB_READ);
	for (int i = 0; i < count; i++) {
		stream_base[i] = (uint8_t *)base + stream_slice * i;
		stream_ptr[i] = stream_flushed[i] = stream_base[i];
	}
	vgl_stream_cached = GL_TRUE;
	vgl_log("Stream arena: %u bytes per slice, %d slices at 0x%08X, memory kind %d (%s).\n", stream_slice, count, (unsigned)base, stream_mem, stream_mem == 1 ? "CDRAM, write-combined" : (stream_mem == 2 ? "uncached RAM" : "cached RAM, written back before draws"));
}

// 64-byte steps: two reservations never share a cache line, so writing one back never touches a neighbour that is still being filled.
uint8_t *vgl_reserve_stream_pool(uint32_t size) {
	size = (size + 63u) & ~63u;
	uint8_t *p = stream_ptr[vgl_circular_idx];
	if (p + size <= stream_base[vgl_circular_idx] + stream_slice) {
		stream_ptr[vgl_circular_idx] = p + size;
		if (stream_mem == 0)
			vgl_stream_unflushed = GL_TRUE;
		return p;
	}
	stream_overruns++;
	stream_overrun_bytes += size;
	return vgl_reserve_data_pool(size);
}

// Writes back what was reserved since the last flush. Called before a draw (VGL_STREAM_FLUSH_IF_NEEDED) and at the frame end.
void vgl_stream_flush(void) {
	vgl_stream_unflushed = GL_FALSE;
	uint8_t *from = stream_flushed[vgl_circular_idx], *to = stream_ptr[vgl_circular_idx];
	if (to <= from)
		return;
	stream_flushed[vgl_circular_idx] = to;
	stream_flushes++;
	stream_flushed_bytes += to - from;
	if (stream_flush_fn)
		stream_flush_fn(from, to - from);
	else
		sceKernelSyncVMDomain(stream_uid, from, to - from);
}

// A CPU write into a buffer of the arena after its range was written back (glBufferSubData in place): write that part back again before the next draw.
void vgl_stream_touch(const void *addr) {
	if (!vgl_stream_cached || stream_mem != 0)
		return;
	uint8_t *a = (uint8_t *)((uintptr_t)addr & ~63u);
	if (a >= stream_base[vgl_circular_idx] && a < stream_ptr[vgl_circular_idx]) {
		if (a < stream_flushed[vgl_circular_idx])
			stream_flushed[vgl_circular_idx] = a;
		vgl_stream_unflushed = GL_TRUE;
	}
}

// Frame end (gxm.c, before the pool index moves on): peaks, then the slice the next frame will use starts empty.
void vgl_glb_frame_end(void) {
	if (vgl_stream_cached) {
		if (vgl_stream_unflushed)
			vgl_stream_flush();
		stream_frame_peak = stream_ptr[vgl_circular_idx] - stream_base[vgl_circular_idx];
		if (stream_frame_peak > stream_peak)
			stream_peak = stream_frame_peak;
	}
	vgl_glb_pool_frame_peak = (uint32_t)(circular_data_pool_ptr[vgl_circular_idx] - circular_data_pool[vgl_circular_idx]);
	if (vgl_glb_pool_frame_peak > vgl_glb_pool_peak)
		vgl_glb_pool_peak = vgl_glb_pool_frame_peak;
	if (vgl_glb_ring_frame_bytes > vgl_glb_ring_peak)
		vgl_glb_ring_peak = vgl_glb_ring_frame_bytes;
	vgl_glb_ring_frame_peak = vgl_glb_ring_frame_bytes;
	vgl_glb_ring_frame_bytes = 0;
}

// The next slice (vgl_circular_idx has just moved on).
void vgl_stream_next_slice(void) {
	if (vgl_stream_cached)
		stream_ptr[vgl_circular_idx] = stream_flushed[vgl_circular_idx] = stream_base[vgl_circular_idx];
}
#endif

void vglSetupStreamArena(uint32_t slice_bytes, int mem, vglCacheFlushFn flush) {
#if !defined(DISABLE_CIRCULAR_POOL) && !defined(CIRCULAR_POOL_SPEEDHACK) && defined(HAVE_SCRATCH_MEMORY)
	stream_slice_req = slice_bytes;
	stream_mem = mem;
	stream_flush_fn = flush;
	if (slice_bytes)
		vgl_stream_wants_scratch = GL_TRUE; // the arena serves scratch stream buffers only
#endif
}

void vglSetupCachedStream(uint32_t slice_bytes, vglCacheFlushFn flush) {
	vglSetupStreamArena(slice_bytes, VGL_STREAM_CACHED, flush);
}

extern GLboolean vgl_glb_ring_cdram_req, vgl_glb_ring_cdram;
void vglSetUniformRingMem(int cdram) {
	vgl_glb_ring_cdram_req = cdram ? GL_TRUE : GL_FALSE;
}

void vglSetSkipRedundantSets(GLboolean enable) {
	vgl_gxc_enabled = enable;
}

void vglGlbGetStats(vglGlbStats *s) {
	vgl_memset(s, 0, sizeof(*s));
	s->version = 1;
#if !defined(DISABLE_CIRCULAR_POOL) && !defined(CIRCULAR_POOL_SPEEDHACK)
	s->pool_slice_bytes = circular_data_pool_size / (gxm_display_buffer_count ? gxm_display_buffer_count : 1);
	s->pool_frame_peak = vgl_glb_pool_frame_peak;
	s->pool_peak = vgl_glb_pool_peak;
	s->pool_overruns = vgl_glb_pool_overruns;
	s->pool_overrun_bytes = vgl_glb_pool_overrun_bytes;
	s->pool_odd = vgl_glb_pool_odd;
	s->stream_slice_bytes = stream_slice;
	s->stream_mem = vgl_stream_cached ? stream_mem + 1 : 0;
	s->stream_frame_peak = stream_frame_peak;
	s->stream_peak = stream_peak;
	s->stream_overruns = stream_overruns;
	s->stream_overrun_bytes = stream_overrun_bytes;
	s->stream_flushes = stream_flushes;
	s->stream_flushed_bytes = stream_flushed_bytes;
#endif
	s->ring_bytes = vgl_glb_ring_size;
	s->ring_cdram = vgl_glb_ring_cdram;
	s->ring_frame_peak = vgl_glb_ring_frame_peak;
	s->ring_peak = vgl_glb_ring_peak;
	s->ring_wraps = vgl_glb_ring_wraps;
	vgl_glb_count_tables(&s->programs_live, &s->shaders_live, &s->programs_max, &s->shaders_max);
	if (gxm_shader_patcher) {
		s->patcher_host = sceGxmShaderPatcherGetHostMemAllocated(gxm_shader_patcher);
		s->patcher_buffer = sceGxmShaderPatcherGetBufferMemAllocated(gxm_shader_patcher);
		s->patcher_vusse = sceGxmShaderPatcherGetVertexUsseMemAllocated(gxm_shader_patcher);
		s->patcher_fusse = sceGxmShaderPatcherGetFragmentUsseMemAllocated(gxm_shader_patcher);
	}
	for (int i = 0; i < 4; i++) {
		s->skip_calls[i] = vgl_gxc_calls[i];
		s->skip_skipped[i] = vgl_gxc_skipped[i];
	}
}

GLboolean vgl_gxc_enabled = GL_FALSE;
vgl_gxc_t vgl_gxc;
uint32_t vgl_gxc_calls[4], vgl_gxc_skipped[4];

void vglSetupScratchMemory(GLboolean scratch_for_dynamic, GLboolean scratch_for_stream) {
#if defined(HAVE_SCRATCH_MEMORY) && !defined(DISABLE_CIRCULAR_POOL)
	vgl_dynamic_wants_scratch = scratch_for_dynamic;
	vgl_stream_wants_scratch = scratch_for_stream;
#endif
}

void *vglAllocFromScratch(size_t size) {
	return gpu_alloc_mapped_temp(size);
}

uint32_t vglGetFrameNumber() {
	return vgl_framecount;
}

void vglPhycontMemLazyInit(size_t size) {
#ifndef PHYCONT_ON_DEMAND
	vgl_mem_provide_phycont(size);
#endif
}

void vglSetShaderCachePath(const char *path) {
#ifdef HAVE_SHADER_CACHE
	strcpy(shader_cache_root, path);
#endif
}

#ifdef HAVE_SHADER_CACHE
extern char vgl_shader_seed_path[256];
#endif
// W5 SEED: the read-only second cache folder (see custom_shaders.c); "" or NULL = none
void vglSetShaderSeedPath(const char *path) {
#ifdef HAVE_SHADER_CACHE
	if (path) {
		strncpy(vgl_shader_seed_path, path, sizeof(vgl_shader_seed_path) - 1);
		vgl_shader_seed_path[sizeof(vgl_shader_seed_path) - 1] = 0;
	} else
		vgl_shader_seed_path[0] = 0;
#endif
}

void vglSetShaderAssociationPath(const char *path) {
	shark_set_shader_association_path(path);
}
