/*
 * This file is part of vitaGL
 * Copyright 2017-2023 Rinnegatamante
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
 * custom_shaders.c:
 * Implementation for custom shaders feature
 */

#define _GNU_SOURCE
#include <string.h>
#include "shared.h"
#include "utils/glsl_utils.h"
#include "utils/geom_fix.h" // 0059
#include "utils/shacccg_paramquery.h"
#if defined(HAVE_SHADER_CACHE) || defined(HAVE_TEX_CACHE)
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#define XXH_NAMESPACE VITAGL_
#define XXH_memcpy vgl_fast_memcpy
#define XXH_memset sceClibMemset
#include "utils/xxhash_utils.h"
#ifdef HAVE_SHADER_CACHE
char vgl_shader_cache_path[256];
#ifdef HAVE_SHADER_CACHE
// W5 SEED: a second, READ-ONLY cache folder (the package's seed: app0:shadercache/<key>/v<magic>), tried when the writable cache has no file for a shader.
// With it a package can carry every compiled shader without copying them to the memory card at the first start (about 75 ms per file or folder there).
char vgl_shader_seed_path[256];
// W5 BOOT: compiled shaders the application holds in memory (one pack file read at start-up instead of one file per shader:
// a memory card answers a file open in about 7 ms). Asked before any cache file; NULL = none. The returned bytes are a cache
// file's contents (the format the cache writes), 4-byte aligned, and must stay valid: unserialize_shader copies what it keeps.
static const void *(*vgl_shader_pack_lookup)(int fragment, uint64_t hash, uint32_t *size) = NULL;
void vglSetShaderPackLookup(const void *(*fn)(int fragment, uint64_t hash, uint32_t *size)) {
	vgl_shader_pack_lookup = fn;
}
static SceUID vgl_open_seed(const char *kind, uint8_t subdir_hash, uint64_t hash) {
	if (!vgl_shader_seed_path[0])
		return -1;
	char fname[256];
	// The names this library's own cache writes (16 hex digits, upstream since cache version 3) first; a seed taken from an older
	// library names its files without the leading zeros: tried second, and only when that spelling is a different one.
	sprintf(fname, "%s/%s/%02X/%016llX.gxp", vgl_shader_seed_path, kind, subdir_hash, hash);
	SceUID f = sceIoOpen(fname, SCE_O_RDONLY, 0777);
	if (f < 0 && (hash >> 60) == 0) {
		sprintf(fname, "%s/%s/%02X/%llX.gxp", vgl_shader_seed_path, kind, subdir_hash, hash);
		f = sceIoOpen(fname, SCE_O_RDONLY, 0777);
	}
	return f;
}
#endif
#endif
#ifdef HAVE_TEX_CACHE
char vgl_file_cache_path[256];
#endif
#endif

#define MAX_CUSTOM_SHADERS 2048 // Maximum number of linkable custom shaders
#define MAX_CUSTOM_PROGRAMS 1024 // Maximum number of linkable custom programs

#define set_default_attrib_binding() \
	uint32_t cnt = sceGxmProgramGetParameterCount(p->vshader->prog); \
	uint32_t *ptr = vglProgramGetParameterBase(p->vshader->prog); \
	for (int i = 0; i < cnt; i++) { \
		SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr; \
		SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param); \
		if (cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE && p->attr_highest_idx < VERTEX_ATTRIBS_NUM) { \
			p->attr[p->attr_highest_idx++].regIndex = sceGxmProgramParameterGetResourceIndex(param); \
		} \
		ptr += 4; \
	}

#define disable_draw_attrib(i) \
	orig_stride[i] = streams[i].stride; \
	orig_fmt[i] = attributes[i].format; \
	orig_size[i] = attributes[i].componentCount; \
	streams[i].stride = 0; \
	attributes[i].offset = 0; \
	attributes[i].componentCount = cur_vao->vertex_attrib_size[attr_idx]; \
	attributes[i].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;

#ifdef SAFER_DRAW_SPEEDHACK
#define handle_unpacked_attrib(first, count) \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		if (cur_vao->vertex_attrib_vbo[attr_idx]) { \
			vbo *gpu_buf = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx]; \
			ptrs[i] = (uint8_t *)gpu_buf->ptr + cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride; \
			gpu_buf->last_frame = vgl_framecount; \
			attributes[i].offset = 0; \
		} else { \
			if (count * streams[i].stride > SAFE_DRAW_SIZE_THRESHOLD) { \
				ptrs[i] = (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride; \
			} else { \
				ptrs[i] = gpu_alloc_mapped_temp(count * streams[i].stride); \
				vgl_fast_memcpy(ptrs[i], (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride, count * streams[i].stride); \
				attributes[i].offset = 0; \
			} \
		} \
	} else { \
		disable_draw_attrib(i) \
	}
#else
#define handle_unpacked_attrib(first, count) \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		if (cur_vao->vertex_attrib_vbo[attr_idx]) { \
			vbo *gpu_buf = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx]; \
			ptrs[i] = (uint8_t *)gpu_buf->ptr + cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride; \
			gpu_buf->last_frame = vgl_framecount; \
			attributes[i].offset = 0; \
		} else { \
			ptrs[i] = gpu_alloc_mapped_temp(count * streams[i].stride); \
			vgl_fast_memcpy(ptrs[i], (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride, count * streams[i].stride); \
			attributes[i].offset = 0; \
		} \
	} else { \
		disable_draw_attrib(i) \
	}
#endif

#define handle_speedhack_attrib() \
	for (int i = 0; i < p->attr_num; i++) { \
		uint8_t attr_idx = p->attr_map[i]; \
		attributes[i].regIndex = p->attr[attr_idx].regIndex; \
		if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
			if (cur_vao->vertex_attrib_vbo[attr_idx]) { \
				vbo *gpu_buf = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx]; \
				ptrs[i] = (uint8_t *)gpu_buf->ptr + cur_vao->vertex_attrib_offsets[attr_idx]; \
				gpu_buf->last_frame = vgl_framecount; \
				attributes[i].offset = 0; \
			} else { \
				ptrs[i] = (void *)cur_vao->vertex_attrib_offsets[attr_idx]; \
				attributes[i].offset = 0; \
			} \
		} else { \
			disable_draw_attrib(i) \
		} \
	}

#define handle_packed_vbo_attrib() \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		attributes[i].offset = cur_vao->vertex_attrib_offsets[attr_idx]; \
	} else { \
		disable_draw_attrib(i) \
	}	

#define handle_packed_attrib() \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		attributes[i].offset = cur_vao->vertex_attrib_offsets[attr_idx] - cur_vao->vertex_attrib_offsets[p->attr_map[0]]; \
	} else { \
		disable_draw_attrib(i) \
	}

#ifndef HAVE_FFP_SHADER_SUPPORT
// be26 (uni_reuse 2): a copy of a value set that equals one of the program's last VGL_UNI_HIST copies of this generation (compared byte for byte with a copy kept in main
// memory, never with the ring) is named again as well; vgl_uni_stat counts copies, last-copy reuses and history reuses (count, bytes).
// 1.3 (vglSetDrawCaches, uni_reuse): a program whose values have not changed since it last copied them in this scene (the switch to it is what set the dirty flag) names
// that copy again. vgl_unif_gen moves at every scene end and every wrap of the ring, so a copy is never named outside the scene it was made for.
#define upload_uniforms() \
	if (p->vert_uniforms && dirty_shader_vert_unifs) { \
		void *vgl_uh_hit = NULL; \
		if (vgl_uni_reuse && p->unif_gen[0] == vgl_unif_gen && !p->unif_changed[0]) { \
			vgl_def_vert_buf = p->unif_last[0]; \
			sceGxmSetVertexDefaultUniformBuffer(gxm_context, vgl_def_vert_buf); \
			vgl_uni_stat[2]++; \
			vgl_uni_stat[3] += p->vshader->unif_buf_size; \
			if (vgl_uni_reuse > 2 && !uni_hist_eq(p->unif_last[0], p->unif_vbuffer, p->vshader->unif_buf_size)) vgl_uni_stat[6]++; \
		} else if (vgl_uni_reuse > 1 && (vgl_uh_hit = uni_hist_find(p, 0, p->unif_vbuffer, p->vshader->unif_buf_size))) { \
			vgl_def_vert_buf = vgl_uh_hit; \
			sceGxmSetVertexDefaultUniformBuffer(gxm_context, vgl_def_vert_buf); \
			p->unif_last[0] = vgl_uh_hit; \
			p->unif_gen[0] = vgl_unif_gen; \
			p->unif_changed[0] = 0; \
			vgl_uni_stat[4]++; \
			vgl_uni_stat[5] += p->vshader->unif_buf_size; \
			if (vgl_uni_reuse > 2 && !uni_hist_eq(vgl_uh_hit, p->unif_vbuffer, p->vshader->unif_buf_size)) vgl_uni_stat[6]++; \
		} else { \
			void *buffer = vglReserveVertexUniformBuffer(p->vshader->unif_buf_size); \
			vgl_wr_copy(buffer, p->unif_vbuffer, p->vshader->unif_buf_size); \
			if (vgl_uni_reuse > 1) uni_hist_add(p, 0, buffer, p->unif_vbuffer, p->vshader->unif_buf_size); \
			p->unif_last[0] = buffer; \
			p->unif_gen[0] = vgl_unif_gen; \
			p->unif_changed[0] = 0; \
			vgl_uni_stat[0]++; \
			vgl_uni_stat[1] += p->vshader->unif_buf_size; \
		} \
		dirty_shader_vert_unifs = GL_FALSE; \
	} \
	if (p->frag_uniforms && dirty_shader_frag_unifs) { \
		void *vgl_uh_hit = NULL; \
		if (vgl_uni_reuse && p->unif_gen[1] == vgl_unif_gen && !p->unif_changed[1]) { \
			vgl_def_frag_buf = p->unif_last[1]; \
			sceGxmSetFragmentDefaultUniformBuffer(gxm_context, vgl_def_frag_buf); \
			vgl_uni_stat[2]++; \
			vgl_uni_stat[3] += p->fshader->unif_buf_size; \
			if (vgl_uni_reuse > 2 && !uni_hist_eq(p->unif_last[1], p->unif_fbuffer, p->fshader->unif_buf_size)) vgl_uni_stat[6]++; \
		} else if (vgl_uni_reuse > 1 && (vgl_uh_hit = uni_hist_find(p, 1, p->unif_fbuffer, p->fshader->unif_buf_size))) { \
			vgl_def_frag_buf = vgl_uh_hit; \
			sceGxmSetFragmentDefaultUniformBuffer(gxm_context, vgl_def_frag_buf); \
			p->unif_last[1] = vgl_uh_hit; \
			p->unif_gen[1] = vgl_unif_gen; \
			p->unif_changed[1] = 0; \
			vgl_uni_stat[4]++; \
			vgl_uni_stat[5] += p->fshader->unif_buf_size; \
			if (vgl_uni_reuse > 2 && !uni_hist_eq(vgl_uh_hit, p->unif_fbuffer, p->fshader->unif_buf_size)) vgl_uni_stat[6]++; \
		} else { \
			void *buffer = vglReserveFragmentUniformBuffer(p->fshader->unif_buf_size); \
			vgl_wr_copy(buffer, p->unif_fbuffer, p->fshader->unif_buf_size); \
			if (vgl_uni_reuse > 1) uni_hist_add(p, 1, buffer, p->unif_fbuffer, p->fshader->unif_buf_size); \
			p->unif_last[1] = buffer; \
			p->unif_gen[1] = vgl_unif_gen; \
			p->unif_changed[1] = 0; \
			vgl_uni_stat[0]++; \
			vgl_uni_stat[1] += p->fshader->unif_buf_size; \
		} \
		dirty_shader_frag_unifs = GL_FALSE; \
	} \
	if (p->vert_ubos) { \
		ubo *u = p->vert_ubos; \
		while (u) { \
			ubo *b = u->alias ? u->alias : u; \
			sceGxmSetVertexUniformBuffer(gxm_context, b->idx, (uint8_t *)ubo_buf[b->bind]->ptr + ubo_offset[b->bind]); \
			ubo_buf[b->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	} \
	if (p->frag_ubos) { \
		ubo *u = p->frag_ubos; \
		while (u) { \
			sceGxmSetFragmentUniformBuffer(gxm_context, u->idx, (uint8_t *)ubo_buf[u->bind]->ptr + ubo_offset[u->bind]); \
			ubo_buf[u->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	}
#else
#define upload_uniforms() \
	if (p->vert_uniforms && (dirty_vert_unifs || dirty_shader_vert_unifs)) { \
		void *buffer = vglReserveVertexUniformBuffer(p->vshader->unif_buf_size); \
		if (dirty_vert_unifs) { \
			dirty_vert_unifs = GL_FALSE; \
			if (p->ffp_binds[FFP_MVP_MATRIX]) { \
				if (mvp_modified) { \
					matrix4x4_multiply(vgl_mvp_matrix, projection_matrix, modelview_matrix); \
					recalculate_normal_matrix(); \
					mvp_modified = GL_FALSE; \
				} \
				vglSetUniformData(p->ffp_binds[FFP_MVP_MATRIX]->vptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 4, 4, (const float *)vgl_mvp_matrix, SCE_GXM_PARAMETER_TYPE_F32); \
			} \
			if (p->ffp_binds[FFP_MV_MATRIX]) { \
				vglSetUniformData(p->ffp_binds[FFP_MV_MATRIX]->vptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 4, 4, (const float *)modelview_matrix, SCE_GXM_PARAMETER_TYPE_F32); \
			} \
			if (p->ffp_binds[FFP_NORMAL_MATRIX]) { \
				if (mvp_modified) { \
					matrix4x4_multiply(vgl_mvp_matrix, projection_matrix, modelview_matrix); \
					recalculate_normal_matrix(); \
					mvp_modified = GL_FALSE; \
				} \
				vglSetUniformData(p->ffp_binds[FFP_NORMAL_MATRIX]->vptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 3, 3, (const float *)normal_matrix, SCE_GXM_PARAMETER_TYPE_F32); \
			} \
		} \
		vgl_wr_copy(buffer, p->unif_vbuffer, p->vshader->unif_buf_size); \
		dirty_shader_vert_unifs = GL_FALSE; \
	} \
	if (p->frag_uniforms && dirty_shader_frag_unifs) { \
		void *buffer = vglReserveFragmentUniformBuffer(p->fshader->unif_buf_size); \
		vgl_wr_copy(buffer, p->unif_fbuffer, p->fshader->unif_buf_size); \
		dirty_shader_frag_unifs = GL_FALSE; \
	} \
	if (p->vert_ubos) { \
		ubo *u = p->vert_ubos; \
		while (u) { \
			ubo *b = u->alias ? u->alias : u; \
			sceGxmSetVertexUniformBuffer(gxm_context, b->idx, (uint8_t *)ubo_buf[b->bind]->ptr + ubo_offset[b->bind]); \
			ubo_buf[b->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	} \
	if (p->frag_ubos) { \
		ubo *u = p->frag_ubos; \
		while (u) { \
			sceGxmSetFragmentUniformBuffer(gxm_context, u->idx, (uint8_t *)ubo_buf[u->bind]->ptr + ubo_offset[u->bind]); \
			ubo_buf[u->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	}
#endif
	
// 1.2 (vglSetRobustScenes): the program for the current blend function and target format is made first and published, together with the key that says
// it is the current one, only when sceGxm made it. A program that could not be made (the shader patcher's memory is full) used to leave the key saying
// "up to date" over the previous program (the draw then used the wrong blend function for as long as the key stayed) or, for a program's first draw,
// over nothing (sceGxmSetFragmentProgram(NULL)). Now the draw is skipped (`failed`) and the next draw tries again.
#define setup_frag_program_tx(failed) \
	if ((p->blend_info.raw != blend_info.raw) || (is_fbo_float != p->is_fbo_float) || (cur_msaa != p->msaa)) { \
		if (vgl_robust_scenes) { \
			SceGxmFragmentProgram *rebuilt = vgl_frag_memo ? frag_memo_find(p) : NULL; \
			if (!rebuilt) { \
				if (rebuild_frag_shader(p->fshader->id, &rebuilt, (SceGxmProgram *)p->vshader->prog, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4) || !rebuilt) { \
					failed; \
				} \
				if (vgl_frag_memo) \
					frag_memo_add(p, rebuilt); \
			} \
			p->fprog = rebuilt; \
			p->is_fbo_float = is_fbo_float; \
			p->msaa = cur_msaa; \
			p->blend_info.raw = blend_info.raw; \
		} else { \
			p->is_fbo_float = is_fbo_float; \
			p->msaa = cur_msaa; \
			p->blend_info.raw = blend_info.raw; \
			rebuild_frag_shader(p->fshader->id, &p->fprog, (SceGxmProgram *)p->vshader->prog, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4); \
		} \
	} \
	sceGxmSetFragmentProgram(gxm_context, p->fprog);
#define setup_frag_program() setup_frag_program_tx(return GL_FALSE) // (in the functions that return GLboolean: the draw is skipped by their callers)
#define setup_frag_program_void() setup_frag_program_tx(return) // (the others: the program is not published, nothing is skipped)
	
#define align_attributes(attributes, streams) \
	if (p->has_unaligned_attrs) { \
		attributes = temp_attributes; \
		streams = temp_streams; \
		for (int i = 0; i < p->attr_num; i++) { \
			uint8_t attr_idx = p->attr_map[i]; \
			temp_attributes[i] = cur_vao->vertex_attrib_config[attr_idx]; \
			temp_streams[i] = cur_vao->vertex_stream_config[attr_idx]; \
			attributes[i].streamIndex = i; \
		} \
	} else { \
		attributes = cur_vao->vertex_attrib_config; \
		streams = cur_vao->vertex_stream_config; \
	}
	
#ifdef HAVE_FFP_SHADER_SUPPORT
const char *ffp_bind_names[FFP_BINDS_NUM] = {
	"gl_ModelViewProjectionMatrix",
	"gl_ModelViewMatrix",
	"gl_NormalMatrix",
};
#endif

// Internal stuffs
GLboolean is_shark_online = GL_FALSE; // Current vitaShaRK status
static SceGxmVertexAttribute temp_attributes[VERTEX_ATTRIBS_NUM];
static SceGxmVertexStream temp_streams[VERTEX_ATTRIBS_NUM];
static unsigned short orig_stride[VERTEX_ATTRIBS_NUM];
static SceGxmAttributeFormat orig_fmt[VERTEX_ATTRIBS_NUM];
static unsigned char orig_size[VERTEX_ATTRIBS_NUM];
static vbo *ubo_buf[UBOS_NUM];
static uint32_t ubo_offset[UBOS_NUM];
static uint8_t tex2d_override = 0;
GLboolean dirty_shader_frag_unifs = GL_TRUE;
GLboolean dirty_shader_vert_unifs = GL_TRUE;

typedef struct {
	GLuint idx;
	char name[64];
} attr_mapping;

#ifdef STRICT_UNIFORMS_COMPLIANCE
typedef union {
	struct {
		uint32_t offset : 12;
		uint32_t program_idx : 10;
		uint32_t uniform_idx : 8;
		uint32_t is_vertex : 1;
		uint32_t zero : 1;
	};
	int raw;
} uniform_location;
#endif

// Internal runtime shader compiler settings
int32_t compiler_fastmath = GL_TRUE;
int32_t compiler_fastprecision = GL_FALSE;
int32_t compiler_fastint = GL_TRUE;
shark_opt compiler_opts = SHARK_OPT_FAST;

GLuint cur_program = 0; // Current in use custom program (0 = No custom program)

#define VGL_DIRECT_VPROGS 8
#define VGL_FRAG_MEMO 8
GLboolean vgl_frag_memo = GL_FALSE, vgl_uni_reuse = GL_FALSE; // vglSetDrawCaches (uni_reuse: 0 off, 1 the last copy, 2 the last VGL_UNI_HIST copies, 3 as 2 and every reuse reads the ring copy back and counts a difference: a test mode)
#define VGL_UNI_HIST 4
#define VGL_UNI_HIST_BUDGET (1536u * 1024u) // bytes of main memory all programs' history copies may take together; a program past it gets none (it copies as before)
uint32_t vgl_uni_stat[7]; // copies made, their bytes; last-copy reuses, their bytes; history reuses, their bytes; (uni_reuse 3, verify) reuses whose ring copy differs from the buffer now (since start; vglGlbGetStats)
static uint32_t vgl_uni_hist_bytes;
uint32_t vgl_uni_hist_cap = 0; // gem2b (vglSetUniHistCap): 0 every buffer size keeps a history (be26); else only the buffers of at most this many bytes (the big skin / palette buffers rarely repeat: their misses cost a scan and a copy for nothing)
static const vglDirectLayout *direct_layout = NULL; // vglDrawElementsDirect: the layout of the draw in progress
static vbo *direct_vbo = NULL;
static uint32_t direct_base = 0;
static GLboolean direct_failed = GL_FALSE;

// What glUseProgram did before upstream 9bf3e3f for every call: the uniform buffers of the program in use are reserved and copied again by the next draw.
static inline __attribute__((always_inline)) void vgl_dirty_all_uniforms(void) {
	dirty_shader_frag_unifs = GL_TRUE;
	dirty_shader_vert_unifs = GL_TRUE;
	dirty_vert_unifs = 0xFFFF;
	dirty_frag_unifs = 0xFFFFFFFF;
}

typedef enum {
	UNIFORM_DATA,
	UNIFORM_SAMPLER,
	UNIFORM_CUBE_SAMPLER
} uniform_type;

// Uniform struct
typedef struct {
	const SceGxmProgramParameter *ptr;
#ifdef HAVE_GLSL_TEXTURE_SIZE
	glsl_samplers_info *sampler;
#endif
	void *fptr;
	void *vptr;
	uint8_t *changed; // 1.3: the program's unif_changed[2] (vertex, fragment): set by every write of a value
	uniform_type type;
	uint8_t sampler_index;
} uniform;

// Program status enum
typedef enum {
	PROG_INVALID,
	PROG_UNLINKED,
	PROG_LINKED
} prog_status;

// Uniform buffer object struct
typedef struct {
	const SceGxmProgramParameter *ptr;
	uint32_t bind;
	uint32_t idx;
	void *alias;
	void *chain;
} ubo;

#ifdef ENABLE_LEGACY_PIPELINE
typedef enum {
	VGL_ATTRIB_REGULAR,
	VGL_ATTRIB_PACKED,
	VGL_ATTRIB_UNPACKED
} attrib_mode;
#endif

// Program struct holding vertex/fragment shader info
typedef struct {
	shader *vshader;
	shader *fshader;
	uint8_t status;
	uint8_t max_frag_texunit_idx;
	uint8_t max_vert_texunit_idx;
	uniform *vert_texunits[TEXTURE_IMAGE_UNITS_NUM];
	uniform *frag_texunits[TEXTURE_IMAGE_UNITS_NUM];
	SceGxmVertexAttribute attr[VERTEX_ATTRIBS_NUM];
	SceGxmVertexStream stream[VERTEX_ATTRIBS_NUM];
	uint8_t attr_map[VERTEX_ATTRIBS_NUM];
	SceGxmVertexProgram *vprog;
	SceGxmFragmentProgram *fprog;
	blend_config blend_info;
	GLuint attr_num;
	GLuint attr_idx;
#ifdef ENABLE_LEGACY_PIPELINE
	attrib_mode attr_mode;
#endif
#ifdef HAVE_FFP_SHADER_SUPPORT
	uniform *ffp_binds[FFP_BINDS_NUM];
#endif
#ifdef HAVE_GLSL_TEXTURE_SIZE
	float **samplers_ptr;
#endif
	uniform *vert_uniforms;
	uniform *frag_uniforms;
	uint32_t vert_uniforms_num;
	uint32_t frag_uniforms_num;
	ubo *vert_ubos;
	ubo *frag_ubos;
	GLuint attr_highest_idx;
	GLboolean has_unaligned_attrs;
	GLboolean is_fbo_float;
	SceGxmMultisampleMode msaa; // the multisample mode fprog was patched for
	uint8_t num_glsl_attr;
	attr_mapping *glsl_attr_map;
	void *unif_fbuffer;
	void *unif_vbuffer;
	// vglDrawElementsDirect: vertex programs patched for a caller's layout keys (round robin). Every entry stays valid while the program does: the
	// vertex cache's own evictions forget a pointer, they do not release it, and the shader's patcher registration outlives the program (release_shader).
	uint32_t direct_key[VGL_DIRECT_VPROGS];
	SceGxmVertexProgram *direct_vprog[VGL_DIRECT_VPROGS];
	uint8_t direct_next;
	// vglSetDrawCaches: the fragment programs this program was patched into (round robin; each stays valid as p->fprog does: only the splash screen releases one)
	uint32_t memo_blend[VGL_FRAG_MEMO];
	SceGxmFragmentProgram *memo_fprog[VGL_FRAG_MEMO];
	uint8_t memo_mode[VGL_FRAG_MEMO];
	uint8_t memo_next;
	uint8_t fresh_draw; // 0040: linked and not drawn with yet (vglDrawElementsDirect times that first draw)
	// vglSetDrawCaches: the last copy of each default uniform buffer (vertex, fragment), the generation it belongs to (0: none), values written since
	void *unif_last[2];
	uint32_t unif_gen[2];
	uint8_t unif_changed[2];
	// be26 (vglSetDrawCaches uni_reuse 2): the last VGL_UNI_HIST copies of each default uniform buffer: their ring addresses, generations (0: empty), and the bytes they held (hdata, VGL_UNI_HIST slots of the buffer size, made at the first copy)
	void *unif_hptr[2][VGL_UNI_HIST];
	uint32_t unif_hgen[2][VGL_UNI_HIST];
	uint8_t *unif_hdata[2];
	uint32_t unif_hsize[2];
	uint8_t unif_hnext[2];
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	uint32_t vertex_layout_version;
#endif
} program;

// 1.3 (vglSetDrawCaches, frag_memo): what vitaGL asks the shader patcher at every change of a program's blend function, target format or samples, kept per program. The
// patcher keeps one object per combination, so the answer is the same pointer; what goes is the patcher's hash lookup. The vertex program a fragment program links to is
// the program's own, so the key is the three things setup_frag_program compares.
static inline __attribute__((always_inline)) SceGxmFragmentProgram *frag_memo_find(const program *p) {
	const uint8_t mode = (is_fbo_float ? 4 : 0) | (uint8_t)cur_msaa;
	for (int i = 0; i < VGL_FRAG_MEMO; i++) {
		if (p->memo_fprog[i] && p->memo_blend[i] == blend_info.raw && p->memo_mode[i] == mode)
			return p->memo_fprog[i];
	}
	return NULL;
}
static inline __attribute__((always_inline)) void frag_memo_add(program *p, SceGxmFragmentProgram *f) {
	p->memo_blend[p->memo_next] = blend_info.raw;
	p->memo_mode[p->memo_next] = (is_fbo_float ? 4 : 0) | (uint8_t)cur_msaa;
	p->memo_fprog[p->memo_next] = f;
	p->memo_next = (p->memo_next + 1) % VGL_FRAG_MEMO;
}
// be26: are the n bytes at a and b equal? 32 bytes a step (words, one branch a step; newlib's memcmp spends ten instructions on four bytes). n is a multiple of 4 (sizes of uniform buffers).
static inline __attribute__((always_inline)) int uni_hist_eq(const void *a, const void *b, uint32_t n) {
	const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
	uint32_t w = n >> 2;
	while (w >= 8) {
		if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3]) | (x[4] ^ y[4]) | (x[5] ^ y[5]) | (x[6] ^ y[6]) | (x[7] ^ y[7]))
			return 0;
		x += 8;
		y += 8;
		w -= 8;
	}
	while (w--) {
		if (*x++ != *y++)
			return 0;
	}
	return 1;
}
// be26 (uni_reuse 2): the ring address of a copy of this program's stage `st` (0 vertex, 1 fragment) made in this generation that holds exactly the n bytes at buf, or NULL.
static inline __attribute__((always_inline)) void *uni_hist_find(program *p, int st, const void *buf, uint32_t n) {
	if (!p->unif_hdata[st] || p->unif_hsize[st] != n || (vgl_uni_hist_cap && n > vgl_uni_hist_cap))
		return NULL;
	for (int i = 0; i < VGL_UNI_HIST; i++) {
		if (p->unif_hgen[st][i] == vgl_unif_gen && uni_hist_eq(p->unif_hdata[st] + (size_t)i * n, buf, n))
			return p->unif_hptr[st][i];
	}
	return NULL;
}
// be26: remember a copy just made (ring address `ring`, contents buf). The slots are taken round robin; the memory is made at the first copy while the budget lasts.
static inline __attribute__((always_inline)) void uni_hist_add(program *p, int st, void *ring, const void *buf, uint32_t n) {
	if (vgl_uni_hist_cap && n > vgl_uni_hist_cap)
		return;
	if (!p->unif_hdata[st]) {
		if (n == 0 || n > 65536 || vgl_uni_hist_bytes + n * VGL_UNI_HIST > VGL_UNI_HIST_BUDGET)
			return;
		p->unif_hdata[st] = (uint8_t *)vglMalloc(n * VGL_UNI_HIST);
		if (!p->unif_hdata[st])
			return;
		p->unif_hsize[st] = n;
		vgl_uni_hist_bytes += n * VGL_UNI_HIST;
		vgl_memset(p->unif_hgen[st], 0, sizeof(p->unif_hgen[st]));
		p->unif_hnext[st] = 0;
	}
	if (p->unif_hsize[st] != n)
		return;
	const uint8_t i = p->unif_hnext[st];
	vgl_fast_memcpy(p->unif_hdata[st] + (size_t)i * n, buf, n);
	p->unif_hptr[st][i] = ring;
	p->unif_hgen[st][i] = vgl_unif_gen;
	p->unif_hnext[st] = (uint8_t)((i + 1) % VGL_UNI_HIST);
}
static inline __attribute__((always_inline)) void uni_hist_free(program *p) {
	for (int st = 0; st < 2; st++) {
		if (p->unif_hdata[st]) {
			vgl_uni_hist_bytes -= p->unif_hsize[st] * VGL_UNI_HIST;
			vgl_free(p->unif_hdata[st]);
		}
		p->unif_hdata[st] = NULL;
		p->unif_hsize[st] = 0;
		p->unif_hnext[st] = 0;
		vgl_memset(p->unif_hgen[st], 0, sizeof(p->unif_hgen[st]));
	}
}
static inline __attribute__((always_inline)) void program_caches_reset(program *p) {
	vgl_memset(p->memo_fprog, 0, sizeof(p->memo_fprog));
	p->memo_next = 0;
	uni_hist_free(p);
	p->unif_gen[0] = p->unif_gen[1] = 0;
	p->unif_changed[0] = p->unif_changed[1] = 1;
}

void vglSetDrawCaches(GLboolean frag_memo, GLboolean uni_reuse) {
	vgl_frag_memo = frag_memo;
	vgl_uni_reuse = uni_reuse;
}

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
enum {
	VERTEX_LAYOUT_MULTI_DRAW,
	VERTEX_LAYOUT_ARRAYS,
	VERTEX_LAYOUT_ELEMENTS
};

typedef struct {
	program *prog;
	vao *vao;
	uint32_t prog_version;
	uint32_t vao_version;
	uint8_t type;
	uint8_t variant;
	GLboolean is_packed;
	GLboolean is_full_vbo;
	SceGxmVertexAttribute attr[VERTEX_ATTRIBS_NUM];
	SceGxmVertexStream stream[VERTEX_ATTRIBS_NUM];
	SceGxmVertexProgram *vprog;
} vertex_layout_cache_entry;

#define VERTEX_LAYOUT_CACHE_ENTRIES 4
static uint32_t vertex_program_layout_current_ver = 0;
static vertex_layout_cache_entry vertex_layout_cache[VERTEX_LAYOUT_CACHE_ENTRIES] = {};
static uint8_t vertex_layout_cache_id = 0;

static inline __attribute__((always_inline)) vertex_layout_cache_entry *vertex_layout_lookup(program *p, uint8_t type, uint8_t variant) {
	for (uint32_t i = 0; i < VERTEX_LAYOUT_CACHE_ENTRIES; i++) {
		vertex_layout_cache_entry *entry = &vertex_layout_cache[i];
		if (entry->prog == p && entry->vao == cur_vao && entry->type == type && entry->prog_version == p->vertex_layout_version && entry->vao_version == cur_vao->vertex_layout_version && entry->variant == variant) {
			return entry;
		}
	}
	return NULL;
}

static inline __attribute__((always_inline)) vertex_layout_cache_entry *vertex_layout_lookup_simple(program *p, uint8_t type) {
	for (uint32_t i = 0; i < VERTEX_LAYOUT_CACHE_ENTRIES; i++) {
		vertex_layout_cache_entry *entry = &vertex_layout_cache[i];
		if (entry->prog == p && entry->vao == cur_vao && entry->type == type && entry->prog_version == p->vertex_layout_version && entry->vao_version == cur_vao->vertex_layout_version) {
			return entry;
		}
	}
	return NULL;
}

static inline __attribute__((always_inline)) void vertex_layout_cache_insert(program *p, uint8_t type, uint8_t variant,
	GLboolean is_packed, GLboolean is_full_vbo, const SceGxmVertexAttribute *attributes, const SceGxmVertexStream *streams) {
	vertex_layout_cache_entry *entry = &vertex_layout_cache[vertex_layout_cache_id];
	vertex_layout_cache_id = (vertex_layout_cache_id + 1) & (VERTEX_LAYOUT_CACHE_ENTRIES - 1);
	entry->prog = p;
	entry->vao = cur_vao;
	entry->prog_version = p->vertex_layout_version;
	entry->vao_version = cur_vao->vertex_layout_version;
	entry->type = type;
	entry->variant = variant;
	entry->is_packed = is_packed;
	entry->is_full_vbo = is_full_vbo;
	vgl_fast_memcpy(entry->attr, attributes, p->attr_num * sizeof(SceGxmVertexAttribute));
	vgl_fast_memcpy(entry->stream, streams, p->attr_num * sizeof(SceGxmVertexStream));
	entry->vprog = p->vprog;
}

static inline __attribute__((always_inline)) void vertex_layout_setup_ptrs(program *p, GLboolean is_packed,
	uint32_t first, uint32_t packed_count, uint32_t unpacked_count, void **ptrs) {
	uint8_t first_attr_idx = p->attr_map[0];
	vbo *target_vbo = (vbo *)cur_vao->vertex_attrib_vbo[first_attr_idx];

	if (is_packed) {
		uint32_t stride = cur_vao->vertex_stream_config[first_attr_idx].stride;
		if (target_vbo) {
			ptrs[0] = (uint8_t *)target_vbo->ptr + first * stride;
			target_vbo->last_frame = vgl_framecount;
		} else {
			uint32_t size = packed_count * stride;
#ifdef SAFER_DRAW_SPEEDHACK
			if (size > SAFE_DRAW_SIZE_THRESHOLD) {
				ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[first_attr_idx] + first * stride;
			} else
#endif
			{
				ptrs[0] = gpu_alloc_mapped_temp(size);
				vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[first_attr_idx] + first * stride, size);
			}
		}
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			if (cur_vao->vertex_attrib_state & (1 << attr_idx)) {
				uint32_t stride = cur_vao->vertex_stream_config[attr_idx].stride;
				vbo *attr_vbo = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
				if (attr_vbo) {
					ptrs[i] = (uint8_t *)attr_vbo->ptr + cur_vao->vertex_attrib_offsets[attr_idx] + first * stride;
					attr_vbo->last_frame = vgl_framecount;
				} else {
					uint32_t size = unpacked_count * stride;
#ifdef SAFER_DRAW_SPEEDHACK
					if (size > SAFE_DRAW_SIZE_THRESHOLD) {
						ptrs[i] = (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * stride;
					} else
#endif
					{
						ptrs[i] = gpu_alloc_mapped_temp(size);
						vgl_fast_memcpy(ptrs[i], (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * stride, size);
					}
				}
			}
		}
	}
}
#endif

// Internal shaders and array
static shader *shaders[MAX_CUSTOM_SHADERS];
static program progs[MAX_CUSTOM_PROGRAMS];

#ifdef HAVE_SHARK_LOG
static char *shark_log = NULL;
#endif

#ifdef STRICT_UNIFORMS_COMPLIANCE
static inline __attribute__((always_inline)) uniform *get_uniform_from_ptr(GLint ptr, uint32_t *offset) {
	uniform_location u;
	u.raw = ptr;
	uniform *ret = u.is_vertex ? &progs[u.program_idx].vert_uniforms[u.uniform_idx] : &progs[u.program_idx].frag_uniforms[u.uniform_idx];
	*offset = u.offset;
	return ret;
}
#else
#define get_uniform_from_ptr(ptr, offs) (-ptr)
#endif

// 1.3 vgx uniform shadows (MSC_VGX_UNI_SHADOW; ported from the Galaxy port's patch 0015, GXV_UNI_SHADOW). Nothing changes unless a caller uses the two
// calls below. glUniform4fv / glUniformMatrix4fv (transpose GL_FALSE) on an F32 four-component uniform end in copies into u->vptr and u->fptr (the
// program's default uniform buffers), the program's unif_changed flag of each stage written and the global dirty flag of that stage
// (vgl_fill_uniform_data). For more than one element that copy goes through vglSetUniformData, element by element. A caller that keeps its own copy of
// what it gave can write the changed values into these buffers itself and then make one vglUniformShadowWritten call for the flags.
// vglGetUniformShadow: the addresses of a linked program's uniform in its vertex / fragment buffers (either may be null: the stage does not use it) and the
// bytes that belong to it there (array size x 16, bounded by what is left of each buffer). GL_FALSE for anything but a plain F32 vec4 / vec4 array / mat4
// of this program (sampler, other type or component count, element offset, unlinked program, a location of another program).
GLboolean vglGetUniformShadow(GLuint prog, GLint location, void **vptr, void **fptr, uint32_t *bytes) {
#if defined(HAVE_GLSL_TEXTURE_SIZE) || defined(HAVE_FFP_SHADER_SUPPORT)
	return GL_FALSE;
#else
	if (!prog || prog > MAX_CUSTOM_PROGRAMS || location == -1 || location == 0 || !vptr || !fptr || !bytes)
		return GL_FALSE;
	program *p = &progs[prog - 1];
	if (p->status != PROG_LINKED)
		return GL_FALSE;
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);
	if (offs != 0 || !u || u->type != UNIFORM_DATA || !u->ptr)
		return GL_FALSE;
	if (sceGxmProgramParameterGetType(u->ptr) != SCE_GXM_PARAMETER_TYPE_F32 || sceGxmProgramParameterGetComponentCount(u->ptr) != 4)
		return GL_FALSE;
	uint32_t room = sceGxmProgramParameterGetArraySize(u->ptr) * 16;
	if (!room)
		room = 16;
	// (the uniform must be this program's: its addresses lie in this program's buffers; what is left of each buffer bounds it)
	if (u->vptr) {
		const uint8_t *b = (const uint8_t *)p->unif_vbuffer, *a = (const uint8_t *)u->vptr;
		if (!b || a < b || a >= b + p->vshader->unif_buf_size)
			return GL_FALSE;
		if ((uint32_t)(b + p->vshader->unif_buf_size - a) < room)
			room = (uint32_t)(b + p->vshader->unif_buf_size - a);
	}
	if (u->fptr) {
		const uint8_t *b = (const uint8_t *)p->unif_fbuffer, *a = (const uint8_t *)u->fptr;
		if (!b || a < b || a >= b + p->fshader->unif_buf_size)
			return GL_FALSE;
		if ((uint32_t)(b + p->fshader->unif_buf_size - a) < room)
			room = (uint32_t)(b + p->fshader->unif_buf_size - a);
	}
	if (!u->vptr && !u->fptr)
		return GL_FALSE;
	*vptr = u->vptr;
	*fptr = u->fptr;
	*bytes = room;
	return GL_TRUE;
#endif
}
// What vgl_fill_uniform_data does after its copies, for the stages a caller wrote through vglGetUniformShadow's addresses (bit 0 vertex, bit 1 fragment):
// the program's unif_changed flag (no uni_reuse copy of an older value is named again) and the dirty flag of the stage. The program must be the current one,
// as for glUniform*.
void vglUniformShadowWritten(GLuint prog, uint32_t stages) {
#if !defined(HAVE_GLSL_TEXTURE_SIZE) && !defined(HAVE_FFP_SHADER_SUPPORT)
	if (!prog || prog > MAX_CUSTOM_PROGRAMS)
		return;
	program *p = &progs[prog - 1];
	if (stages & 1) {
		p->unif_changed[0] = 1;
		dirty_shader_vert_unifs = GL_TRUE;
	}
	if (stages & 2) {
		p->unif_changed[1] = 1;
		dirty_shader_frag_unifs = GL_TRUE;
	}
#endif
}

void release_shader(shader *s) {
	// Deallocating shader and unregistering it from sceGxmShaderPatcher
	if (s->prog) {
		vglVertexProgramCacheInvalidate(s->id);
		vgl_free((void *)s->prog);
		while (s->mat) {
			matrix_uniform *m = (matrix_uniform *)s->mat->chain;
			vgl_free(s->mat);
			s->mat = m;
		}
		while (s->unif_blk) {
			block_uniform *b = (block_uniform *)s->unif_blk->chain;
			vgl_free(s->unif_blk);
			s->unif_blk = b;
		}
	}
#ifdef HAVE_SHARK_LOG
	if (s->log)
		vgl_free(s->log);
#endif
	if (s->source) {
		vgl_free(s->source);
	}
	shaders[s->slot] = NULL;
	vgl_free(s);
}

float *reserve_attrib_pool(uint8_t count) {
	float *res = cur_vao->vertex_attrib_pool_ptr;
	cur_vao->vertex_attrib_pool_ptr += count;
	if (cur_vao->vertex_attrib_pool_ptr > cur_vao->vertex_attrib_pool_limit) {
		cur_vao->vertex_attrib_pool_ptr = cur_vao->vertex_attrib_pool + count;
		return cur_vao->vertex_attrib_pool;
	}
	return res;
}

static inline __attribute__((always_inline)) GLenum gxm_vd_fmt_to_gl(SceGxmAttributeFormat fmt) {
	switch (fmt) {
	case SCE_GXM_ATTRIBUTE_FORMAT_F16:
		return GL_HALF_FLOAT;
	case SCE_GXM_ATTRIBUTE_FORMAT_F32:
		return GL_FLOAT;
	case SCE_GXM_ATTRIBUTE_FORMAT_S16:
	case SCE_GXM_ATTRIBUTE_FORMAT_S16N:
		return GL_SHORT;
	case SCE_GXM_ATTRIBUTE_FORMAT_U16:
	case SCE_GXM_ATTRIBUTE_FORMAT_U16N:
		return GL_UNSIGNED_SHORT;
	case SCE_GXM_ATTRIBUTE_FORMAT_S8:
	case SCE_GXM_ATTRIBUTE_FORMAT_S8N:
		return GL_BYTE;
	case SCE_GXM_ATTRIBUTE_FORMAT_U8:
	case SCE_GXM_ATTRIBUTE_FORMAT_U8N:
		return GL_UNSIGNED_BYTE;
	default:
		return GL_FLOAT;
	}
}

static inline __attribute__((always_inline)) GLenum gxm_attr_type_to_gl(uint8_t size, uint8_t num) {
	switch (size * num) {
	case 1:
		return GL_FLOAT;
	case 2:
		return GL_FLOAT_VEC2;
	case 3:
		return GL_FLOAT_VEC3;
	case 4:
		return GL_FLOAT_VEC4;
	case 9:
		return GL_FLOAT_MAT3;
	case 16:
		return GL_FLOAT_MAT4;
	default:
		return GL_FLOAT;
	}
}

static inline __attribute__((always_inline)) GLenum gxm_unif_type_to_gl(SceGxmParameterType type, uint8_t count, int *size) {
	switch (type) {
	case SCE_GXM_PARAMETER_TYPE_F32:
	case SCE_GXM_PARAMETER_TYPE_F16:
	case SCE_GXM_PARAMETER_TYPE_C10:
		switch (count) {
		case 1:
			return GL_FLOAT;
		case 2:
			return GL_FLOAT_VEC2;
		case 3:
			return GL_FLOAT_VEC3;
		case 4:
			return GL_FLOAT_VEC4;
		default:
			return GL_FLOAT;
		}
	case SCE_GXM_PARAMETER_TYPE_U32:
	case SCE_GXM_PARAMETER_TYPE_S32:
		switch (count) {
		case 1:
			return GL_INT;
		case 2:
			return GL_INT_VEC2;
		case 3:
			return GL_INT_VEC3;
		case 4:
			return GL_INT_VEC4;
		default:
			return GL_INT;
		}
	default:
		return GL_FLOAT;
	}
}

static inline __attribute__((always_inline)) void gxm_unif_to_mat(GLenum *type, int *size) {
	switch (*type) {
	case GL_FLOAT_VEC2:
		*type = GL_FLOAT_MAT2;
		*size = *size / 2;
		break;
	case GL_FLOAT_VEC3:
		*type = GL_FLOAT_MAT3;
		*size = *size / 3;
		break;
	case GL_FLOAT_VEC4:
		*type = GL_FLOAT_MAT4;
		*size = *size / 4;
		break;
	default:
		break;
	}
}

static inline __attribute__((always_inline)) size_t serialized_shader_size(shader *s, GLboolean have_bindings, uint32_t *matrix_uniforms_num) {
	*matrix_uniforms_num = 0;
	matrix_uniform *m = s->mat;
	while (m) {
		*matrix_uniforms_num = *matrix_uniforms_num + 1;
		m = (matrix_uniform *)m->chain;
	}
	size_t sz = (1 + *matrix_uniforms_num) * sizeof(uint32_t) + s->size;
	if (have_bindings)
		sz += sizeof(binds_map);
#ifdef HAVE_GLSL_TEXTURE_SIZE
	sz += sizeof(glsl_samplers_info) * s->sized_samplers_num + sizeof(uint8_t);
#endif
	return sz;
}

void *serialize_shader(void *out, size_t *sz, shader *s, GLboolean save_bindings) {
	uint32_t matrix_uniforms_num;
	*sz = serialized_shader_size(s, save_bindings, &matrix_uniforms_num);
	void *_out = out;
	if (!_out) {
		_out = vglMalloc(*sz);
		if (!_out)
			return NULL;
	}
	uint8_t *buf = (uint8_t *)_out;
	vgl_fast_memcpy(buf, &matrix_uniforms_num, sizeof(uint32_t));
	buf += sizeof(uint32_t);
	matrix_uniform *m = s->mat;
	while (m) {
		uint32_t idx = sceGxmProgramParameterGetIndex(s->prog, m->ptr);
		vgl_fast_memcpy(buf, &idx, sizeof(uint32_t));
		buf += sizeof(uint32_t);
		m = (matrix_uniform *)m->chain;
	}
	if (save_bindings) {
		vgl_fast_memcpy(buf, &s->semantics, sizeof(binds_map));
		buf += sizeof(binds_map);
	}
#ifdef HAVE_GLSL_TEXTURE_SIZE
	*buf = s->sized_samplers_num;
	buf++;
	if (s->sized_samplers_num) {
		vgl_fast_memcpy(buf, s->sized_samplers, sizeof(glsl_samplers_info) * s->sized_samplers_num);
		buf += sizeof(glsl_samplers_info) * s->sized_samplers_num;
	}
#endif
	vgl_fast_memcpy(buf, s->prog, s->size);
	return _out;
}

#include "utils/shader_candidate.h"
#include "utils/shader_binary.h"

GLboolean unserialize_shader(void *in, size_t sz, shader *s, GLboolean load_bindings) {
	if (!in || sz < sizeof(uint32_t) || s->prog)
		return GL_FALSE;
	uint8_t *buf = (uint8_t *)in;
	uint32_t matrix_uniforms_num;
	vgl_fast_memcpy(&matrix_uniforms_num, buf, sizeof(uint32_t));
	if (matrix_uniforms_num > (sz - sizeof(uint32_t)) / sizeof(uint32_t))
		return GL_FALSE;
	size_t offset = sizeof(uint32_t) + (size_t)matrix_uniforms_num * sizeof(uint32_t);
	shader candidate = {0};
	if (load_bindings) {
		if (sizeof(binds_map) > sz - offset)
			return GL_FALSE;
		vgl_fast_memcpy(&candidate.semantics, buf + offset, sizeof(binds_map));
		offset += sizeof(binds_map);
	}
#ifdef HAVE_GLSL_TEXTURE_SIZE
	if (offset == sz)
		return GL_FALSE;
	candidate.sized_samplers_num = buf[offset++];
	if (candidate.sized_samplers_num > SCE_GXM_MAX_TEXTURE_UNITS || candidate.sized_samplers_num > (sz - offset) / sizeof(glsl_samplers_info))
		return GL_FALSE;
	if (candidate.sized_samplers_num) {
		vgl_fast_memcpy(candidate.sized_samplers, buf + offset, sizeof(glsl_samplers_info) * candidate.sized_samplers_num);
		for (uint8_t i = 0; i < candidate.sized_samplers_num; ++i)
			if (!memchr(candidate.sized_samplers[i].name, 0, sizeof(candidate.sized_samplers[i].name)))
				return GL_FALSE;
		offset += sizeof(glsl_samplers_info) * candidate.sized_samplers_num;
	}
#endif
	if (sz - offset > UINT32_MAX || !shader_binary_valid(buf + offset, sz - offset, s->type))
		return GL_FALSE;
	candidate.size = sz - offset;
	candidate.prog = (SceGxmProgram *)vglMalloc(candidate.size);
	if (!candidate.prog)
		return GL_FALSE;
	vgl_fast_memcpy((void *)candidate.prog, buf + offset, candidate.size);
	if (sceGxmProgramCheck(candidate.prog))
		goto failed;
	candidate.unif_buf_size = sceGxmProgramGetDefaultUniformBufferSize(candidate.prog);
	if (matrix_uniforms_num) {
		uint32_t count = sceGxmProgramGetParameterCount(candidate.prog);
		uint32_t *ptr = vglProgramGetParameterBase(candidate.prog);
		for (uint32_t i = 0; i < matrix_uniforms_num; i++) {
			uint32_t index;
			vgl_fast_memcpy(&index, buf + sizeof(uint32_t) + (size_t)i * sizeof(uint32_t), sizeof(index));
			if (index >= count)
				goto failed;
			matrix_uniform *m = (matrix_uniform *)vglMalloc(sizeof(matrix_uniform));
			if (!m)
				goto failed;
			m->chain = candidate.mat;
			m->ptr = (SceGxmProgramParameter *)(ptr + (size_t)index * 4);
			candidate.mat = m;
		}
	}
	if (vgl_register_program(candidate.prog, &candidate.id))
		goto failed;
	commit_shader_candidate(s, &candidate);
	if (load_bindings)
		s->semantics = candidate.semantics;
#ifdef HAVE_GLSL_TEXTURE_SIZE
	s->sized_samplers_num = candidate.sized_samplers_num;
	vgl_fast_memcpy(s->sized_samplers, candidate.sized_samplers, sizeof(candidate.sized_samplers));
#endif
	return GL_TRUE;
failed:
	discard_shader_candidate(&candidate);
	return GL_FALSE;
}

#ifdef HAVE_SHADER_CACHE
static GLboolean load_shader_cache(SceUID f, shader *s, GLboolean bindings) {
	if (f < 0)
		return GL_FALSE;
	GLboolean ok = GL_FALSE;
	void *buf = NULL;
	SceOff size = sceIoLseek(f, 0, SCE_SEEK_END);
	// Local seed maximum is 3140 bytes; leave ample room without trusting a corrupt seek.
	if (size > 0 && size <= 1024 * 1024 && sceIoLseek(f, 0, SCE_SEEK_SET) == 0) {
		buf = vglMalloc((size_t)size);
		if (buf) {
			size_t done = 0;
			while (done < (size_t)size) {
				SceSSize n = sceIoRead(f, (uint8_t *)buf + done, (size_t)size - done);
				if (n <= 0 || (size_t)n > (size_t)size - done)
					break;
				done += n;
			}
			ok = done == (size_t)size;
		}
	}
	if (sceIoClose(f) < 0)
		ok = GL_FALSE;
	if (ok)
		ok = unserialize_shader(buf, (size_t)size, s, bindings);
	vgl_free(buf);
	return ok;
}
#endif

#ifdef HAVE_SHADER_CACHE
#define vgl_compile_shader(shd, sv) compile_shader(shd, sv, fname, fname_len)
#include "utils/shader_cache_io.h"
/* Compile-time capability, valid before vglInit: bit 0 owns bucket creation. */
unsigned vglStrikersShaderCacheCaps(void) { return 1u; }
/* The cache version folder this library writes under the cache path ("v<n>", SHADER_CACHE_MAGIC): the host needs it before vglInit
   (upstream moved it from 2 to 3 together with the 16-digit file names, 13 Oct 2026). */
unsigned vglStrikersShaderCacheVersion(void) { return SHADER_CACHE_MAGIC; }
static inline __attribute__((always_inline)) void compile_shader(shader *s, GLboolean save_bindings, const char *cache_fname, int fname_len) {
#else
#define vgl_compile_shader(shd, sv) compile_shader(shd, sv)
static inline __attribute__((always_inline)) void compile_shader(shader *s, GLboolean save_bindings) {
#endif
#ifdef HAVE_SHADER_CACHE
	GLboolean cache_ready = GL_FALSE;
#endif
	if (s->prog || !s->source)
		return;
	shader candidate = {0};
	// vitaShaRK reads *size as the length of the source text and writes the program's size back into it: it must go in as the
	// source length (a zero here gave the compiler an empty file, "fatal internal error on line -1", for every fresh compile).
	candidate.size = (uint32_t)strlen((const char *)s->source);
	// The compiler owns its output. Do not publish it or clobber the source length.
	const SceGxmProgram *output = shark_compile_shader_extended((const char *)s->source, &candidate.size, s->type == GL_FRAGMENT_SHADER ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER, compiler_opts, compiler_fastmath, compiler_fastprecision, compiler_fastint);
	if (output && candidate.size) {
		SceGxmProgram *res = (SceGxmProgram *)vglMalloc(candidate.size);
		if (!res)
			goto done;
		candidate.prog = res;
		vgl_fast_memcpy(res, output, candidate.size);
		candidate.unif_buf_size = sceGxmProgramGetDefaultUniformBufferSize(res);
		SceShaccCgCompileOutput *cout = (SceShaccCgCompileOutput *)shark_get_internal_compile_output();
		SceShaccCgParameter param = sceShaccCgGetFirstParameter(cout);
		while (param) {
			if (sceShaccCgGetParameterClass(param) == SCE_SHACCCG_PARAMETERCLASS_MATRIX) {
				const SceGxmProgramParameter *ptr = sceGxmProgramFindParameterByName(res, sceShaccCgGetParameterName(param));
				if (!ptr) {
					param = sceShaccCgGetNextParameter(param);
					continue; // The compiler optimized out this matrix.
				}
				matrix_uniform *m = (matrix_uniform *)vglMalloc(sizeof(matrix_uniform));
				if (!m)
					goto done;
				m->ptr = ptr;
				m->chain = candidate.mat;
				candidate.mat = m;
			} else if (sceShaccCgGetParameterClass(param) == SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK) {
				const char *name = sceShaccCgGetParameterName(param);
				if (!name || strlen(name) >= sizeof(((block_uniform *)0)->name))
					goto done;
				block_uniform *b = (block_uniform *)vglMalloc(sizeof(block_uniform));
				if (!b)
					goto done;
				b->idx = sceShaccCgGetParameterBufferIndex(param);
				strcpy(b->name, name);
				b->chain = candidate.unif_blk;
				candidate.unif_blk = b;
			}
			param = sceShaccCgGetNextParameter(param);
		}
		// Registration is last: no fallible work may leave a registered partial shader.
		int r = vgl_register_program(res, &candidate.id);
		if (r) {
			vgl_log("%s: Program registration failed (0x%08X).\n", __func__, r);
			goto done;
		}
		commit_shader_candidate(s, &candidate);
		candidate.prog = NULL;
		candidate.mat = NULL;
		candidate.unif_blk = NULL;
		vgl_free(s->source);
		s->source = NULL;
#ifdef HAVE_SHADER_CACHE
		cache_ready = GL_TRUE;
#endif
	}
done:
	discard_shader_candidate(&candidate);
#ifdef HAVE_SHARK_LOG
	if (s->log)
		vgl_free(s->log);
	s->log = shark_log;
	shark_log = NULL;
#endif
	shark_clear_output();
#ifdef HAVE_SHADER_CACHE
	if (cache_ready)
		vgl_cache_compiled_shader(s, save_bindings, cache_fname);
#endif
}

/* 1.3 ASYNC_SHADERS (patch 0027; the Galaxy port's 0020 vglCompileShaderPairToCache, written again from its description): a shader is compiled
   into the shader cache on another thread, never registered with the patcher. Every compile (this one and glCompileShader's translate and
   compile) holds the compiler lock: the translator's and vitaShaRK's state is global. The render thread's next glCompileShader of the same
   text finds the cache file and loads it. Without vglShaderCompilerLockInit nothing here changes: the lock is not taken. */
static SceUID vgl_compiler_lock = -1;
int vglShaderCompilerLockInit(void) {
	if (vgl_compiler_lock >= 0)
		return 0;
	SceUID m = sceKernelCreateMutex("vgl compiler", 0, 0, NULL);
	if (m < 0)
		return m;
	vgl_compiler_lock = m;
	return 0;
}
static inline void vgl_compiler_enter(void) {
	if (vgl_compiler_lock >= 0)
		sceKernelLockMutex(vgl_compiler_lock, 1, NULL);
}
static inline void vgl_compiler_leave(void) {
	if (vgl_compiler_lock >= 0)
		sceKernelUnlockMutex(vgl_compiler_lock, 1);
}

#ifdef HAVE_SHADER_CACHE
/* The text glShaderSource would make of these parts, and its cache file name (glCompileShader's spelling). */
static char *vgl_async_source(GLenum type, const char *const *parts, int count, uint32_t *size, uint64_t *hash, char *fname) {
	uint32_t n = 0;
	for (int i = 0; i < count; i++)
		n += strlen(parts[i]);
	char *src = (char *)vglMalloc(n + 1);
	if (!src)
		return NULL;
	n = 0;
	for (int i = 0; i < count; i++) {
		size_t l = strlen(parts[i]);
		vgl_fast_memcpy(&src[n], parts[i], l);
		n += l;
	}
	src[n] = 0;
	*size = n;
	*hash = XXH3_64bits(src, n);
	sprintf(fname, "%s/%s/%02X/%016llX.gxp", vgl_shader_cache_path, type == GL_VERTEX_SHADER ? "v" : "f", (uint8_t)(*hash >> 56), *hash);
	return src;
}
static int vgl_async_cached(GLenum type, uint64_t hash, const char *fname) {
	SceIoStat st;
	if (vgl_shader_pack_lookup) {
		uint32_t psz = 0;
		if (vgl_shader_pack_lookup(type == GL_FRAGMENT_SHADER, hash, &psz))
			return 1;
	}
	if (sceIoGetstat(fname, &st) >= 0 && st.st_size > 0)
		return 1;
	SceUID f = vgl_open_seed(type == GL_VERTEX_SHADER ? "v" : "f", (uint8_t)(hash >> 56), hash);
	if (f >= 0) {
		sceIoClose(f);
		return 1;
	}
	return 0;
}
#endif

/* 1 = the pack, the cache or the seed has the compiled form of this text (glCompileShader would load it), 0 = it would compile. */
int vglShaderCacheProbe(GLenum type, const char *const *parts, int count) {
#ifdef HAVE_SHADER_CACHE
	char fname[256];
	uint32_t size;
	uint64_t hash;
	if (!vgl_shader_cache_path[0])
		return 0;
	char *src = vgl_async_source(type, parts, count, &size, &hash, fname);
	if (!src)
		return 0;
	vgl_free(src);
	return vgl_async_cached(type, hash, fname);
#else
	return 0;
#endif
}

/* Compiles the text into the cache: 1 compiled and written, 2 the pack / cache / seed has it already, 0 the compile or the write failed,
   -1 no cache path, -2 the compiler is not online. Any thread; the GL state is not touched. */
int vglCompileShaderToCache(GLenum type, const char *const *parts, int count) {
#ifdef HAVE_SHADER_CACHE
	char fname[256];
	uint32_t size;
	uint64_t hash;
	if (!vgl_shader_cache_path[0])
		return -1;
	shader s = {0};
	s.type = type;
	s.is_glsl = GL_TRUE;
	s.source = vgl_async_source(type, parts, count, &size, &hash, fname);
	if (!s.source)
		return 0;
	s.size = size;
	if (vgl_async_cached(type, hash, fname)) {
		vgl_free(s.source);
		return 2;
	}
	int result = 0;
	vgl_compiler_enter();
	if (!is_shark_online && !start_shader_compiler()) {
		vgl_compiler_leave();
		vgl_free(s.source);
		return -2;
	}
	glsl_translator_process(&s);
	shader candidate = {0};
	candidate.type = type;
	candidate.size = (uint32_t)strlen((const char *)s.source);
	const SceGxmProgram *output = shark_compile_shader_extended((const char *)s.source, &candidate.size, type == GL_FRAGMENT_SHADER ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER, compiler_opts, compiler_fastmath, compiler_fastprecision, compiler_fastint);
	if (output && candidate.size) {
		SceGxmProgram *res = (SceGxmProgram *)vglMalloc(candidate.size);
		if (res) {
			candidate.prog = res;
			vgl_fast_memcpy(res, output, candidate.size);
			GLboolean ok = GL_TRUE;
			SceShaccCgCompileOutput *cout = (SceShaccCgCompileOutput *)shark_get_internal_compile_output();
			SceShaccCgParameter param = sceShaccCgGetFirstParameter(cout);
			while (param && ok) {
				if (sceShaccCgGetParameterClass(param) == SCE_SHACCCG_PARAMETERCLASS_MATRIX) {
					const SceGxmProgramParameter *ptr = sceGxmProgramFindParameterByName(res, sceShaccCgGetParameterName(param));
					if (ptr) {
						matrix_uniform *m = (matrix_uniform *)vglMalloc(sizeof(matrix_uniform));
						if (!m)
							ok = GL_FALSE;
						else {
							m->ptr = ptr;
							m->chain = candidate.mat;
							candidate.mat = m;
						}
					}
				} else if (sceShaccCgGetParameterClass(param) == SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK) {
					const char *name = sceShaccCgGetParameterName(param);
					if (!name || strlen(name) >= sizeof(((block_uniform *)0)->name))
						ok = GL_FALSE; /* compile_shader refuses this shader too */
				}
				param = sceShaccCgGetNextParameter(param);
			}
			if (ok) {
				candidate.semantics = s.semantics;
#ifdef HAVE_GLSL_TEXTURE_SIZE
				candidate.sized_samplers_num = s.sized_samplers_num;
				vgl_fast_memcpy(candidate.sized_samplers, s.sized_samplers, sizeof(candidate.sized_samplers));
#endif
				vgl_cache_compiled_shader(&candidate, GL_FALSE, fname);
				SceIoStat st;
				result = sceIoGetstat(fname, &st) >= 0 && st.st_size > 0 ? 1 : 0;
			}
		}
	}
	discard_shader_candidate(&candidate);
#ifdef HAVE_SHARK_LOG
	if (shark_log)
		vgl_free(shark_log);
	shark_log = NULL;
#endif
	shark_clear_output();
	vgl_compiler_leave();
	vgl_free(s.source);
	return result;
#else
	return -1;
#endif
}

void reset_custom_shaders(void) {
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	sceClibMemset(&vertex_layout_cache, 0, sizeof(vertex_layout_cache));
	vertex_layout_cache_id = 0;
#endif

	// Init custom shaders
	sceClibMemset(shaders, 0, sizeof(uint32_t) * MAX_CUSTOM_SHADERS);

	// Init custom programs
	for (int i = 0; i < MAX_CUSTOM_PROGRAMS; i++) {
		progs[i].status = PROG_INVALID;
	}
}

void _glMultiDrawArrays_CustomShadersIMPL(SceGxmPrimitiveType gxm_p, uint16_t *idx_ptr, const GLint *first, const GLsizei *count, GLint lowest, GLsizei highest, GLsizei drawcount) {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];
	VGL_STREAM_FLUSH_IF_NEEDED() // GLB

	// Check if a blend info rebuild is required and upload fragment program
	setup_frag_program_void();

	// Uploading fragment textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->frag_texunits[i]->sampler_index];
			uint8_t tex_type = p->frag_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifdef HAVE_TEX_CACHE
			restore_tex_cache(tex);
#endif
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->frag_texunits[i]->sampler_index];
			if (smp) {
				vglSetTexMinFilter(&tex->gxm_tex, smp->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, smp->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, smp->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, smp->u_mode);
				vglSetTexVMode(&tex->gxm_tex, smp->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, smp->use_mips ? tex->mip_count : 0);
				vglSetTexLodBias(&tex->gxm_tex, smp->lod_bias);
				tex->overridden = GL_TRUE;
			} else if (tex->overridden) {
				vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
				vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 0);
				vglSetTexLodBias(&tex->gxm_tex, tex->lod_bias);
				tex->overridden = GL_FALSE;
			}
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				p->unif_changed[1] = 1;
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading vertex textures on relative texture units
	for (int i = 0; i < p->max_vert_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->vert_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->vert_texunits[i]->sampler_index];
			uint8_t tex_type = p->vert_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->vert_texunits[i]->sampler_index];
			if (smp) {
				vglSetTexMinFilter(&tex->gxm_tex, smp->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, smp->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, smp->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, smp->u_mode);
				vglSetTexVMode(&tex->gxm_tex, smp->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, smp->use_mips ? tex->mip_count : 0);
				tex->overridden = GL_TRUE;
			} else if (tex->overridden) {
				vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
				vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 0);
				tex->overridden = GL_FALSE;
			}
			sceGxmSetVertexTexture(gxm_context, i, &tex->gxm_tex);
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	vertex_layout_cache_entry *layout_entry = vertex_layout_lookup(p, VERTEX_LAYOUT_MULTI_DRAW, 0);
	if (layout_entry) {
#ifdef HAVE_PROFILING
		vgl_vcache_stats.layout_hits++;
#endif
		void *ptrs[VERTEX_ATTRIBS_NUM];
		SceGxmVertexStream *streams = layout_entry->stream;
		vertex_layout_setup_ptrs(p, layout_entry->is_packed, lowest, highest - lowest, highest, ptrs);
		p->vprog = layout_entry->vprog;
		sceGxmSetVertexProgram(gxm_context, p->vprog);
		upload_uniforms();
		for (int j = 0; j < drawcount; j++) {
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				if (cur_vao->vertex_attrib_state & (1 << attr_idx)) {
					if (layout_entry->is_packed) {
						sceGxmSetVertexStream(gxm_context, i, ptrs[0] + (first[j] - lowest) * streams[0].stride);
					} else {
						sceGxmSetVertexStream(gxm_context, i, ptrs[i] + (first[j] - lowest) * streams[i].stride);
					}
				}
			}
			sceGxmDraw(gxm_context, gxm_p, SCE_GXM_INDEX_FORMAT_U16, idx_ptr, count[j]);
		}
#ifdef HAVE_PROFILING
		shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
		shaders_draw_cnt++;
#endif
		return;
	}
#ifdef HAVE_PROFILING
	vgl_vcache_stats.layout_misses++;
#endif
#endif

	// Aligning attributes
	SceGxmVertexAttribute *attributes;
	SceGxmVertexStream *streams;
	align_attributes(attributes, streams);

	void *ptrs[VERTEX_ATTRIBS_NUM];
	vbo *target_vbo = (vbo *)cur_vao->vertex_attrib_vbo[p->attr_map[0]];
#ifdef STRICT_DRAW_COMPLIANCE
	GLboolean is_packed[VERTEX_ATTRIBS_NUM];
	vgl_memset(is_packed, GL_TRUE, p->attr_num * sizeof(GLboolean));
#else
	GLboolean is_packed = p->attr_num > 1;
	if (is_packed) {
#endif
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			vbo *attr_vbo = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
			if (attr_vbo) {
#ifdef STRICT_DRAW_COMPLIANCE
				vgl_memset(is_packed, 0, p->attr_num * sizeof(GLboolean));
				break;
#else
				if (attr_vbo != target_vbo) {
					is_packed = GL_FALSE;
					break;
				}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
			} else {
				if (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[attr_idx] && cur_vao->vertex_attrib_offsets[attr_idx] >= cur_vao->vertex_attrib_offsets[p->attr_map[0]])) {
					is_packed[attr_idx] = GL_FALSE;
				}
#endif
			}
		}
#ifndef STRICT_DRAW_COMPLIANCE
		if (is_packed && (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[p->attr_map[1]] && cur_vao->vertex_attrib_offsets[p->attr_map[1]] > cur_vao->vertex_attrib_offsets[p->attr_map[0]])))
			is_packed = GL_FALSE;
	}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
	// Gathering real attribute data pointers
	if (is_packed[0]) {
		ptrs[0] = gpu_alloc_mapped_temp(highest * streams[0].stride);
		vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + lowest * streams[0].stride, highest * streams[0].stride);
	}
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		attributes[i].regIndex = p->attr[attr_idx].regIndex;
		if (is_packed[i]) {
			handle_packed_attrib();
		} else {
			handle_unpacked_attrib(lowest, highest);
		}
	}
#else
	// 0059: the packed-VBO attribute offset is 16 bits (SceGxmVertexAttribute.offset): an attribute further into the buffer than 64 KiB is read from the wrong place
	if (is_packed && target_vbo && !vgl_pack_offsets_fit(cur_vao->vertex_attrib_offsets, cur_vao->vertex_attrib_state, p->attr_map, p->attr_num))
		is_packed = GL_FALSE;
	// Gathering real attribute data pointers
	if (is_packed) {
		if (target_vbo) {
			ptrs[0] = (void *)target_vbo->ptr + lowest * streams[0].stride;
			target_vbo->last_frame = vgl_framecount;
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_vbo_attrib();
			}
		} else {
			ptrs[0] = gpu_alloc_mapped_temp((highest - lowest) * streams[0].stride);
			vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + lowest * streams[0].stride, (highest - lowest) * streams[0].stride);		
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_attrib();
			}
		}
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			attributes[i].regIndex = p->attr[attr_idx].regIndex;
			handle_unpacked_attrib(lowest, highest);
		}
	}
#endif

#ifndef INDICES_SPEEDHACK
	for (int i = 0; i < p->attr_num; i++) {
		streams[i].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	}
#endif

	// Uploading new vertex program
	if (patch_vertex_program(p->vshader->id, attributes, p->attr_num, streams, p->attr_num, &p->vprog) && vgl_robust_scenes)
		return; // 1.2: a layout that could not be made is not drawn with the previous one's program
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	vertex_layout_cache_insert(p, VERTEX_LAYOUT_MULTI_DRAW, 0, is_packed, GL_FALSE, attributes, streams);
#endif
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();
	
	for (int j = 0; j < drawcount; j++) {
		// Uploading vertex streams
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			GLboolean is_active = (cur_vao->vertex_attrib_state & (1 << attr_idx)) ? GL_TRUE : GL_FALSE;
			if (is_active) {
#ifdef STRICT_DRAW_COMPLIANCE
				if (is_packed[i])
					sceGxmSetVertexStream(gxm_context, i, ptrs[0] + (first[j] - lowest) * streams[0].stride);
				else
					sceGxmSetVertexStream(gxm_context, i, ptrs[i] + (first[j] - lowest) * streams[i].stride);
#else
				if (is_packed)
					sceGxmSetVertexStream(gxm_context, i, ptrs[0] + (first[j] - lowest) * streams[0].stride);
				else
					sceGxmSetVertexStream(gxm_context, i, ptrs[i] + (first[j] - lowest) * streams[i].stride);
#endif
			}
		}
		
		sceGxmDraw(gxm_context, gxm_p, SCE_GXM_INDEX_FORMAT_U16, idx_ptr, count[j]);
	}
#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
}

GLboolean _glDrawArrays_CustomShadersIMPL(GLint first, GLsizei count, GLboolean instanced) {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];
	VGL_STREAM_FLUSH_IF_NEEDED() // GLB

	// Check if a blend info rebuild is required and upload fragment program
	setup_frag_program();

	// Uploading fragment textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->frag_texunits[i]->sampler_index];
			uint8_t tex_type = p->frag_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifdef HAVE_TEX_CACHE
			restore_tex_cache(tex);
#endif
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->frag_texunits[i]->sampler_index];
			if (smp) {
				vglSetTexMinFilter(&tex->gxm_tex, smp->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, smp->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, smp->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, smp->u_mode);
				vglSetTexVMode(&tex->gxm_tex, smp->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, smp->use_mips ? tex->mip_count : 0);
				vglSetTexLodBias(&tex->gxm_tex, smp->lod_bias);
				tex->overridden = GL_TRUE;
			} else if (tex->overridden) {
				vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
				vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 0);
				vglSetTexLodBias(&tex->gxm_tex, tex->lod_bias);
				tex->overridden = GL_FALSE;
			}
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				p->unif_changed[1] = 1;
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading vertex textures on relative texture units
	for (int i = 0; i < p->max_vert_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->vert_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->vert_texunits[i]->sampler_index];
			uint8_t tex_type = p->vert_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->vert_texunits[i]->sampler_index];
			if (smp) {
				vglSetTexMinFilter(&tex->gxm_tex, smp->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, smp->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, smp->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, smp->u_mode);
				vglSetTexVMode(&tex->gxm_tex, smp->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, smp->use_mips ? tex->mip_count : 0);
				tex->overridden = GL_TRUE;
			} else if (tex->overridden) {
				vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
				vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 0);
				tex->overridden = GL_FALSE;
			}
			sceGxmSetVertexTexture(gxm_context, i, &tex->gxm_tex);
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	uint8_t layout_variant = instanced ? 1 : 0;
	vertex_layout_cache_entry *layout_entry = vertex_layout_lookup(p, VERTEX_LAYOUT_ARRAYS, layout_variant);
	if (layout_entry) {
#ifdef HAVE_PROFILING
		vgl_vcache_stats.layout_hits++;
#endif
		void *ptrs[VERTEX_ATTRIBS_NUM];
		vertex_layout_setup_ptrs(p, layout_entry->is_packed, first, count, count, ptrs);
		p->vprog = layout_entry->vprog;
		sceGxmSetVertexProgram(gxm_context, p->vprog);
		upload_uniforms();
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			if (cur_vao->vertex_attrib_state & (1 << attr_idx)) {
				if (layout_entry->is_packed)
					sceGxmSetVertexStream(gxm_context, i, ptrs[0]);
				else
					sceGxmSetVertexStream(gxm_context, i, ptrs[i]);
			} else {
				sceGxmSetVertexStream(gxm_context, i, cur_vao->vertex_attrib_value[attr_idx]);
			}
		}
#ifdef HAVE_PROFILING
		shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
		shaders_draw_cnt++;
#endif
		return GL_TRUE;
	}
#ifdef HAVE_PROFILING
	vgl_vcache_stats.layout_misses++;
#endif
#endif

	// Aligning attributes
	SceGxmVertexAttribute *attributes;
	SceGxmVertexStream *streams;
	align_attributes(attributes, streams);

	void *ptrs[VERTEX_ATTRIBS_NUM];
#ifndef DRAW_SPEEDHACK
	vbo *target_vbo = (vbo *)cur_vao->vertex_attrib_vbo[p->attr_map[0]];
#ifdef STRICT_DRAW_COMPLIANCE
	GLboolean is_packed[VERTEX_ATTRIBS_NUM];
	vgl_memset(is_packed, GL_TRUE, p->attr_num * sizeof(GLboolean));
#else
	GLboolean is_packed = p->attr_num > 1;
	if (is_packed) {
#endif
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			vbo *attr_vbo = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
			if (attr_vbo) {
#ifdef STRICT_DRAW_COMPLIANCE
				vgl_memset(is_packed, 0, p->attr_num * sizeof(GLboolean));
				break;
#else
				if (attr_vbo != target_vbo) {
					is_packed = GL_FALSE;
					break;
				}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
			} else {
				if (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[attr_idx] && cur_vao->vertex_attrib_offsets[attr_idx] >= cur_vao->vertex_attrib_offsets[p->attr_map[0]])) {
					is_packed[attr_idx] = GL_FALSE;
				}
#endif
			}
		}
#ifndef STRICT_DRAW_COMPLIANCE
		if (is_packed && (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[p->attr_map[1]] && cur_vao->vertex_attrib_offsets[p->attr_map[1]] > cur_vao->vertex_attrib_offsets[p->attr_map[0]])))
			is_packed = GL_FALSE;
	}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
	// Gathering real attribute data pointers
	if (is_packed[0]) {
#ifdef SAFER_DRAW_SPEEDHACK
		if (count * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
			ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride;
		} else
#endif
		{
			ptrs[0] = gpu_alloc_mapped_temp(count * streams[0].stride);
			vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride, count * streams[0].stride);
		}
	}
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		attributes[i].regIndex = p->attr[attr_idx].regIndex;
		if (is_packed[i]) {
			handle_packed_attrib();
		} else {
			handle_unpacked_attrib(first, count);
		}
	}
#else
	// 0059: the packed-VBO attribute offset is 16 bits (SceGxmVertexAttribute.offset): an attribute further into the buffer than 64 KiB is read from the wrong place
	if (is_packed && target_vbo && !vgl_pack_offsets_fit(cur_vao->vertex_attrib_offsets, cur_vao->vertex_attrib_state, p->attr_map, p->attr_num))
		is_packed = GL_FALSE;
	// Gathering real attribute data pointers
	if (is_packed) {
		if (target_vbo) {
			ptrs[0] = (void *)target_vbo->ptr + first * streams[0].stride;
			target_vbo->last_frame = vgl_framecount;
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_vbo_attrib();
			}
		} else {
#ifdef SAFER_DRAW_SPEEDHACK
			if (count * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
				ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride;
			} else
#endif
			{
				ptrs[0] = gpu_alloc_mapped_temp(count * streams[0].stride);
				vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride, count * streams[0].stride);
			}
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_attrib();
			}
		}
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			attributes[i].regIndex = p->attr[attr_idx].regIndex;
			handle_unpacked_attrib(first, count);
		}
	}
#endif
#else // DRAW_SPEEDHACK
	handle_speedhack_attrib();
#endif

#ifndef INDICES_SPEEDHACK
	if (instanced) { // Instanced draw
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			streams[i].indexSource = (cur_vao->vertex_attrib_divisor & (1 << attr_idx)) ? SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
		}		
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			streams[i].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
		}
	}
#endif

	// Uploading new vertex program
	if (patch_vertex_program(p->vshader->id, attributes, p->attr_num, streams, p->attr_num, &p->vprog) && vgl_robust_scenes)
		return GL_FALSE; // 1.2: a layout that could not be made is not drawn with the previous one's program
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	vertex_layout_cache_insert(p, VERTEX_LAYOUT_ARRAYS, layout_variant, is_packed, GL_FALSE, attributes, streams);
#endif
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();

	// Uploading vertex streams
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		GLboolean is_active = (cur_vao->vertex_attrib_state & (1 << attr_idx)) ? GL_TRUE : GL_FALSE;
		if (is_active) {
#ifdef DRAW_SPEEDHACK
			sceGxmSetVertexStream(gxm_context, i, ptrs[i]);
#else
#ifdef STRICT_DRAW_COMPLIANCE
			sceGxmSetVertexStream(gxm_context, i, is_packed[i] ? ptrs[0] : ptrs[i]);
#else
			sceGxmSetVertexStream(gxm_context, i, is_packed ? ptrs[0] : ptrs[i]);
#endif
#endif
		} else {
			sceGxmSetVertexStream(gxm_context, i, cur_vao->vertex_attrib_value[attr_idx]);
		}
		if (!p->has_unaligned_attrs) {
			attributes[i].regIndex = i;
			if (!is_active) {
				streams[i].stride = orig_stride[i];
				attributes[i].componentCount = orig_size[i];
				attributes[i].format = orig_fmt[i];
			}
		}
	}
#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
	return GL_TRUE;
}

// One hardware stream for an interleaved layout (STR-076). With DRAW_SPEEDHACK every attribute of a draw gets a stream of its own, all of them pointing into
// the same interleaved vertex buffer. vglSetOneStream(GL_TRUE) makes glDrawElements describe such a draw with ONE stream (the lowest attribute offset
// is the stream's base, the others are attribute offsets inside the vertex) plus one stream per attribute that is disabled (a constant), so the vertex
// program has fewer streams and the draw sets fewer of them. Draws that do not qualify are made the old way. Off by default.
static GLboolean vgl_one_stream = GL_FALSE;
void vglSetOneStream(GLboolean enable) {
	vgl_one_stream = enable;
}
#ifdef DRAW_SPEEDHACK
// Qualifies and rewrites the draw's attributes / streams (the arrays the speedhack macro filled) into out_attr / out_streams.
// Returns the number of streams (0: does not qualify, nothing was written). *ptr0 is the pointer of stream 0.
static uint32_t vgl_collapse_streams(const program *p, const SceGxmVertexAttribute *attributes, const SceGxmVertexStream *streams,
	SceGxmVertexAttribute *out_attr, SceGxmVertexStream *out_streams, void **ptr0) {
	vbo *common = NULL;
	uint32_t stride = 0, base = 0xFFFFFFFFu, active = 0;
	SceGxmIndexSource index_source = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	for (uint32_t i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		if (!(cur_vao->vertex_attrib_state & (1 << attr_idx)))
			continue;
		vbo *v = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
		if (!v || (common && v != common) || streams[i].stride == 0 || (stride && streams[i].stride != stride))
			return 0;
		if (active && streams[i].indexSource != index_source)
			return 0;
		index_source = streams[i].indexSource;
		common = v;
		stride = streams[i].stride;
		uint32_t off = cur_vao->vertex_attrib_offsets[attr_idx];
		if (off < base)
			base = off;
		active++;
	}
	if (active < 2)
		return 0;
	uint32_t next = 1;
	GLboolean first = GL_TRUE;
	for (uint32_t i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		out_attr[i] = attributes[i];
		if (cur_vao->vertex_attrib_state & (1 << attr_idx)) {
			uint32_t rel = cur_vao->vertex_attrib_offsets[attr_idx] - base;
			if (rel >= stride)
				return 0;
			out_attr[i].streamIndex = 0;
			out_attr[i].offset = (uint16_t)rel;
			if (first) {
				out_streams[0] = streams[i];
				first = GL_FALSE;
			}
		} else {
			out_attr[i].streamIndex = next;
			out_streams[next++] = streams[i];
		}
	}
	*ptr0 = (uint8_t *)common->ptr + base;
	common->last_frame = vgl_framecount;
	return next;
}
#endif

// vglDrawElementsDirect (from the Galaxy port; 1.3, MSC_VGX_GXM_DIRECT): every enabled attribute of the draw comes from one interleaved buffer, so one vertex
// stream carries them all; each attribute the layout does not supply reads its constant (glVertexAttrib*, as on the ordinary path) from a stream of stride 0.
// This is the program shape vgl_collapse_streams (MSC_VGX_ONESTREAM) arrives at per draw, so both paths share one patched program in the vertex cache; what this
// path leaves out is the per-attribute walk and the cache's hash: the patched program is kept per (program, caller's key), and the VAO's arrays are not read.
static GLboolean direct_vertex_setup(program *p, SceGxmIndexSource index_type) {
	const vglDirectLayout *L = direct_layout;
	SceGxmVertexProgram *vp = NULL;
	for (int k = 0; k < VGL_DIRECT_VPROGS; k++) {
		if (p->direct_vprog[k] && p->direct_key[k] == L->key) {
			vp = p->direct_vprog[k];
			break;
		}
	}
	if (!vp) {
		SceGxmVertexAttribute attrs[VERTEX_ATTRIBS_NUM];
		SceGxmVertexStream strs[VERTEX_ATTRIBS_NUM];
		uint32_t nstreams = 1;
		strs[0].stride = L->stride;
		strs[0].indexSource = index_type & 1;
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			attrs[i].regIndex = p->attr[attr_idx].regIndex;
			if (L->enabled & (1u << attr_idx)) {
				attrs[i].streamIndex = 0;
				attrs[i].offset = L->offset[attr_idx];
				attrs[i].componentCount = L->size[attr_idx];
				attrs[i].format = L->type[attr_idx] == VGL_DIRECT_F32 ? SCE_GXM_ATTRIBUTE_FORMAT_F32 : (L->type[attr_idx] == VGL_DIRECT_U8N ? SCE_GXM_ATTRIBUTE_FORMAT_U8N : SCE_GXM_ATTRIBUTE_FORMAT_U8);
			} else {
				if (nstreams == VERTEX_ATTRIBS_NUM)
					return GL_FALSE;
				attrs[i].streamIndex = nstreams;
				attrs[i].offset = 0;
				attrs[i].componentCount = cur_vao->vertex_attrib_size[attr_idx];
				attrs[i].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
				strs[nstreams].stride = 0;
				strs[nstreams].indexSource = index_type & 1;
				nstreams++;
			}
		}
		if (patch_vertex_program(p->vshader->id, attrs, p->attr_num, strs, nstreams, &vp) || !vp)
			return GL_FALSE;
		p->direct_key[p->direct_next] = L->key;
		p->direct_vprog[p->direct_next] = vp;
		p->direct_next = (p->direct_next + 1) % VGL_DIRECT_VPROGS;
	}
	p->vprog = vp;
	sceGxmSetVertexProgram(gxm_context, vp);
	upload_uniforms();
	sceGxmSetVertexStream(gxm_context, 0, (uint8_t *)direct_vbo->ptr + direct_base);
	direct_vbo->last_frame = vgl_framecount;
	uint32_t s = 1;
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		if (!(L->enabled & (1u << attr_idx)))
			sceGxmSetVertexStream(gxm_context, s++, cur_vao->vertex_attrib_value[attr_idx]);
	}
	return GL_TRUE;
}

GLboolean vglDrawElementsDirect(GLenum mode, GLsizei count, uint32_t idx_offset, GLuint vbo_name, uint32_t base, const vglDirectLayout *layout) {
	if (!cur_program || !vbo_name || !layout || mode != GL_TRIANGLES || count <= 0)
		return GL_FALSE;
	vbo *v = (vbo *)vbo_name;
	if (!v->ptr)
		return GL_FALSE;
	direct_layout = layout;
	direct_vbo = v;
	direct_base = base;
	direct_failed = GL_FALSE;
	program *fresh_prog = &progs[cur_program - 1];
	const uint8_t fresh = fresh_prog->fresh_draw;
	const uint64_t fresh_t0 = fresh ? sceKernelGetProcessTimeWide() : 0;
	glDrawElements(mode, count, GL_UNSIGNED_SHORT, (const GLvoid *)idx_offset);
	if (fresh) {
		fresh_prog->fresh_draw = 0;
		vgl_bt_note(2, fresh_t0); // 0040: the first draw since the link
	}
	direct_layout = NULL;
	direct_vbo = NULL;
	return !direct_failed;
}

GLboolean _glDrawElements_CustomShadersIMPL(uint16_t *idx_buf, GLsizei count, uint32_t top_idx, uint32_t base_idx, SceGxmIndexSource index_type) {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];

	// GLB: stream buffers in the cached arena are written back before the first draw that can read them
	VGL_STREAM_FLUSH_IF_NEEDED()

	// Check if a blend info rebuild is required and upload fragment program
	setup_frag_program();

	// Uploading fragment textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->frag_texunits[i]->sampler_index];
			uint8_t tex_type = p->frag_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifdef HAVE_TEX_CACHE
			restore_tex_cache(tex);
#endif
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->frag_texunits[i]->sampler_index];
			if (smp) {
				vglSetTexMinFilter(&tex->gxm_tex, smp->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, smp->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, smp->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, smp->u_mode);
				vglSetTexVMode(&tex->gxm_tex, smp->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, smp->use_mips ? tex->mip_count : 0);
				vglSetTexLodBias(&tex->gxm_tex, smp->lod_bias);
				tex->overridden = GL_TRUE;
			} else if (tex->overridden) {
				vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
				vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 0);
				vglSetTexLodBias(&tex->gxm_tex, tex->lod_bias);
				tex->overridden = GL_FALSE;
			}
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				p->unif_changed[1] = 1;
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading vertex textures on relative texture units
	for (int i = 0; i < p->max_vert_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->vert_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->vert_texunits[i]->sampler_index];
			uint8_t tex_type = p->vert_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->vert_texunits[i]->sampler_index];
			if (smp) {
				vglSetTexMinFilter(&tex->gxm_tex, smp->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, smp->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, smp->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, smp->u_mode);
				vglSetTexVMode(&tex->gxm_tex, smp->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, smp->use_mips ? tex->mip_count : 0);
				tex->overridden = GL_TRUE;
			} else if (tex->overridden) {
				vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
				vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
				vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
				vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
				vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
				vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 0);
				tex->overridden = GL_FALSE;
			}
			sceGxmSetVertexTexture(gxm_context, i, &tex->gxm_tex);
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// vglDrawElementsDirect: before the layout cache, which would read the VAO's arrays
	if (direct_layout) {
		if (direct_vertex_setup(p, index_type))
			return GL_TRUE;
		direct_failed = GL_TRUE;
		return GL_FALSE;
	}

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	vertex_layout_cache_entry *layout_entry = vertex_layout_lookup_simple(p, VERTEX_LAYOUT_ELEMENTS);
	if (layout_entry) {
		if (!layout_entry->is_full_vbo && !top_idx) {
			if ((index_type & 1) == 0) {
				for (int i = 0; i < count; i++) {
					if (idx_buf[i] > top_idx)
						top_idx = idx_buf[i];
				}
			} else {
				uint32_t *_idx_buf = (uint32_t *)idx_buf;
				for (int i = 0; i < count; i++) {
					if (_idx_buf[i] > top_idx)
						top_idx = _idx_buf[i];
				}
			}
			top_idx += base_idx + 1;
		}
#ifndef INDICES_SPEEDHACK
		if (top_idx && top_idx < 0xFFFF)
			index_type &= ~1;
#endif
		if (layout_entry->variant == (uint8_t)index_type) {
#ifdef HAVE_PROFILING
			vgl_vcache_stats.layout_hits++;
#endif
			void *ptrs[VERTEX_ATTRIBS_NUM];
			vertex_layout_setup_ptrs(p, layout_entry->is_packed, 0, top_idx, top_idx, ptrs);
			p->vprog = layout_entry->vprog;
			sceGxmSetVertexProgram(gxm_context, p->vprog);
			upload_uniforms();
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				if (cur_vao->vertex_attrib_state & (1 << attr_idx)) {
					if (layout_entry->is_packed)
						sceGxmSetVertexStream(gxm_context, i, ptrs[0]);
					else
						sceGxmSetVertexStream(gxm_context, i, ptrs[i]);
				} else {
					sceGxmSetVertexStream(gxm_context, i, cur_vao->vertex_attrib_value[attr_idx]);
				}
			}
#ifdef HAVE_PROFILING
			shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
			shaders_draw_cnt++;
#endif
			return GL_TRUE;
		}
	}
#ifdef HAVE_PROFILING
	vgl_vcache_stats.layout_misses++;
#endif
#endif

	// Aligning attributes
	SceGxmVertexAttribute *attributes;
	SceGxmVertexStream *streams;
	align_attributes(attributes, streams);

	void *ptrs[VERTEX_ATTRIBS_NUM];
#ifndef DRAW_SPEEDHACK
	vbo *target_vbo = (vbo *)cur_vao->vertex_attrib_vbo[p->attr_map[0]];
	GLboolean is_full_vbo = GL_TRUE;
#ifdef STRICT_DRAW_COMPLIANCE
	GLboolean is_packed[VERTEX_ATTRIBS_NUM];
	vgl_memset(is_packed, GL_TRUE, p->attr_num * sizeof(GLboolean));
#else
	GLboolean is_packed = p->attr_num > 1;
	if (is_packed) {
#endif
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			vbo *attr_vbo = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
			if (attr_vbo) {
#ifdef STRICT_DRAW_COMPLIANCE
					vgl_memset(is_packed, 0, p->attr_num * sizeof(GLboolean));
					break;
#else
				if (attr_vbo != target_vbo) {
					is_packed = GL_FALSE;
					break;
				}
#endif
			} else {
#ifdef STRICT_DRAW_COMPLIANCE
				if (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[attr_idx] && cur_vao->vertex_attrib_offsets[attr_idx] >= cur_vao->vertex_attrib_offsets[p->attr_map[0]])) {
					is_packed[attr_idx] = GL_FALSE;
				}
#endif
				is_full_vbo = GL_FALSE;
			}
		}
#ifndef STRICT_DRAW_COMPLIANCE
		if (is_packed && (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[p->attr_map[1]] && cur_vao->vertex_attrib_offsets[p->attr_map[1]] > cur_vao->vertex_attrib_offsets[p->attr_map[0]])))
			is_packed = GL_FALSE;
	} else if (!target_vbo)
		is_full_vbo = GL_FALSE;
#endif

	// Detecting highest index value
	if (!is_full_vbo && !top_idx) {
		if ((index_type & 1) == 0)
		{
			for (int i = 0; i < count; i++) {
				if (idx_buf[i] > top_idx) {
					top_idx = idx_buf[i];
				}
			}
		}
		else
		{
			uint32_t *_idx_buf = (uint32_t *)idx_buf;
			for (int i = 0; i < count; i++) {
				if (_idx_buf[i] > top_idx) {
					top_idx = _idx_buf[i];
				}
			}
		}
		top_idx += base_idx + 1;
	}

#ifdef STRICT_DRAW_COMPLIANCE
	// Gathering real attribute data pointers
	if (is_packed[0]) {
#ifdef SAFER_DRAW_SPEEDHACK
		if (top_idx * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
			ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]];
		} else
#endif
		{
			ptrs[0] = gpu_alloc_mapped_temp(top_idx * streams[0].stride);
			vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]], top_idx * streams[0].stride);
		}
	}
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		attributes[i].regIndex = p->attr[attr_idx].regIndex;
		if (is_packed[i]) {
			handle_packed_attrib();
		} else {
			handle_unpacked_attrib(0, top_idx);
		}
	}
#else
	// 0059: the packed-VBO attribute offset is 16 bits (SceGxmVertexAttribute.offset): an attribute further into the buffer than 64 KiB is read from the wrong place
	if (is_packed && target_vbo && !vgl_pack_offsets_fit(cur_vao->vertex_attrib_offsets, cur_vao->vertex_attrib_state, p->attr_map, p->attr_num))
		is_packed = GL_FALSE;
	// Gathering real attribute data pointers
	if (is_packed) {
		if (target_vbo) {
			ptrs[0] = (void *)target_vbo->ptr;
			target_vbo->last_frame = vgl_framecount;
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_vbo_attrib();
			}
		} else {
#ifdef SAFER_DRAW_SPEEDHACK
			if (top_idx * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
				ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]];
			} else
#endif
			{
				ptrs[0] = gpu_alloc_mapped_temp(top_idx * streams[0].stride);
				vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]], top_idx * streams[0].stride);
			}
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_attrib();
			}
		}	
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			attributes[i].regIndex = p->attr[attr_idx].regIndex;
			handle_unpacked_attrib(0, top_idx);
		}
	}
#endif
#else // DRAW_SPEEDHACK
	handle_speedhack_attrib();
#endif
	SceGxmVertexAttribute *attributes_orig = attributes; // (what the draw changed in the VAO's own arrays is put back through these)
	SceGxmVertexStream *streams_orig = streams;
	uint32_t stream_num = p->attr_num;
#ifdef DRAW_SPEEDHACK
	SceGxmVertexAttribute one_attr[VERTEX_ATTRIBS_NUM];
	SceGxmVertexStream one_streams[VERTEX_ATTRIBS_NUM];
	void *one_ptr = NULL;
	uint32_t collapsed = 0;
#endif

#ifndef INDICES_SPEEDHACK
	// Check if highest index is small enough for 16 bit usage and if so, downgrade to 16 bit vertex sources for faster emitted code
	if (top_idx && top_idx < 0xFFFF) {
		index_type &= ~1;
	}

	if (index_type & 2) { // Instanced draw
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			streams[i].indexSource = (cur_vao->vertex_attrib_divisor & (1 << attr_idx)) ? index_type : (index_type & 1);
		}		
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			streams[i].indexSource = index_type;
		}
	}
#endif

#ifdef DRAW_SPEEDHACK
	if (vgl_one_stream && p->attr_num > 1 && !(index_type & 2)) {
		collapsed = vgl_collapse_streams(p, attributes, streams, one_attr, one_streams, &one_ptr);
		if (collapsed) {
			attributes = one_attr;
			streams = one_streams;
			stream_num = collapsed;
		}
	}
#endif

	// Uploading new vertex program
	if (patch_vertex_program(p->vshader->id, attributes, p->attr_num, streams, stream_num, &p->vprog) && vgl_robust_scenes)
		return GL_FALSE; // 1.2: a layout that could not be made is not drawn with the previous one's program
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	vertex_layout_cache_insert(p, VERTEX_LAYOUT_ELEMENTS, (uint8_t)index_type, is_packed, is_full_vbo, attributes, streams);
#endif
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();

	// Uploading vertex streams
#ifdef DRAW_SPEEDHACK
	if (collapsed) {
		sceGxmSetVertexStream(gxm_context, 0, one_ptr);
		uint32_t next = 1;
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			if (!(cur_vao->vertex_attrib_state & (1 << attr_idx)))
				sceGxmSetVertexStream(gxm_context, next++, cur_vao->vertex_attrib_value[attr_idx]);
		}
	}
#endif
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		GLboolean is_active = (cur_vao->vertex_attrib_state & (1 << attr_idx)) ? GL_TRUE : GL_FALSE;
		if (is_active) {
#ifdef DRAW_SPEEDHACK
			if (!collapsed)
				sceGxmSetVertexStream(gxm_context, i, ptrs[i]);
#else
#ifdef STRICT_DRAW_COMPLIANCE
			sceGxmSetVertexStream(gxm_context, i, is_packed[i] ? ptrs[0] : ptrs[i]);
#else
			sceGxmSetVertexStream(gxm_context, i, is_packed ? ptrs[0] : ptrs[i]);
#endif
#endif
		} else {
#ifdef DRAW_SPEEDHACK
			if (!collapsed)
#endif
				sceGxmSetVertexStream(gxm_context, i, cur_vao->vertex_attrib_value[attr_idx]);
		}
		if (!p->has_unaligned_attrs) {
			attributes_orig[i].regIndex = i;
			if (!is_active) {
				streams_orig[i].stride = orig_stride[i];
				attributes_orig[i].componentCount = orig_size[i];
				attributes_orig[i].format = orig_fmt[i];
			}
		}
	}
#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
	return GL_TRUE;
}

#ifdef ENABLE_LEGACY_PIPELINE
void _vglDrawObjects_CustomShadersIMPL() {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];
	VGL_STREAM_FLUSH_IF_NEEDED() // GLB

	// Check if a blend info rebuild is required
	setup_frag_program_void();

	// Setting up required vertex shader
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture *tex = &texture_slots[texture_units[i].tex_id[0]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				p->unif_changed[1] = 1;
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();

#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
}
#endif

#ifdef HAVE_SHARK_LOG
void shark_log_cb(const char *msg, shark_log_level msg_level, int line) {
	char newline[1024];
	GLboolean is_extra_line = shark_log ? GL_TRUE : GL_FALSE;
	switch (msg_level) {
	case SHARK_LOG_INFO:
		sprintf(newline, "%sI] %s on line %d.", is_extra_line ? "\n" : "", msg, line);
#ifdef LOG_ERRORS
		vgl_log("Shader Compiler: I] %s on line %d.\n", msg, line);
#endif
		break;
	case SHARK_LOG_WARNING:
		sprintf(newline, "%sW] %s on line %d.", is_extra_line ? "\n" : "", msg, line);
#ifdef LOG_ERRORS
		vgl_log("Shader Compiler: W] %s on line %d.\n", msg, line);
#endif
		break;
	case SHARK_LOG_ERROR:
		sprintf(newline, "%sE] %s on line %d.", is_extra_line ? "\n" : "", msg, line);
#ifdef LOG_ERRORS
		vgl_log("Shader Compiler: E] %s on line %d.\n", msg, line);
#endif
		break;
	}
	uint32_t size = (is_extra_line ? strlen(shark_log) : 0) + strlen(newline);
	shark_log = shark_log ? vglRealloc(shark_log, size + 1) : vglMalloc(size + 1);
	if (is_extra_line)
		strcat(shark_log, newline);
	else
		strcpy(shark_log, newline);
}
#elif defined(LOG_ERRORS)
void shark_log_cb(const char *msg, shark_log_level msg_level, int line) {
	switch (msg_level) {
	case SHARK_LOG_INFO:
		vgl_log("Shader Compiler: I] %s on line %d.\n", msg, line);
		break;
	case SHARK_LOG_WARNING:
		vgl_log("Shader Compiler: W] %s on line %d.\n", msg, line);
		break;
	case SHARK_LOG_ERROR:
		vgl_log("Shader Compiler: E] %s on line %d.\n", msg, line);
		break;
	}
}
#endif

static inline __attribute__((always_inline)) float *get_uniform_alias_data_ptr(uniform *src, uint32_t cnt, const char *name) {
	for (int i = 0; i < cnt; i++) {
		uniform *u = &src[i];
		if (!strcmp(name, sceGxmProgramParameterGetName(u->ptr))) {
			return u->fptr;
		}
	}
	return NULL;
}

static inline __attribute__((always_inline)) ubo *has_block_alias(ubo *u, const char *name) {
	while (u) {
		if (!strcmp(name, ((block_uniform*)u->ptr)->name)) {
			return u;
		}
		u = u->chain;
	}
	return NULL;
}

static inline __attribute__((always_inline)) block_uniform *get_block_details(block_uniform *b, uint8_t idx) {
	while (b) {
		if (b->idx == idx)
			return b;
		b = (block_uniform *)b->chain;
	}
	return NULL;
}

/*
 * ------------------------------
 * - IMPLEMENTATION STARTS HERE -
 * ------------------------------
 */
void vglSetupRuntimeShaderCompiler(shark_opt opt_level, int32_t use_fastmath, int32_t use_fastprecision, int32_t use_fastint) {
	compiler_opts = opt_level;
	compiler_fastmath = use_fastmath;
	compiler_fastprecision = use_fastprecision;
	compiler_fastint = use_fastint;
}

GLuint glCreateShader(GLenum shaderType) {
#ifndef SKIP_ERROR_HANDLING
	if (shaderType != GL_FRAGMENT_SHADER && shaderType != GL_VERTEX_SHADER && shaderType != GL_CG_FRAGMENT_SHADER_EXT && shaderType != GL_CG_VERTEX_SHADER_EXT) {
		SET_GL_ERROR_WITH_RET_AND_VALUE(GL_INVALID_ENUM, 0, shaderType)
	}
#endif

	// Looking for a free shader slot
	GLuint res = MAX_CUSTOM_SHADERS;
	for (GLuint i = 0; i < MAX_CUSTOM_SHADERS; i++) {
		if (!shaders[i]) {
			res = i;
			break;
		}
	}

	// All shader slots are busy, exiting call
	if (res == MAX_CUSTOM_SHADERS) {
		vgl_log("%s:%d %s: Out of shaders handles. Consider increasing MAX_CUSTOM_SHADERS...\n", __FILE__, __LINE__, __func__);
		return 0;
	}

	shaders[res] = vglCalloc(1, sizeof(shader));
	shader *s = shaders[res];
	if (!s)
		return 0;
	s->slot = res;

	// Reserving and initializing shader slot
	switch (shaderType) {
	case GL_CG_VERTEX_SHADER_EXT:
		s->type = GL_VERTEX_SHADER;
		break;
	case GL_CG_FRAGMENT_SHADER_EXT:
		s->type = GL_FRAGMENT_SHADER;
		break;
	default:
		s->type = shaderType;
		s->is_glsl = GL_TRUE;
		break;
	}

	return res + 1;
}

void glGetShaderiv(GLuint handle, GLenum pname, GLint *params) {
	THREAD_SAFE()

	// Grabbing passed shader
	shader *s = shaders[handle - 1];
	switch (pname) {
	case GL_SHADER_TYPE:
		*params = s->type;
		break;
	case GL_COMPILE_STATUS:
		if (glsl_sema_mode == VGL_MODE_POSTPONED && s->is_glsl) {
			*params = s->prog || s->source ? GL_TRUE : GL_FALSE;
			break;
		}
		*params = s->prog ? GL_TRUE : GL_FALSE;
		break;
	case GL_DELETE_STATUS:
		*params = s->dirty ? GL_TRUE : GL_FALSE;
		break;
	case GL_INFO_LOG_LENGTH:
#ifdef HAVE_SHARK_LOG
		*params = s->log ? (strlen(s->log) + 1) : 0;
#else
		*params = 0;
#endif
		break;
	case GL_SHADER_SOURCE_LENGTH:
		*params = s->source ? (strlen(s->source) + 1) : 0;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glGetShaderInfoLog(GLuint handle, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (maxLength < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	GLsizei len = 0;
#ifdef HAVE_SHARK_LOG
	shader *s = shaders[handle - 1];
	if (s->log) {
		len = min(strlen(s->log), maxLength - 1);
		vgl_fast_memcpy(infoLog, s->log, len);
		infoLog[len] = 0;
	}
#endif
	if (length)
		*length = len;
}

void glGetShaderSource(GLuint handle, GLsizei bufSize, GLsizei *length, GLchar *source) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed shader
	shader *s = shaders[handle - 1];

	GLsizei size = 0;
	if (s->source) {
		GLsizei src_len = strlen(s->source);
		if (bufSize <= src_len)
			src_len = bufSize - 1;
		strncpy(source, s->source, src_len);
		size = src_len;
	}
	if (length)
		*length = size;
}

void glShaderSource(GLuint handle, GLsizei count, const GLchar *const *string, const GLint *length) {
	THREAD_SAFE()

	if (!handle || handle > MAX_CUSTOM_SHADERS || !shaders[handle - 1] || count < 0 || count > 512 || (count && !string)) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
	// Grabbing passed shader
	shader *s = shaders[handle - 1];
	
	uint32_t size = 1;
	size_t lengths[512];

	for (int i = 0; i < count; i++) {
		if (!string[i]) {
			SET_GL_ERROR(GL_INVALID_VALUE)
		}
		if (length && length[i] >= 0) {
			lengths[i] = length[i];
		} else {
			lengths[i] = strlen(string[i]);
			
		}
		if (lengths[i] > UINT32_MAX - size) {
			SET_GL_ERROR(GL_OUT_OF_MEMORY)
		}
		size += lengths[i];
	}

	char *source = (char *)vglMalloc(size);
	if (!source) {
		SET_GL_ERROR(GL_OUT_OF_MEMORY)
	}
	size = 0;
	for (int i = 0; i < count; i++) {
		vgl_fast_memcpy(&source[size], string[i], lengths[i]);
		size += lengths[i];
	}

	source[size] = 0;
	vgl_free(s->source);
	s->source = source;
	s->size = size;
}

void glShaderBinary(GLsizei count, const GLuint *handles, GLenum binaryFormat, const void *binary, GLsizei length) {
	THREAD_SAFE()
	if (count < 1 || !handles || !handles[0] || handles[0] > MAX_CUSTOM_SHADERS || !shaders[handles[0] - 1] || length < 0)
		return;

	// Grabbing passed shader
	shader *s = shaders[handles[0] - 1];

	unserialize_shader((void *)binary, length, s, GL_FALSE);
}

void glCompileShader(GLuint handle) {
	THREAD_SAFE()
	if (!handle || handle > MAX_CUSTOM_SHADERS || !shaders[handle - 1])
		return;
	shader *s = shaders[handle - 1];
	if (s->prog || !s->source)
		return;

	// If vitaShaRK is not enabled, we try to initialize it
	vgl_compiler_enter(); // 1.3 ASYNC_SHADERS (0027): a worker may be starting or using the compiler
	if (!is_shark_online && !start_shader_compiler()) {
		vgl_compiler_leave();
		SET_GL_ERROR(GL_INVALID_OPERATION)
	}
	vgl_compiler_leave();
	
	// If we use VGL_MODE_POSTPONED, we compile shaders in glLinkProgram
	if (s->is_glsl && glsl_sema_mode == VGL_MODE_POSTPONED)
		return;
	
#ifdef HAVE_SHADER_CACHE
	char fname[256];
	uint64_t hash = XXH3_64bits(s->source, s->size);
	uint8_t subdir_hash = hash >> 56;
	int fname_len = sprintf(fname, "%s/%s/%02X/%016llX.gxp", vgl_shader_cache_path, s->type == GL_VERTEX_SHADER ? "v" : "f", subdir_hash, hash);
	if (vgl_shader_pack_lookup) {
		uint32_t psz = 0;
		const void *pb = vgl_shader_pack_lookup(s->type == GL_FRAGMENT_SHADER, hash, &psz);
		if (pb && unserialize_shader((void *)pb, psz, s, GL_FALSE)) {
			vgl_free(s->source);
			s->source = NULL;
			return;
		}
	}
	if (load_shader_cache(sceIoOpen(fname, SCE_O_RDONLY, 0777), s, GL_FALSE) ||
		load_shader_cache(vgl_open_seed(s->type == GL_VERTEX_SHADER ? "v" : "f", subdir_hash, hash), s, GL_FALSE)) {
		vgl_free(s->source);
		s->source = NULL;
		return;
	}
#endif

	vgl_compiler_enter(); // 1.3 ASYNC_SHADERS (0027): the translator's and the compiler's state are global
	if (s->is_glsl) {
		glsl_translator_process(s);
	}
	vgl_compile_shader(s, GL_FALSE);
	vgl_compiler_leave();
}

void glDeleteShader(GLuint shad) {
	THREAD_SAFE()

	// Grabbing passed shader
	shader *s = shaders[shad - 1];

	// If the shader is attached to any program, we only mark it for deletion
	if (s->ref_counter > 0) {
		s->dirty = GL_TRUE;
	} else {
		release_shader(s);
	}
}

void glAttachShader(GLuint prog, GLuint shad) {
	THREAD_SAFE()

	// Grabbing passed shader and program
	shader *s = shaders[shad - 1];
	program *p = &progs[prog - 1];
	
	// Attaching shader to desired program
	if (p->status == PROG_UNLINKED) {
		switch (s->type) {
		case GL_VERTEX_SHADER:
			s->ref_counter++;
			if (p->vshader) {
				p->vshader->ref_counter--;
				if (p->vshader->dirty && p->vshader->ref_counter == 0) {
					release_shader(p->vshader);
				}
			}
			p->vshader = s;
			// If we use VGL_MODE_POSTPONED, we perform attributes binding in glLinkProgram
			if (s->prog && (glsl_sema_mode != VGL_MODE_POSTPONED || !s->is_glsl)) {
				// Setting progressive default attribute bindings
				set_default_attrib_binding();	
			}
			break;
		case GL_FRAGMENT_SHADER:
			s->ref_counter++;
			if (p->fshader) {
				p->fshader->ref_counter--;
				if (p->fshader->dirty && p->fshader->ref_counter == 0) {
					release_shader(p->fshader);
				}
			}
			p->fshader = s;
			break;
		default:
			break;
		}
	} else {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
}

void glGetAttachedShaders(GLuint prog, GLsizei maxCount, GLsizei *count, GLuint *shads) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];

#ifndef SKIP_ERROR_HANDLING
	if (maxCount < 0) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, maxCount)
	} else if (maxCount == 0) {
		return;
	}
#endif

	// Returning attached shaders
	GLuint shad = 1;
	*count = 0;
	if (p->vshader) {
		shads[0] = p->vshader->slot + 1;
		*count = 1;
	}
#ifndef SKIP_ERROR_HANDLING
	if (*count >= maxCount) {
		return;
	}
#endif
	if (p->fshader) {
		shads[*count] = p->fshader->slot + 1;
		*count = *count + 1;
	}
}

// GLB: how many entries of the program and shader tables are in use (vglGlbGetStats).
void vgl_glb_count_tables(uint32_t *progs_live, uint32_t *shaders_live, uint32_t *progs_max, uint32_t *shaders_max) {
	uint32_t p = 0, s = 0;
	for (int i = 0; i < MAX_CUSTOM_PROGRAMS; i++)
		if (progs[i].status)
			p++;
	for (int i = 0; i < MAX_CUSTOM_SHADERS; i++)
		if (shaders[i])
			s++;
	*progs_live = p;
	*shaders_live = s;
	*progs_max = MAX_CUSTOM_PROGRAMS;
	*shaders_max = MAX_CUSTOM_SHADERS;
}

GLuint glCreateProgram(void) {
	// Looking for a free program slot
	GLuint i, j, res = 0xFFFFFFFF;
	for (i = 1; i <= MAX_CUSTOM_PROGRAMS; i++) {
		// Program slot found, reserving and initializing it
		if (!(progs[i - 1].status)) {
			res = i--;
			progs[i].status = PROG_UNLINKED;
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
			progs[i].vertex_layout_version = ++vertex_program_layout_current_ver;
#endif
			progs[i].attr_num = 0;
			vgl_memset(progs[i].direct_key, 0, sizeof(progs[i].direct_key));
			vgl_memset(progs[i].direct_vprog, 0, sizeof(progs[i].direct_vprog));
			progs[i].direct_next = 0;
			program_caches_reset(&progs[i]);
#ifdef ENABLE_LEGACY_PIPELINE
			progs[i].attr_mode = VGL_ATTRIB_REGULAR;
#endif
			progs[i].attr_idx = 0;
			progs[i].max_frag_texunit_idx = 0;
			progs[i].max_vert_texunit_idx = 0;
#ifdef HAVE_FFP_SHADER_SUPPORT
			for (int j = 0; j < FFP_BINDS_NUM; j++) {
				progs[i].ffp_binds[j] = NULL;
			}
#endif
			progs[i].vshader = NULL;
			progs[i].fshader = NULL;
			progs[i].vert_uniforms = NULL;
			progs[i].frag_uniforms = NULL;
			progs[i].unif_vbuffer = NULL;
			progs[i].unif_fbuffer = NULL;
			progs[i].vert_uniforms_num = 0;
			progs[i].frag_uniforms_num = 0;
			progs[i].vert_ubos = NULL;
			progs[i].frag_ubos = NULL;
			progs[i].attr_highest_idx = 0;
			progs[i].num_glsl_attr = 0;
			progs[i].glsl_attr_map = NULL;
			progs[i].is_fbo_float = 0xFF;
			for (j = 0; j < VERTEX_ATTRIBS_NUM; j++) {
				progs[i].attr[j].regIndex = 0xDEAD;
			}
			break;
		}
	}
#ifndef SKIP_ERROR_HANDLING
	if (res == 0xFFFFFFFF) {
		vgl_log("%s:%d %s: Out of programs handles. Consider increasing MAX_CUSTOM_PROGRAMS...\n", __FILE__, __LINE__, __func__);
		return 0;
	}
#endif
	return res;
}

GLboolean glIsProgram(GLuint i) {
	if (progs[i - 1].status != PROG_INVALID)
		return GL_TRUE;
	return GL_FALSE;
}

void glGetProgramBinary(GLuint prog, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed program
	program *p = &progs[prog - 1];

	// Saving info related to bound attributes locations
	GLuint *b = (GLuint *)binary;
	b[0] = p->attr_highest_idx;
	vgl_fast_memcpy(&b[1], p->attr, sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM);
	GLsizei size = sizeof(GLuint) + sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM;
	uint8_t *buf = (uint8_t *)binary + size;

	// Dumping vertex binary
	serialize_shader(&buf[sizeof(uint32_t)], (size_t *)buf, p->vshader, GL_FALSE);

	// Dumping fragment binary
	buf += sizeof(uint32_t) + *(uint32_t *)buf;
	serialize_shader(&buf[sizeof(uint32_t)], (size_t *)buf, p->fshader, GL_FALSE);

	if (length)
		*length = ((uintptr_t)buf - (uintptr_t)binary) + sizeof(uint32_t) + *(uint32_t *)buf;
}

void glProgramBinary(GLuint prog, GLenum binaryFormat, const void *binary, GLsizei length) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];
	
	// Restoring bound attributes info
	GLuint *b = (GLuint *)binary;
	p->attr_highest_idx = b[0];
	vgl_fast_memcpy(p->attr, &b[1], sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM);
	GLsizei size = sizeof(GLuint) + sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM;

	// Restoring shaders
	GLuint vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
	GLuint fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
	uint32_t *sizeptr = (uint32_t *)((uint8_t *)binary + size);
	glShaderBinary(1, &vs, 0, &sizeptr[1], sizeptr[0]);
	sizeptr = (uint32_t *)((uint8_t *)binary + size + sizeptr[0] + sizeof(uint32_t));
	glShaderBinary(1, &fs, 0, &sizeptr[1], sizeptr[0]);
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);

	// Linking program and marking for deletion temporary shaders
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
}

void glDeleteProgram(GLuint prog) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];

	// Releasing both vertex and fragment programs from sceGxmShaderPatcher
	if (p->status) {
		sceGxmFinish(gxm_context);
		if (p->vert_uniforms) {
			vgl_free(p->vert_uniforms);
		}
		if (p->unif_vbuffer) {
			vgl_free(p->unif_vbuffer);
		}
		if (p->frag_uniforms) {
			vgl_free(p->frag_uniforms);
		}
		if (p->unif_fbuffer) {
			vgl_free(p->unif_fbuffer);
		}
		uni_hist_free(p);
		while (p->vert_ubos) {
			ubo *old = p->vert_ubos;
			p->vert_ubos = (ubo *)p->vert_ubos->chain;
			vgl_free(old);
		}
		while (p->frag_ubos) {
			ubo *old = p->frag_ubos;
			p->frag_ubos = (ubo *)p->frag_ubos->chain;
			vgl_free(old);
		}
		if (p->glsl_attr_map) {
			vgl_free(p->glsl_attr_map);
		}

		// Checking if attached shaders are marked for deletion and should be deleted
		if (p->vshader) {
			p->vshader->ref_counter--;
			if (p->vshader->dirty && p->vshader->ref_counter == 0) {
				release_shader(p->vshader);
			}
		}
		if (p->fshader) {
			p->fshader->ref_counter--;
			if (p->fshader->dirty && p->fshader->ref_counter == 0) {
				release_shader(p->fshader);
			}
		}
	}
	p->status = PROG_INVALID;
	// 1.2 (upstream 9bf3e3f): see glLinkProgram. A program id that is deleted and made again is the same id to glUseProgram's early return.
	if (prog == cur_program)
		vgl_dirty_all_uniforms();
}

void glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
	THREAD_SAFE()

	if (length)
		*length = 0;
}

void glGetProgramiv(GLuint progr, GLenum pname, GLint *params) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[progr - 1];
	int i, cnt;
	matrix_uniform *m;
	uint32_t *ptr;
	uint32_t dummy;

	switch (pname) {
	case GL_LINK_STATUS:
	case GL_VALIDATE_STATUS:
		*params = p->status == PROG_LINKED;
		break;
	case GL_INFO_LOG_LENGTH:
		*params = 0;
		break;
	case GL_PROGRAM_BINARY_LENGTH:
		*params = serialized_shader_size(p->vshader, GL_FALSE, &dummy) + serialized_shader_size(p->fshader, GL_FALSE, &dummy) + sizeof(GLuint) * 3 + sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM;
		break;
	case GL_ATTACHED_SHADERS:
		i = 0;
		if (p->fshader)
			i++;
		if (p->vshader)
			i++;
		*params = i;
		break;
	case GL_ACTIVE_ATTRIBUTES:
		*params = p->attr_num;
		break;
	case GL_ACTIVE_UNIFORM_MAX_LENGTH:
		i = 0;
		for (int j = 0; j < p->vert_uniforms_num; j++) {
			int len = strlen(sceGxmProgramParameterGetName(p->vert_uniforms[j].ptr)) + 1;
			if (len > i)
				i = len;
		}
		for (int j = 0; j < p->frag_uniforms_num; j++) {
			int len = strlen(sceGxmProgramParameterGetName(p->frag_uniforms[j].ptr)) + 1;
			if (len > i)
				i = len;
		}
		*params = i;
		break;
	case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
		i = 0;
		cnt = sceGxmProgramGetParameterCount(p->vshader->prog);
		uint32_t *ptr = vglProgramGetParameterBase(p->vshader->prog);
		while (cnt--) {
			SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
			if (sceGxmProgramParameterGetCategory(param) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) {
				int len = strlen(sceGxmProgramParameterGetName(param)) + 1;
				if (len > i)
					i = len;
			}
			ptr += 4;
		}
		*params = i;
		break;
	case GL_ACTIVE_UNIFORMS:
		*params = p->vert_uniforms_num + p->frag_uniforms_num;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

// 1.2: SAMPLERS_SPEEDHACK (upstream) loops over the texture units of a program without asking whether a unit has a sampler. The units are indexed by the resource index
// the shader compiler gave the sampler, which has a gap when a sampler was optimised out. A gap holds this record (unit 0, nothing is sampled from it) so the loop never
// meets a null pointer. The arrays are cleared at every link: a program slot that is made again must not keep the pointers of the program that was in it.
static uniform vgl_dummy_texunit = { .type = UNIFORM_SAMPLER, .sampler_index = 0 };

static void link_program_untimed(GLuint progr) {
	THREAD_SAFE()
	if (!progr || progr > MAX_CUSTOM_PROGRAMS)
		return;

	// Grabbing passed program
	program *p = &progs[progr - 1];
	if (p->status != PROG_UNLINKED || !p->vshader || !p->fshader)
		return;
	vgl_memset(p->direct_key, 0, sizeof(p->direct_key));
	vgl_memset(p->direct_vprog, 0, sizeof(p->direct_vprog));
	p->direct_next = 0;
	program_caches_reset(p);
	vgl_memset(p->vert_texunits, 0, sizeof(p->vert_texunits));
	vgl_memset(p->frag_texunits, 0, sizeof(p->frag_texunits));
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	p->vertex_layout_version = ++vertex_program_layout_current_ver;
#endif
	if (glsl_sema_mode == VGL_MODE_POSTPONED) {
		if (!(p->fshader->prog || (p->fshader->is_glsl && p->fshader->source)) || !(p->vshader->prog || (p->vshader->is_glsl && p->vshader->source))) {
			vgl_log("%s:%d: %s: %s shader is missing.\n", __FILE__, __LINE__, __func__, (p->fshader->prog || (p->fshader->is_glsl && p->fshader->source)) ? "vertex" : "fragment");
			return;
		}
	} else {
		if (!p->fshader->prog || !p->vshader->prog) {
			vgl_log("%s:%d: %s: %s shader is missing.\n", __FILE__, __LINE__, __func__, p->fshader->prog ? "vertex" : "fragment");
			return;
		}
	}

	// With VGL_MODE_POSTPONED we perform shaders translation+compilation and attributes binding prior actual program linking
	if (glsl_sema_mode == VGL_MODE_POSTPONED && (p->vshader->is_glsl || p->fshader->is_glsl)) {
		glsl_sema_mode = VGL_MODE_SHADER_PAIR;
#ifdef HAVE_SHADER_CACHE
		char frag_fname[256], vert_fname[256];
		int fname_len;
		char *fname;
		if (!p->vshader->prog) {
			uint64_t hash = XXH3_64bits(p->vshader->source, p->vshader->size);
			uint8_t subdir_hash = hash >> 56;
			fname_len = sprintf(vert_fname, "%s/v/%02X/%016llX.gxp", vgl_shader_cache_path, subdir_hash, hash);
			uint32_t psz = 0;
			const void *pb = vgl_shader_pack_lookup ? vgl_shader_pack_lookup(0, hash, &psz) : NULL;
			if (pb)
				unserialize_shader((void *)pb, psz, p->vshader, GL_TRUE);
			if (!p->vshader->prog && !load_shader_cache(sceIoOpen(vert_fname, SCE_O_RDONLY, 0777), p->vshader, GL_TRUE))
				load_shader_cache(vgl_open_seed("v", subdir_hash, hash), p->vshader, GL_TRUE);
		}
		if (!p->fshader->prog) {
			uint64_t hash = XXH3_64bits(p->fshader->source, p->fshader->size);
			uint8_t subdir_hash = hash >> 56;
			fname_len = sprintf(frag_fname, "%s/f/%02X/%016llX.gxp", vgl_shader_cache_path, subdir_hash, hash);
			uint32_t psz = 0;
			const void *pb = vgl_shader_pack_lookup ? vgl_shader_pack_lookup(1, hash, &psz) : NULL;
			if (pb)
				unserialize_shader((void *)pb, psz, p->fshader, GL_TRUE);
			if (!p->fshader->prog && !load_shader_cache(sceIoOpen(frag_fname, SCE_O_RDONLY, 0777), p->fshader, GL_TRUE))
				load_shader_cache(vgl_open_seed("f", subdir_hash, hash), p->fshader, GL_TRUE);
		}
#endif
		if (!p->vshader->prog || !p->fshader->prog) {
			if (p->vshader->is_glsl || p->fshader->is_glsl) {
				glsl_translator_set_process(p->vshader, p->fshader);
			}
			if (!p->vshader->prog) {
#ifdef HAVE_SHADER_CACHE
				fname = vert_fname;
#endif
				vgl_compile_shader(p->vshader, GL_TRUE);
			}
			if (!p->fshader->prog) {
#ifdef HAVE_SHADER_CACHE
				fname = frag_fname;
#endif
				vgl_compile_shader(p->fshader, GL_TRUE);
			}
		}

		if (!p->vshader->prog || !p->fshader->prog) {
			glsl_sema_mode = VGL_MODE_POSTPONED;
			return;
		}
		// Setting progressive default attribute bindings
		set_default_attrib_binding();

		if (p->glsl_attr_map) {
			for (int i = 0; i < p->num_glsl_attr; i++) {
				glBindAttribLocation(progr, p->glsl_attr_map[i].idx, p->glsl_attr_map[i].name);
			}
			vgl_free(p->glsl_attr_map);
			p->glsl_attr_map = NULL;
		}
		glsl_sema_mode = VGL_MODE_POSTPONED;
	}

	if (p->status == PROG_LINKED) {
		vgl_log("%s:%d: %s: A program has been re-linked. vitaGL doesn't support re-linking, glitches may happen.\n", __FILE__, __LINE__, __func__);
		return;
	}
	program saved = *p;
	
	// Set up uniform buffers
	if (p->vshader->unif_buf_size) {
		p->unif_vbuffer = vglCalloc(1, p->vshader->unif_buf_size);
		if (!p->unif_vbuffer)
			goto link_failed;
	}
	if (p->fshader->unif_buf_size) {
		p->unif_fbuffer = vglCalloc(1, p->fshader->unif_buf_size);
		if (!p->unif_fbuffer)
			goto link_failed;
	}

	// Analyzing fragment shader
	uint32_t i, cnt, j;
	for (i = 0; i < TEXTURE_IMAGE_UNITS_NUM; i++) {
		p->frag_texunits[i] = GL_FALSE;
		p->vert_texunits[i] = GL_FALSE;
	}
	cnt = sceGxmProgramGetParameterCount(p->fshader->prog);
	uint32_t *_ptr = vglProgramGetParameterBase(p->fshader->prog);
	uint32_t *ptr = _ptr;
	for (i = 0; i < cnt; i++) {
		SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
		SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
		if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER || (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM)) {
			p->frag_uniforms_num++;
		} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER) {
			ubo *u = (ubo *)vglMalloc(sizeof(ubo));
			if (!u)
				goto link_failed;
			u->chain = p->frag_ubos;
			u->idx = sceGxmProgramParameterGetResourceIndex(param);
			u->ptr = (const SceGxmProgramParameter *)get_block_details(p->fshader->unif_blk, u->idx);
			u->bind = 0;
			u->alias = NULL;
			p->frag_ubos = u;
			if (!u->ptr)
				goto link_failed;
		}
		ptr += 4;
	}
	if (p->frag_uniforms_num) {
		p->frag_uniforms = (uniform *)vglMalloc(sizeof(uniform) * p->frag_uniforms_num);
		if (!p->frag_uniforms)
			goto link_failed;
		ptr = _ptr;
		j = 0;
		for (i = 0; i < cnt; i++) {
			SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
			SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
			if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
				uint32_t resource = sceGxmProgramParameterGetResourceIndex(param);
				if (resource >= TEXTURE_IMAGE_UNITS_NUM)
					goto link_failed;
				uint8_t texunit_idx = resource + 1;
				if (p->max_frag_texunit_idx < texunit_idx)
					p->max_frag_texunit_idx = texunit_idx;
				uniform *u = &p->frag_uniforms[j++];
				u->ptr = param;
				u->type = sceGxmProgramParameterIsSamplerCube(param) ? UNIFORM_CUBE_SAMPLER : UNIFORM_SAMPLER;
				u->sampler_index = 0;
				p->frag_texunits[texunit_idx - 1] = u;
#ifdef HAVE_GLSL_TEXTURE_SIZE
				u->sampler = NULL;
				const char *pname = sceGxmProgramParameterGetName(param);
				for (uint8_t i = 0; i < p->fshader->sized_samplers_num; i++) {
					if (!strcmp(pname, p->fshader->sized_samplers[i].name)) {
						char smp[16];
						sprintf(smp, "vgl_smp%u", i);
						// If the linked uniform is missing, textureSize usage has been stripped out
						if (sceGxmProgramFindParameterByName(p->fshader->prog, smp))
							u->sampler = &p->fshader->sized_samplers[i];
						break;
					}
				}
#endif
			} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM) {
				if (sceGxmProgramParameterGetResourceIndex(param) >= p->fshader->unif_buf_size / 4)
					goto link_failed;
				uniform *u = &p->frag_uniforms[j++];
				u->ptr = param;
				u->vptr = NULL;
				u->changed = p->unif_changed;
				u->fptr = (uint8_t *)p->unif_fbuffer + sceGxmProgramParameterGetResourceIndex(param) * 4;
				u->type = UNIFORM_DATA;
#ifdef HAVE_GLSL_TEXTURE_SIZE
				u->sampler = NULL;
				const char *pname = sceGxmProgramParameterGetName(param);
				for (uint8_t i = 0; i < p->fshader->sized_samplers_num; i++) {
					char smp[16];
					sprintf(smp, "vgl_smp%u", i);
					if (!strcmp(pname, smp)) {
						u->sampler = &p->fshader->sized_samplers[i];
						break;
					}
				}
#endif
			}
			ptr += 4;
		}
#ifdef HAVE_GLSL_TEXTURE_SIZE
		// Assign to every sampler uniform the uniform buffer ptr of the equivalent data uniform
		for (int i = 0; i < p->frag_uniforms_num; i++) {
			uniform *u1 = &p->frag_uniforms[i];
			if (u1->sampler && u1->type != UNIFORM_DATA) {
				for (int j = 0; j < p->frag_uniforms_num; j++) {
					uniform *u2 = &p->frag_uniforms[j];
					if (u2->sampler == u1->sampler && u2->type == UNIFORM_DATA) {
						u1->fptr = u2->fptr;
						u1->changed = u2->changed;
						break;
					}
				}
			}
		}
#endif
#ifdef HAVE_FFP_SHADER_SUPPORT
		for (int i = 0; i < FFP_BINDS_NUM; i++) {
			SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->fshader->prog, ffp_bind_names[i]);
			if (param) {
				for (int j = 0; j < p->frag_uniforms_num; j++) {
					if (p->frag_uniforms[j].ptr == param) {
						p->ffp_binds[i] = &p->frag_uniforms[j];
						break;
					}
				}
			}
		}
#endif
	}

	// Analyzing vertex shader
	cnt = sceGxmProgramGetParameterCount(p->vshader->prog);
	_ptr = vglProgramGetParameterBase(p->vshader->prog);
	ptr = _ptr;
	for (i = 0; i < cnt; i++) {
		SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
		SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
		if (cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) {
			p->attr_num++;
			if (p->attr_num > VERTEX_ATTRIBS_NUM)
				goto link_failed;
		} else if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER || (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM)) {
			p->vert_uniforms_num++;
		} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER) {
			ubo *u = (ubo *)vglMalloc(sizeof(ubo));
			if (!u)
				goto link_failed;
			u->chain = p->vert_ubos;
			u->idx = sceGxmProgramParameterGetResourceIndex(param);
			u->ptr = (const SceGxmProgramParameter *)get_block_details(p->vshader->unif_blk, u->idx);
			u->bind = 0;
			p->vert_ubos = u;
			if (!u->ptr)
				goto link_failed;
			u->alias = has_block_alias(p->frag_ubos, ((block_uniform *)u->ptr)->name);
		}
		ptr += 4;
	}
	if (p->vert_uniforms_num) {
		p->vert_uniforms = (uniform *)vglMalloc(sizeof(uniform) * p->vert_uniforms_num);
		if (!p->vert_uniforms)
			goto link_failed;
		j = 0;
		ptr = _ptr;
		for (i = 0; i < cnt; i++) {
			SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
			SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
			if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
				uint32_t resource = sceGxmProgramParameterGetResourceIndex(param);
				if (resource >= TEXTURE_IMAGE_UNITS_NUM)
					goto link_failed;
				uint8_t texunit_idx = resource + 1;
				if (p->max_vert_texunit_idx < texunit_idx)
					p->max_vert_texunit_idx = texunit_idx;
				uniform *u = &p->vert_uniforms[j++];
				u->ptr = param;
				u->type = sceGxmProgramParameterIsSamplerCube(param) ? UNIFORM_CUBE_SAMPLER : UNIFORM_SAMPLER;
				u->sampler_index = 0;
				p->vert_texunits[texunit_idx - 1] = u;
			} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM) {
				if (sceGxmProgramParameterGetResourceIndex(param) >= p->vshader->unif_buf_size / 4)
					goto link_failed;
				uniform *u = &p->vert_uniforms[j++];
				u->ptr = param;
				u->type = UNIFORM_DATA;
				u->vptr = (uint8_t *)p->unif_vbuffer + sceGxmProgramParameterGetResourceIndex(param) * 4;
				u->changed = p->unif_changed;
				u->fptr = get_uniform_alias_data_ptr(p->frag_uniforms, p->frag_uniforms_num, sceGxmProgramParameterGetName(param));
			}
			ptr += 4;
		}
#ifdef HAVE_FFP_SHADER_SUPPORT
		for (int i = 0; i < FFP_BINDS_NUM; i++) {
			SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, ffp_bind_names[i]);
			if (param) {
				for (int j = 0; j < p->vert_uniforms_num; j++) {
					if (p->vert_uniforms[j].ptr == param) {
						p->ffp_binds[i] = &p->vert_uniforms[j];
						break;
					}
				}
			}
		}
#endif
	}

#ifdef ENABLE_LEGACY_PIPELINE
	// Creating fragment and vertex program via sceGxmShaderPatcher if using vgl* draw pipeline
	if (p->attr_mode != VGL_ATTRIB_REGULAR) {
		patch_vertex_program(p->vshader->id, p->attr, p->attr_num,
			p->stream, p->attr_mode == VGL_ATTRIB_UNPACKED ? p->attr_num : 1, &p->vprog);
		rebuild_frag_shader(p->fshader->id, &p->fprog, (SceGxmProgram *)p->vshader->prog, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4);
		p->is_fbo_float = is_fbo_float;
		p->msaa = cur_msaa;

		// Populating current blend settings
		p->blend_info.raw = blend_info.raw;
	} else
#endif
	{
		// Checking if bound attributes are aligned
		p->has_unaligned_attrs = GL_FALSE;

		for (i = 0; i < p->attr_num; i++) {
			p->attr_map[i] = i;
			if (p->attr[i].regIndex == 0xDEAD) {
				p->has_unaligned_attrs = GL_TRUE;
				break;
			}
		}

		// Fixing attributes mapping cache if in presence of unaligned attributes
		if (p->has_unaligned_attrs) {
			int j = 0;
			for (i = 0; i < p->attr_highest_idx; i++) {
				if (p->attr[i].regIndex != 0xDEAD) {
					p->attr_map[j] = i;
					j++;
				}
			}
		}
	}
#ifdef SAMPLERS_SPEEDHACK
	for (int i = 0; i < p->max_frag_texunit_idx; i++)
		if (!p->frag_texunits[i])
			p->frag_texunits[i] = &vgl_dummy_texunit;
	for (int i = 0; i < p->max_vert_texunit_idx; i++)
		if (!p->vert_texunits[i])
			p->vert_texunits[i] = &vgl_dummy_texunit;
#endif
	p->status = PROG_LINKED;
	// 1.2 (upstream 9bf3e3f): glUseProgram(cur_program) no longer marks the uniform buffers dirty. A (re)linked program that is the one in use has fresh
	// uniform buffers that no draw has uploaded yet: mark them here, where the old glUseProgram(same id) used to.
	if (progr == cur_program)
		vgl_dirty_all_uniforms();
	return;
link_failed:
	vgl_free(p->unif_vbuffer);
	vgl_free(p->unif_fbuffer);
	vgl_free(p->vert_uniforms);
	vgl_free(p->frag_uniforms);
	while (p->vert_ubos) {
		ubo *next = (ubo *)p->vert_ubos->chain;
		vgl_free(p->vert_ubos);
		p->vert_ubos = next;
	}
	while (p->frag_ubos) {
		ubo *next = (ubo *)p->frag_ubos->chain;
		vgl_free(p->frag_ubos);
		p->frag_ubos = next;
	}
	*p = saved;
}

// 0040: the whole link is timed (vglBackendTimingTake [1]); a program that this call linked has its first vglDrawElementsDirect draw timed ([2])
void glLinkProgram(GLuint progr) {
	THREAD_SAFE()
	uint64_t t0 = sceKernelGetProcessTimeWide();
	const GLboolean was_linked = (progr && progr <= MAX_CUSTOM_PROGRAMS && progs[progr - 1].status == PROG_LINKED) ? GL_TRUE : GL_FALSE;
	link_program_untimed(progr);
	vgl_bt_note(1, t0);
	if (progr && progr <= MAX_CUSTOM_PROGRAMS && !was_linked && progs[progr - 1].status == PROG_LINKED)
		progs[progr - 1].fresh_draw = 1;
}

void glUseProgram(GLuint prog) {
	THREAD_SAFE()

	if (cur_program == prog) {
		return;
	}

	// Setting current custom program to passed program
	cur_program = prog;
	vgl_dirty_all_uniforms();
}

GLuint glGetUniformBlockIndex(GLuint prog, const GLchar *uniformBlockName) {
	// Grabbing passed program
	program *p = &progs[prog - 1];

	// Getting the desired location
	ubo *j = p->vert_ubos;
	while (j) {
		block_uniform *b = (block_uniform *)j->ptr;
		if (!strcmp(b->name, uniformBlockName))
			return j->alias ? (GLuint)j->alias : (GLuint)j;
		j = j->chain;
	}
	j = p->frag_ubos;
	while (j) {
		block_uniform *b = (block_uniform *)j->ptr;
		if (!strcmp(b->name, uniformBlockName))
			return (GLuint)j;
		j = j->chain;
	}

	return GL_INVALID_INDEX;
}

void glUniformBlockBinding(GLuint prog, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
	THREAD_SAFE()

	ubo *u = (ubo *)uniformBlockIndex;
	u->bind = uniformBlockBinding;
}

GLint glGetUniformLocation(GLuint prog, const GLchar *name) {
	// Grabbing passed program
	program *p = &progs[prog - 1];

	// texture, sampler and matrix are reserved keywords in CG but are not in GLSL
	if (!strcmp(name, "texture"))
		name = "vgl_tex";
	else if (!strcmp(name, "Texture"))
		name = "Vgl_tex";
	else if (!strcmp(name, "matrix"))
		name = "_matrix";
	else if (!strcmp(name, "Matrix"))
		name = "_Matrix";
	else if (!strcmp(name, "sampler"))
		name = "_sampler";

#ifdef STRICT_UNIFORMS_COMPLIANCE
	int index = 0;
	char tmp[64];
	char *start = strstr(name, "[");
	if (start) {
		strcpy(tmp, name);
		start = tmp + (start - name);
		name = tmp;
		char *end = strstr(start + 1, "]");
		start[0] = end[0] = 0;
		index = atoi(start + 1);
	}

	uniform_location ret;
#endif
	// Checking if parameter is a vertex or fragment related one
	uniform *j;
	uint32_t cnt;
	const SceGxmProgramParameter *u = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (u == NULL) {
		u = sceGxmProgramFindParameterByName(p->fshader->prog, name);
		if (u == NULL) {
			return -1;
		} else {
			j = p->frag_uniforms;
			cnt = p->frag_uniforms_num;
#ifdef STRICT_UNIFORMS_COMPLIANCE
			ret.is_vertex = GL_FALSE;
#endif
		}
	} else {
		j = p->vert_uniforms;
		cnt = p->vert_uniforms_num;
#ifdef STRICT_UNIFORMS_COMPLIANCE
		ret.is_vertex = GL_TRUE;
#endif
	}

	// Getting the desired location
	for (uint32_t i = 0; i < cnt; i++) {
		if (j[i].ptr == u) {
#ifdef STRICT_UNIFORMS_COMPLIANCE			
			ret.offset = index;
			ret.zero = 0;
			ret.program_idx = prog - 1;
			ret.uniform_idx = i;
			return ret.raw;
#else
			return -((GLint)&j[i]);
#endif
		}
	}

	return -1;
}

#define vgl_fill_uniform_data(p, type, size, count) { \
	if (count == 1 && size >= 1 && size <= 4 && offs == 0 && (type == SCE_GXM_PARAMETER_TYPE_F32 || type == SCE_GXM_PARAMETER_TYPE_S32)) { \
		const SceGxmParameterType dst_type = sceGxmProgramParameterGetType(u->ptr); \
		if (dst_type == type) { \
			if (u->vptr) { \
				__builtin_memcpy(u->vptr, p, size * sizeof(uint32_t)); \
				u->changed[0] = 1; \
				dirty_shader_vert_unifs = GL_TRUE; \
			} \
			if (u->fptr) { \
				__builtin_memcpy(u->fptr, p, size * sizeof(uint32_t)); \
				u->changed[1] = 1; \
				dirty_shader_frag_unifs = GL_TRUE; \
			} \
		} else { \
			if (u->vptr) { \
				vglSetUniformData(u->vptr, dst_type, offs, count, size, p, type); \
				u->changed[0] = 1; \
				dirty_shader_vert_unifs = GL_TRUE; \
			} \
			if (u->fptr) { \
				vglSetUniformData(u->fptr, dst_type, offs, count, size, p, type); \
				u->changed[1] = 1; \
				dirty_shader_frag_unifs = GL_TRUE; \
			} \
		} \
	} else { \
		if (u->vptr) { \
			vglSetUniformData(u->vptr, sceGxmProgramParameterGetType(u->ptr), offs, count, size, p, type); \
			u->changed[0] = 1; \
			dirty_shader_vert_unifs = GL_TRUE; \
		} \
		if (u->fptr) { \
			vglSetUniformData(u->fptr, sceGxmProgramParameterGetType(u->ptr), offs, count, size, p, type); \
			u->changed[1] = 1; \
			dirty_shader_frag_unifs = GL_TRUE; \
		} \
	} \
}

inline void glUniform1i(GLint location, GLint v0) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	if (u->type != UNIFORM_DATA) { // Sampler
		u->sampler_index = v0;
	} else { // Regular Uniform
		vgl_fill_uniform_data(&v0, SCE_GXM_PARAMETER_TYPE_S32, 1, 1)
	}
}

void glProgramUniform1i(GLuint prog, GLint location, GLint v0) {
	glUniform1i(location, v0);
}

inline void glUniform1iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);
	
	// Setting passed value to desired uniform
	if (u->type != UNIFORM_DATA) { // Sampler
		u->sampler_index = value[0];
	} else { // Regular Uniform
		vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 1, count)
	}
}

void glProgramUniform1iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform1iv(location, count, value);
}

inline void glUniform1f(GLint location, GLfloat v0) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);
	
	// Setting passed value to desired uniform
	vgl_fill_uniform_data(&v0, SCE_GXM_PARAMETER_TYPE_F32, 1, 1)
}

void glProgramUniform1f(GLuint prog, GLint location, GLfloat v0) {
	glUniform1f(location, v0);
}

inline void glUniform1fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 1, count)
}

void glProgramUniform1fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform1fv(location, count, value);
}

inline void glUniform2i(GLint location, GLint v0, GLint v1) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLint src[2] = {v0, v1};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_S32, 2, 1)
}

void glProgramUniform2i(GLuint prog, GLint location, GLint v0, GLint v1) {
	glUniform2i(location, v0, v1);
}

inline void glUniform2iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 2, count)
}

void glProgramUniform2iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform2iv(location, count, value);
}

inline void glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLfloat src[2] = {v0, v1};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_F32, 2, 1)
}

void glProgramUniform2f(GLuint prog, GLint location, GLfloat v0, GLfloat v1) {
	glUniform2f(location, v0, v1);
}

inline void glUniform2fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 2, count)
}

void glProgramUniform2fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform2fv(location, count, value);
}

inline void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLint src[3] = {v0, v1, v2};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_S32, 3, 1)
}

void glProgramUniform3i(GLuint prog, GLint location, GLint v0, GLint v1, GLint v2) {
	glUniform3i(location, v0, v1, v2);
}

inline void glUniform3iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 3, count)
}

void glProgramUniform3iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform3iv(location, count, value);
}

inline void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLfloat src[3] = {v0, v1, v2};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_F32, 3, 1)
}

void glProgramUniform3f(GLuint prog, GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
	glUniform3f(location, v0, v1, v2);
}

inline void glUniform3fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 3, count)
}

void glProgramUniform3fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform3fv(location, count, value);
}

inline void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLint src[4] = {v0, v1, v2, v3};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_S32, 4, 1)
}

void glProgramUniform4i(GLuint prog, GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
	glUniform4i(location, v0, v1, v2, v3);
}

inline void glUniform4iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 4, count)
}

void glProgramUniform4iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform4iv(location, count, value);
}

inline void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLfloat src[4] = {v0, v1, v2, v3};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_F32, 4, 1)
}

void glProgramUniform4f(GLuint prog, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
	glUniform4f(location, v0, v1, v2, v3);
}

inline void glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 4, count)
}

void glProgramUniform4fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform4fv(location, count, value);
}

inline void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	float stack_tmp[4 * 4];
	if (transpose) {
		float *tmp = count <= 4 ? stack_tmp : gpu_alloc_mapped_temp(count * 4 * sizeof(float));
		for (int i = 0; i < count; i++) {
			matrix2x2_transpose(&tmp[i * 4], &value[i * 4]);
		}
		value = tmp;
	}
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 2, 2 * count)
}

void glProgramUniformMatrix2fv(GLuint prog, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	glUniformMatrix2fv(location, count, transpose, value);
}

inline void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	float stack_tmp[4 * 9];
	if (transpose) {
		float *tmp = count <= 4 ? stack_tmp : gpu_alloc_mapped_temp(count * 9 * sizeof(float));
		for (int i = 0; i < count; i++) {
			matrix3x3_transpose(&tmp[i * 9], &value[i * 9]);
		}
		value = tmp;
	}
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 3, 3 * count)
}

void glProgramUniformMatrix3fv(GLuint prog, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	glUniformMatrix3fv(location, count, transpose, value);
}

inline void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0) {
		return;
	}

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	float stack_tmp[4 * 16];
	if (transpose) {
		float *tmp = count <= 4 ? stack_tmp : gpu_alloc_mapped_temp(count * 16 * sizeof(float));
		for (int i = 0; i < count; i++) {
			matrix4x4_transpose(&tmp[i * 16], &value[i * 16]);
		}
		value = tmp;
	}
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 4, 4 * count)
}

void glProgramUniformMatrix4fv(GLuint prog, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	glUniformMatrix4fv(location, count, transpose, value);
}

void glEnableVertexAttribArray(GLuint index) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	uint32_t bit = 1 << index;
	if (!(cur_vao->vertex_attrib_state & bit)) {
		cur_vao->vertex_attrib_state |= bit;
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#else
	cur_vao->vertex_attrib_state |= (1 << index);
#endif
}

void glDisableVertexAttribArray(GLuint index) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	uint32_t bit = 1 << index;
	if (cur_vao->vertex_attrib_state & bit) {
		cur_vao->vertex_attrib_state &= ~bit;
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#else
	cur_vao->vertex_attrib_state &= ~(1 << index);
#endif
}

void glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	} else if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE);
	}
#endif
	pointer[0] = (void *)cur_vao->vertex_attrib_offsets[index];
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (size < 1 || size > 4 || stride < 0 || index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	uint32_t old_offset = cur_vao->vertex_attrib_offsets[index];
	uint32_t old_vbo = cur_vao->vertex_attrib_vbo[index];
	SceGxmAttributeFormat old_format = cur_vao->vertex_attrib_config[index].format;
	uint8_t old_component_count = cur_vao->vertex_attrib_config[index].componentCount;
	uint16_t old_stride = cur_vao->vertex_stream_config[index].stride;
#endif
	cur_vao->vertex_attrib_offsets[index] = (uint32_t)pointer;
	cur_vao->vertex_attrib_vbo[index] = vertex_array_unit;

	SceGxmVertexAttribute *attributes = &cur_vao->vertex_attrib_config[index];
	SceGxmVertexStream *streams = &cur_vao->vertex_stream_config[index];

	// Detecting attribute format and size
	unsigned short bpe;
	switch (type) {
	case GL_HALF_FLOAT:
	case GL_HALF_FLOAT_OES:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F16;
		bpe = 2;
		break;
	case GL_FLOAT:
#ifdef HAVE_FIXED_ATTRIBUTES
	case GL_FIXED:
#endif
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = 4;
		break;
	case GL_SHORT:
		attributes->format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_S16N : SCE_GXM_ATTRIBUTE_FORMAT_S16;
		bpe = 2;
		break;
	case GL_UNSIGNED_SHORT:
		attributes->format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_U16N : SCE_GXM_ATTRIBUTE_FORMAT_U16;
		bpe = 2;
		break;
	case GL_BYTE:
		attributes->format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_S8N : SCE_GXM_ATTRIBUTE_FORMAT_S8;
		bpe = 1;
		break;
	case GL_UNSIGNED_BYTE:
		attributes->format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_U8N : SCE_GXM_ATTRIBUTE_FORMAT_U8;
		bpe = 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}
	attributes->componentCount = size;
	streams->stride = stride ? stride : bpe * size;
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (old_offset != cur_vao->vertex_attrib_offsets[index] || old_vbo != cur_vao->vertex_attrib_vbo[index] || old_format != attributes->format || old_component_count != attributes->componentCount || old_stride != streams->stride) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
}

void glVertexAttribDivisor(GLuint index, GLuint divisor) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)		
	}
	if (divisor > 1) {
		vgl_log("%s:%d %s: Divisor value higher than 1 is not supported. Downgrading to 1.\n", __FILE__, __LINE__, __func__);
	}
#endif

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	uint32_t bit = 1 << index;
	if (divisor) {
		if (!(cur_vao->vertex_attrib_divisor & bit)) {
			cur_vao->vertex_attrib_divisor |= bit;
			cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
		}
	} else if (cur_vao->vertex_attrib_divisor & bit) {
		cur_vao->vertex_attrib_divisor &= ~bit;
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#else
	if (divisor) {
		cur_vao->vertex_attrib_divisor |= (1 << index);
	} else {
		cur_vao->vertex_attrib_divisor &= ~(1 << index);
	}
#endif
}

void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	switch (pname) {
	case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_vbo[index] : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? GL_TRUE : GL_FALSE;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_SIZE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_config[index].componentCount : cur_vao->vertex_attrib_size[index];
		break;
	case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_stream_config[index].stride : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_TYPE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? gxm_vd_fmt_to_gl(cur_vao->vertex_attrib_config[index].format) : GL_FLOAT;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? (cur_vao->vertex_attrib_config[index].format >= SCE_GXM_ATTRIBUTE_FORMAT_U8N && cur_vao->vertex_attrib_config[index].format <= SCE_GXM_ATTRIBUTE_FORMAT_S16N) : GL_FALSE;
		break;
	case GL_CURRENT_VERTEX_ATTRIB:
#ifndef SKIP_ERROR_HANDLING
		if (index == 0) {
			SET_GL_ERROR(GL_INVALID_OPERATION)
		}
#endif
		params[0] = cur_vao->vertex_attrib_value[index][0];
		params[1] = cur_vao->vertex_attrib_size[index] > 1 ? cur_vao->vertex_attrib_value[index][1] : 0;
		params[2] = cur_vao->vertex_attrib_size[index] > 2 ? cur_vao->vertex_attrib_value[index][2] : 0;
		params[3] = cur_vao->vertex_attrib_size[index] > 3 ? cur_vao->vertex_attrib_value[index][3] : 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	switch (pname) {
	case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_vbo[index] : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? GL_TRUE : GL_FALSE;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_SIZE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_config[index].componentCount : cur_vao->vertex_attrib_size[index];
		break;
	case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_stream_config[index].stride : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_TYPE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? gxm_vd_fmt_to_gl(cur_vao->vertex_attrib_config[index].format) : GL_FLOAT;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? (cur_vao->vertex_attrib_config[index].format >= SCE_GXM_ATTRIBUTE_FORMAT_U8N && cur_vao->vertex_attrib_config[index].format <= SCE_GXM_ATTRIBUTE_FORMAT_S16N) : GL_FALSE;
		break;
	case GL_CURRENT_VERTEX_ATTRIB:
#ifndef SKIP_ERROR_HANDLING
		if (index == 0) {
			SET_GL_ERROR(GL_INVALID_OPERATION)
		}
#endif
		params[0] = cur_vao->vertex_attrib_value[index][0];
		params[1] = cur_vao->vertex_attrib_size[index] > 1 ? cur_vao->vertex_attrib_value[index][1] : 0;
		params[2] = cur_vao->vertex_attrib_size[index] > 2 ? cur_vao->vertex_attrib_value[index][2] : 0;
		params[3] = cur_vao->vertex_attrib_size[index] > 3 ? cur_vao->vertex_attrib_value[index][3] : 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glVertexAttrib1f(GLuint index, GLfloat v0) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 1) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(1);
	cur_vao->vertex_attrib_size[index] = 1;
	cur_vao->vertex_attrib_value[index][0] = v0;
}

void glVertexAttrib2f(GLuint index, GLfloat v0, GLfloat v1) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 2) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(2);
	cur_vao->vertex_attrib_size[index] = 2;
	cur_vao->vertex_attrib_value[index][0] = v0;
	cur_vao->vertex_attrib_value[index][1] = v1;
}

void glVertexAttrib3f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 3) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(3);
	cur_vao->vertex_attrib_size[index] = 3;
	cur_vao->vertex_attrib_value[index][0] = v0;
	cur_vao->vertex_attrib_value[index][1] = v1;
	cur_vao->vertex_attrib_value[index][2] = v2;
}

void glVertexAttrib4f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 4) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(4);
	cur_vao->vertex_attrib_size[index] = 4;
	cur_vao->vertex_attrib_value[index][0] = v0;
	cur_vao->vertex_attrib_value[index][1] = v1;
	cur_vao->vertex_attrib_value[index][2] = v2;
	cur_vao->vertex_attrib_value[index][3] = v3;
}

void glVertexAttrib1fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 1) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(1);
	cur_vao->vertex_attrib_size[index] = 1;
	cur_vao->vertex_attrib_value[index][0] = v[0];
}

void glVertexAttrib2fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 2) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(2);
	cur_vao->vertex_attrib_size[index] = 2;
	cur_vao->vertex_attrib_value[index][0] = v[0];
	cur_vao->vertex_attrib_value[index][1] = v[1];
}

void glVertexAttrib3fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 3) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(3);
	cur_vao->vertex_attrib_size[index] = 3;
	cur_vao->vertex_attrib_value[index][0] = v[0];
	cur_vao->vertex_attrib_value[index][1] = v[1];
	cur_vao->vertex_attrib_value[index][2] = v[2];
}

void glVertexAttrib4fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	if (cur_vao->vertex_attrib_size[index] != 4) {
		cur_vao->vertex_layout_version = ++vertex_layout_cur_ver;
	}
#endif
	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(4);
	cur_vao->vertex_attrib_size[index] = 4;
	cur_vao->vertex_attrib_value[index][0] = v[0];
	cur_vao->vertex_attrib_value[index][1] = v[1];
	cur_vao->vertex_attrib_value[index][2] = v[2];
	cur_vao->vertex_attrib_value[index][3] = v[3];
}

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (target != GL_UNIFORM_BUFFER) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	} else if (index >= UBOS_NUM) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, index)
	} else if (size <= 0) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, size)
	}
#endif

	ubo_buf[index] = (vbo *)buffer;
	ubo_offset[index] = offset;
}

void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (target != GL_UNIFORM_BUFFER) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	} else if (index >= UBOS_NUM) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, index)
	}
#endif

	ubo_buf[index] = (vbo *)buffer;
	ubo_offset[index] = 0;
}

void glBindAttribLocation(GLuint prog, GLuint index, const GLchar *name) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];
	
	// If we use VGL_MODE_POSTPONED, we perform attributes binding in glLinkProgram
	if (glsl_sema_mode == VGL_MODE_POSTPONED && p->vshader->is_glsl) {
		if (!p->glsl_attr_map)
			p->glsl_attr_map = vglMalloc(sizeof(attr_mapping) * VERTEX_ATTRIBS_NUM);
		p->glsl_attr_map[p->num_glsl_attr].idx = index;
		strcpy(p->glsl_attr_map[p->num_glsl_attr++].name, name);
		return;
	}

	// Looking for desired parameter in requested program
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL || sceGxmProgramParameterGetCategory(param) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
		return;
	uint32_t attr_index = sceGxmProgramParameterGetResourceIndex(param);
	
	// Swapping any previously made bind to the requested attribute
	for (int i = 0; i < p->attr_highest_idx; i++) {
		if (p->attr[i].regIndex == attr_index) {
			p->attr[i].regIndex = p->attr[index].regIndex;
			break;
		}
	}
	
	// Set new binding to the requested attribute
	p->attr[index].regIndex = attr_index;
	if (p->attr_highest_idx <= index) {
		p->attr_highest_idx = index + 1;
	}
#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
	p->vertex_layout_version = ++vertex_program_layout_current_ver;
#endif
}

GLint glGetAttribLocation(GLuint prog, const GLchar *name) {
	program *p = &progs[prog - 1];
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL || sceGxmProgramParameterGetCategory(param) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
		return -1;
	uint32_t index = sceGxmProgramParameterGetResourceIndex(param);

	// Return requested attribute location
	for (int i = 0; i < p->attr_highest_idx; i++) {
		if (p->attr[i].regIndex == index)
			return i;
	}

	return -1;
}

void glGetActiveAttrib(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed program
	program *p = &progs[prog - 1];

	int i, cnt = sceGxmProgramGetParameterCount(p->vshader->prog);
	uint32_t *ptr = vglProgramGetParameterBase(p->vshader->prog);
	SceGxmProgramParameter *param;
	for (i = 0; i < cnt; i++) {
		param = (SceGxmProgramParameter *)ptr;
		if (sceGxmProgramParameterGetCategory(param) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE && (sceGxmProgramParameterGetResourceIndex(param) / 4) == index)
			break;
		ptr += 4;
	}

	// Copying attribute name
	const char *pname = sceGxmProgramParameterGetName(param);
	bufSize = min(strlen(pname), bufSize - 1);
	if (length)
		*length = bufSize;
	strncpy(name, pname, bufSize);
	name[bufSize] = 0;

	*type = gxm_attr_type_to_gl(sceGxmProgramParameterGetComponentCount(param), sceGxmProgramParameterGetArraySize(param));
	*size = 1;
}

void glGetActiveUniform(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0 || (index >= p->vert_uniforms_num + p->frag_uniforms_num)) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing desired uniform
	uniform *u;
	if (index >= p->vert_uniforms_num) {
		u = &p->frag_uniforms[index - p->vert_uniforms_num];
	} else {
		u = &p->vert_uniforms[index];
	}

	// Detecting uniform type
	const char *pname = sceGxmProgramParameterGetName(u->ptr);
	if (sceGxmProgramParameterGetCategory(u->ptr) == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
		*type = sceGxmProgramParameterIsSamplerCube(u->ptr) ? GL_SAMPLER_CUBE : GL_SAMPLER_2D;
		*size = 1;
	} else {
		*size = sceGxmProgramParameterGetArraySize(u->ptr);
		*type = gxm_unif_type_to_gl(sceGxmProgramParameterGetType(u->ptr), sceGxmProgramParameterGetComponentCount(u->ptr), size);
		if (*type >= GL_FLOAT_VEC2 && *type <= GL_FLOAT_VEC4 && *size > 1) {
			matrix_uniform *m = p->vshader->mat;
			while (m) {
				if (m->ptr == u->ptr) {
					gxm_unif_to_mat(type, size);
					break;
				}
				m = (matrix_uniform *)m->chain;
			}
			if (!m) {
				m = p->fshader->mat;
				while (m) {
					if (m->ptr == u->ptr) {
						gxm_unif_to_mat(type, size);
						break;
					}
					m = (matrix_uniform *)m->chain;
				}
			}
		}
	}
	
	// Copying uniform name
	if (bufSize == 0) {
		if (length)
			*length = 0;
		return;
	}
	// texture, sampler and matrix are reserved keywords in CG but are not in GLSL
	if (!strcmp(pname, "vgl_tex"))
		pname = "texture";
	else if (!strcmp(pname, "Vgl_tex"))
		pname = "Texture";
	else if (!strcmp(pname, "_matrix"))
		pname = "matrix";
	else if (!strcmp(pname, "_sampler"))
		pname = "sampler";

	bufSize = min(strlen(pname), bufSize - 1);
	if (length)
		*length = bufSize;
	strncpy(name, pname, bufSize);
	name[bufSize] = 0;
}

/*
 * ------------------------------
 * -    VGL_EXT_gxp_shaders     -
 * ------------------------------
 */

// Equivalent of glBindAttribLocation but for sceGxm architecture
void vglBindAttribLocation(GLuint prog, GLuint index, const GLchar *name, const GLuint num, const GLenum type) {
	THREAD_SAFE()

#ifdef ENABLE_LEGACY_PIPELINE
	// Grabbing passed program
	program *p = &progs[prog - 1];
	SceGxmVertexAttribute *attributes = &p->attr[index];
	SceGxmVertexStream *streams = &p->stream[index];

	// Looking for desired parameter in requested program
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL)
		return;

	// Setting stream index and offset values
	attributes->streamIndex = index;
	attributes->offset = 0;

	// Detecting attribute format and size
	int bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = sizeof(float);
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = sizeof(int16_t);
		break;
	case GL_UNSIGNED_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
		bpe = sizeof(uint8_t);
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	// Setting various info about the stream
	attributes->componentCount = num;
	attributes->regIndex = sceGxmProgramParameterGetResourceIndex(param);
	streams->stride = bpe * num;
	streams->indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	p->attr_mode = VGL_ATTRIB_UNPACKED;
#endif
}

// Equivalent of glBindAttribLocation but for sceGxm architecture when packed attributes are used
GLint vglBindPackedAttribLocation(GLuint prog, const GLchar *name, const GLuint num, const GLenum type, GLuint offset, GLint stride) {
#ifdef ENABLE_LEGACY_PIPELINE
	// Grabbing passed program
	program *p = &progs[prog - 1];
	SceGxmVertexAttribute *attributes = &p->attr[p->attr_idx];
	SceGxmVertexStream *streams = &p->stream[0];

	// Looking for desired parameter in requested program
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL)
		return GL_FALSE;

	// Setting stream index and offset values
	attributes->streamIndex = 0;
	attributes->offset = offset;

	// Detecting attribute format and size
	int bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = sizeof(float);
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = sizeof(int16_t);
		break;
	case GL_UNSIGNED_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
		bpe = sizeof(uint8_t);
		break;
	default:
		SET_GL_ERROR_WITH_RET(GL_INVALID_ENUM, GL_FALSE)
	}

	// Setting various info about the stream
	attributes->componentCount = num;
	attributes->regIndex = sceGxmProgramParameterGetResourceIndex(param);
	streams->stride = stride ? stride : bpe * num;
	streams->indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	p->attr_mode = VGL_ATTRIB_PACKED;
	p->attr_idx++;
#endif
	return GL_TRUE;
}

// Equivalent of glVertexAttribPointer but for sceGxm architecture
void vglVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, GLuint count, const GLvoid *pointer) {
	THREAD_SAFE()

#ifdef ENABLE_LEGACY_PIPELINE
#ifndef SKIP_ERROR_HANDLING
	// Error handling
	if (stride < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Detecting type size
	int bpe;
	switch (type) {
	case GL_FLOAT:
		bpe = sizeof(GLfloat);
		break;
	case GL_SHORT:
		bpe = sizeof(GLshort);
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	// Allocating enough memory on vitaGL mempool
	void *ptr = gpu_alloc_mapped_temp(count * bpe * size);

	// Copying passed data to vitaGL mempool
	if (stride == 0)
		vgl_fast_memcpy(ptr, pointer, count * bpe * size); // Faster if stride == 0
	else {
		int i;
		uint8_t *dst = (uint8_t *)ptr;
		uint8_t *src = (uint8_t *)pointer;
		for (i = 0; i < count; i++) {
			vgl_fast_memcpy(dst, src, bpe * size);
			dst += (bpe * size);
			src += stride;
		}
	}

	// Setting vertex stream to passed index in sceGxm
	sceGxmSetVertexStream(gxm_context, index, ptr);
#endif
}

void vglVertexAttribPointerMapped(GLuint index, const GLvoid *pointer) {
	THREAD_SAFE()

#ifdef ENABLE_LEGACY_PIPELINE
	// Setting vertex stream to passed index in sceGxm
	sceGxmSetVertexStream(gxm_context, index, pointer);
#endif
}

void vglGetShaderBinary(GLuint handle, GLsizei bufSize, GLsizei *length, void *binary) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed shader
	shader *s = shaders[handle - 1];

#ifndef SKIP_ERROR_HANDLING
	if (s->prog == NULL) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	}
#endif

	size_t sz;
	serialize_shader(binary, length ? (size_t *)length : &sz, s, GL_FALSE);
}

void vglAddSemanticBinding(const GLchar *const *varying, GLint index, GLenum type) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (glsl_custom_bindings_num >= MAX_CUSTOM_BINDINGS) {
		vgl_log("%s:%d %s: Too many custom bindings supplied. Consider increasing MAX_CUSTOM_BINDINGS.\n", __FILE__, __LINE__, __func__);
		return;
	}			
#endif
	strcpy(glsl_custom_bindings[glsl_custom_bindings_num].name, varying);
	glsl_custom_bindings[glsl_custom_bindings_num].idx = index;
	glsl_custom_bindings[glsl_custom_bindings_num].type = type;
	glsl_custom_bindings[glsl_custom_bindings_num++].ref_idx = glsl_current_ref_idx;
}

void vglAddSemanticBindingHint(const GLchar *const *varying, GLenum type) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (glsl_custom_bindings_num >= MAX_CUSTOM_BINDINGS) {
		vgl_log("%s:%d %s: Too many custom bindings supplied. Consider increasing MAX_CUSTOM_BINDINGS.\n", __FILE__, __LINE__, __func__);
		return;
	}			
#endif
	strcpy(glsl_custom_bindings[glsl_custom_bindings_num].name, varying);
	glsl_custom_bindings[glsl_custom_bindings_num].idx = -1;
	glsl_custom_bindings[glsl_custom_bindings_num].type = type;
	glsl_custom_bindings[glsl_custom_bindings_num++].ref_idx = glsl_current_ref_idx;
}

void vglUseLowPrecision(GLboolean val) {
	glsl_precision_low = val;
}

void vglSetSemanticBindingMode(GLenum mode) {
	glsl_sema_mode = mode;
}

void vglOverrideTexFormat(GLenum target) {
	THREAD_SAFE()

#ifdef HAVE_UNPURE_TEXFORMATS
	switch (target) {
	case GL_TEXTURE_1D:
		tex2d_override = 1;
		break;
	default:
		tex2d_override = 0;
		break;
	}
#endif
}

void vglShaderGxpBinary(GLsizei count, const GLuint *handles, const void *binary, GLsizei length) {
	THREAD_SAFE()
	if (count < 1 || !handles || !handles[0] || handles[0] > MAX_CUSTOM_SHADERS || !shaders[handles[0] - 1] || length < 0)
		return;

	// Grabbing passed shader
	shader *s = shaders[handles[0] - 1];
	
	if (s->prog || !shader_binary_valid(binary, (size_t)length, s->type))
		return;
	shader candidate = {0};
	candidate.size = length;
	candidate.prog = (SceGxmProgram *)vglMalloc(candidate.size);
	if (!candidate.prog)
		return;
	vgl_fast_memcpy((void *)candidate.prog, binary, candidate.size);
	if (sceGxmProgramCheck(candidate.prog) || vgl_register_program(candidate.prog, &candidate.id)) {
		discard_shader_candidate(&candidate);
		return;
	}
	candidate.unif_buf_size = sceGxmProgramGetDefaultUniformBufferSize(candidate.prog);
	commit_shader_candidate(s, &candidate);
}
