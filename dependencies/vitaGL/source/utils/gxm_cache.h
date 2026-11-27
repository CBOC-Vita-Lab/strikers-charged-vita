/*
 * gxm_cache.h (GLB, wave 5): optional elimination of redundant sceGxm state calls.
 *
 * vitaGL sets the vertex program, the fragment program, every vertex stream and every fragment texture again for each draw. With
 * vglSetSkipRedundantSets(GL_TRUE) the calls that would set what the context already has are not forwarded. The cache records every call that passes
 * through these wrappers (all of vitaGL includes this header through shared.h, so no call can bypass it) and is forgotten at sceGxmBeginScene /
 * sceGxmEndScene (libgxm does not promise that state survives a scene; the aurora-vita audit found that it did not after a suspend/resume) and after the
 * calls that set state behind its back (precomputed draws and vertex states, the reserve-uniform-buffer calls).
 *   vertex program   skipped when the same pointer is current
 *   fragment program same (a rebuilt blend variant is a different pointer: the patcher keeps one object per combination)
 *   vertex stream    skipped when the same pointer was set under the same current vertex program
 *   fragment texture skipped when the 16 bytes of the texture object equal what the unit has (vitaGL edits filters and modes in place before the call)
 *
 * 1.2 (GXM_STATE_CACHE, vglSetStateCache): the same for the state the draw path and the state setters of GL set (see the second part of this file).
 * Calls for any context but the main one (gxm_context; the splash screen has its own) are never cached or counted as skipped.
 */
#ifndef _GXM_CACHE_H_
#define _GXM_CACHE_H_

extern SceGxmContext *gxm_context;

typedef struct {
	const SceGxmVertexProgram *vprog;
	const SceGxmFragmentProgram *fprog;
	const void *vstream[SCE_GXM_MAX_VERTEX_STREAMS];
	const SceGxmVertexProgram *vstream_prog[SCE_GXM_MAX_VERTEX_STREAMS];
	uint32_t ftex[SCE_GXM_MAX_TEXTURE_UNITS][4];
	uint32_t ftex_valid;
} vgl_gxc_t;

extern vgl_gxc_t vgl_gxc;
extern GLboolean vgl_gxc_enabled;
extern uint32_t vgl_gxc_calls[4], vgl_gxc_skipped[4];

#ifdef GXM_STATE_CACHE
extern uint32_t vgl_gxs_valid;
#endif
static inline __attribute__((always_inline)) void vgl_gxc_reset(void) {
#ifdef GXM_STATE_CACHE
	vgl_gxs_valid = 0;
#endif
	vgl_gxc.vprog = NULL;
	vgl_gxc.fprog = NULL;
	vgl_gxc.ftex_valid = 0;
	for (int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
		vgl_gxc.vstream[i] = NULL;
		vgl_gxc.vstream_prog[i] = NULL;
	}
}

static inline __attribute__((always_inline)) void vgl_gxc_SetVertexProgram(SceGxmContext *ctx, const SceGxmVertexProgram *p) {
	vgl_gxc_calls[0]++;
	if (vgl_gxc_enabled && ctx == gxm_context) {
		if (vgl_gxc.vprog == p) {
			vgl_gxc_skipped[0]++;
			return;
		}
		vgl_gxc.vprog = p;
	}
	sceGxmSetVertexProgram(ctx, p);
}

static inline __attribute__((always_inline)) void vgl_gxc_SetFragmentProgram(SceGxmContext *ctx, const SceGxmFragmentProgram *p) {
	vgl_gxc_calls[1]++;
	if (vgl_gxc_enabled && ctx == gxm_context) {
		if (vgl_gxc.fprog == p) {
			vgl_gxc_skipped[1]++;
			return;
		}
		vgl_gxc.fprog = p;
	}
	sceGxmSetFragmentProgram(ctx, p);
}

static inline __attribute__((always_inline)) int vgl_gxc_SetVertexStream(SceGxmContext *ctx, uint32_t idx, const void *ptr) {
	vgl_gxc_calls[2]++;
	if (vgl_gxc_enabled && ctx == gxm_context && idx < SCE_GXM_MAX_VERTEX_STREAMS) {
		if (vgl_gxc.vstream[idx] == ptr && vgl_gxc.vstream_prog[idx] == vgl_gxc.vprog && vgl_gxc.vprog) {
			vgl_gxc_skipped[2]++;
			return 0;
		}
		vgl_gxc.vstream[idx] = ptr;
		vgl_gxc.vstream_prog[idx] = vgl_gxc.vprog;
	}
	return sceGxmSetVertexStream(ctx, idx, ptr);
}

static inline __attribute__((always_inline)) int vgl_gxc_SetFragmentTexture(SceGxmContext *ctx, uint32_t idx, const SceGxmTexture *tex) {
	vgl_gxc_calls[3]++;
	if (vgl_gxc_enabled && ctx == gxm_context && idx < SCE_GXM_MAX_TEXTURE_UNITS) {
		const uint32_t *w = (const uint32_t *)tex;
		uint32_t *c = vgl_gxc.ftex[idx];
		if ((vgl_gxc.ftex_valid & (1u << idx)) && c[0] == w[0] && c[1] == w[1] && c[2] == w[2] && c[3] == w[3]) {
			vgl_gxc_skipped[3]++;
			return 0;
		}
		c[0] = w[0];
		c[1] = w[1];
		c[2] = w[2];
		c[3] = w[3];
		vgl_gxc.ftex_valid |= 1u << idx;
	}
	return sceGxmSetFragmentTexture(ctx, idx, tex);
}

// (the real functions are called inside the wrappers above: a function-like macro is not expanded in its own expansion or when the name is not
// followed by a parenthesis)
#define sceGxmSetVertexProgram(ctx, p) vgl_gxc_SetVertexProgram(ctx, p)
#define sceGxmSetFragmentProgram(ctx, p) vgl_gxc_SetFragmentProgram(ctx, p)
#define sceGxmSetVertexStream(ctx, i, p) vgl_gxc_SetVertexStream(ctx, i, p)
#define sceGxmSetFragmentTexture(ctx, i, t) vgl_gxc_SetFragmentTexture(ctx, i, t)
#define sceGxmBeginScene(...) ({ vgl_gxc_reset(); int vgl_gxc_r_ = sceGxmBeginScene(__VA_ARGS__); vgl_gxc_reset(); vgl_gxc_r_; })
#define sceGxmEndScene(...) ({ vgl_gxc_reset(); int vgl_gxc_r_ = sceGxmEndScene(__VA_ARGS__); vgl_gxc_reset(); vgl_gxc_r_; })
#define sceGxmDrawPrecomputed(...) ({ int vgl_gxc_r_ = sceGxmDrawPrecomputed(__VA_ARGS__); vgl_gxc_reset(); vgl_gxc_r_; })
#define sceGxmSetPrecomputedVertexState(...) ({ sceGxmSetPrecomputedVertexState(__VA_ARGS__); vgl_gxc_reset(); })

#ifdef GXM_STATE_CACHE
/*
 * ---- 1.2: the state calls (GXM_STATE_CACHE, switched at run time by vglSetStateCache) ----
 * Each setter below is forwarded only when its arguments differ from the arguments of the last forwarded call of the same setter since the cache was last
 * forgotten. "The same setter" is exact: SetFrontDepthFunc and SetBackDepthFunc are two entries, a stencil function is compared with all six arguments, a viewport with
 * the bit patterns of its six floats. Nothing is inferred: no default value is assumed after a scene begins (the entry is empty, so the first call of a scene is always
 * forwarded), and the value of a call that was skipped is by construction the value libgxm already holds.
 * Forgotten (vgl_gxc_reset): sceGxmBeginScene, sceGxmEndScene, precomputed draws and vertex states, and a Reserve*DefaultUniformBuffer (it binds a buffer behind the
 * cache's back). A call for another context than the main one is neither cached nor counted as skipped.
 * vitaGL's own setters call these in pairs and groups (glDepthFunc also sets the write mode again, glStencilFunc sets the reference again, a clear pass sets the cull
 * mode, polygon mode and depth bias it needs and the restore sets them back): those are what this removes. Groups (vglSetStateCache mask bits):
 *   0x01 depth func / write      0x02 stencil func / ref      0x04 cull / polygon mode / two-sided / depth bias
 *   0x08 viewport / region clip  0x10 vertex textures         0x20 default uniform buffers and uniform buffer objects
 */
enum {
	GXS_FDEPTHFUNC, GXS_BDEPTHFUNC, GXS_FDEPTHWRITE, GXS_BDEPTHWRITE,
	GXS_FSTENCILFUNC, GXS_BSTENCILFUNC, GXS_FSTENCILREF, GXS_BSTENCILREF,
	GXS_FPOLYMODE, GXS_BPOLYMODE, GXS_CULL, GXS_TWOSIDED, GXS_FDEPTHBIAS, GXS_BDEPTHBIAS,
	GXS_VIEWPORT, GXS_REGIONCLIP,
	GXS_VTEX0, GXS_VTEX1, GXS_VTEX2, GXS_VTEX3, // vertex texture units 0-3 (higher units are never cached)
	GXS_VDEFUBUF, GXS_FDEFUBUF, GXS_VUBO, GXS_FUBO,
	GXS_KINDS // = VGL_GXS_KINDS in vitaGL.h
};
#define GXS_GROUP_DEPTH 0x01
#define GXS_GROUP_STENCIL 0x02
#define GXS_GROUP_RASTER 0x04
#define GXS_GROUP_VIEW 0x08
#define GXS_GROUP_VTEX 0x10
#define GXS_GROUP_UBUF 0x20

extern uint32_t vgl_gxs_val[GXS_KINDS][8]; // the arguments of the last forwarded call (32-bit words; at most 6 are used, a texture uses 4)
extern uint32_t vgl_gxs_mask;               // enabled groups (0 = the cache is off, every call is forwarded)
extern uint32_t vgl_gxs_calls[GXS_KINDS], vgl_gxs_skipped[GXS_KINDS];
extern const uint8_t vgl_gxs_group[GXS_KINDS];
extern uint32_t vgl_gxs_ubo_valid_v, vgl_gxs_ubo_valid_f; // (slots with a recorded buffer: uniform buffer objects; GXS_VUBO / GXS_FUBO use val[] as an 8-slot table)

// True when the call must be forwarded (and records its arguments as the new current ones), false when it would set what the context already has.
static inline __attribute__((always_inline)) int vgl_gxs_need(SceGxmContext *ctx, int kind, const uint32_t *a, int n) {
	vgl_gxs_calls[kind]++;
	if (!(vgl_gxs_mask & vgl_gxs_group[kind]) || ctx != gxm_context)
		return 1;
	uint32_t *c = vgl_gxs_val[kind];
	const uint32_t bit = 1u << kind;
	if (vgl_gxs_valid & bit) {
		int same = 1;
		for (int i = 0; i < n; i++)
			same &= (c[i] == a[i]);
		if (same) {
			vgl_gxs_skipped[kind]++;
			return 0;
		}
	}
	for (int i = 0; i < n; i++)
		c[i] = a[i];
	vgl_gxs_valid |= bit;
	return 1;
}

#define GXS_ARGS1(a) (const uint32_t[1]){ (uint32_t)(a) }
static inline __attribute__((always_inline)) void vgl_gxs_FrontDepthFunc(SceGxmContext *ctx, SceGxmDepthFunc f) {
	if (vgl_gxs_need(ctx, GXS_FDEPTHFUNC, GXS_ARGS1(f), 1)) sceGxmSetFrontDepthFunc(ctx, f);
}
static inline __attribute__((always_inline)) void vgl_gxs_BackDepthFunc(SceGxmContext *ctx, SceGxmDepthFunc f) {
	if (vgl_gxs_need(ctx, GXS_BDEPTHFUNC, GXS_ARGS1(f), 1)) sceGxmSetBackDepthFunc(ctx, f);
}
static inline __attribute__((always_inline)) void vgl_gxs_FrontDepthWrite(SceGxmContext *ctx, SceGxmDepthWriteMode m) {
	if (vgl_gxs_need(ctx, GXS_FDEPTHWRITE, GXS_ARGS1(m), 1)) sceGxmSetFrontDepthWriteEnable(ctx, m);
}
static inline __attribute__((always_inline)) void vgl_gxs_BackDepthWrite(SceGxmContext *ctx, SceGxmDepthWriteMode m) {
	if (vgl_gxs_need(ctx, GXS_BDEPTHWRITE, GXS_ARGS1(m), 1)) sceGxmSetBackDepthWriteEnable(ctx, m);
}
static inline __attribute__((always_inline)) void vgl_gxs_FrontStencilFunc(SceGxmContext *ctx, SceGxmStencilFunc f, SceGxmStencilOp o1, SceGxmStencilOp o2, SceGxmStencilOp o3, unsigned char cm, unsigned char wm) {
	const uint32_t a[6] = { f, o1, o2, o3, cm, wm };
	if (vgl_gxs_need(ctx, GXS_FSTENCILFUNC, a, 6)) sceGxmSetFrontStencilFunc(ctx, f, o1, o2, o3, cm, wm);
}
static inline __attribute__((always_inline)) void vgl_gxs_BackStencilFunc(SceGxmContext *ctx, SceGxmStencilFunc f, SceGxmStencilOp o1, SceGxmStencilOp o2, SceGxmStencilOp o3, unsigned char cm, unsigned char wm) {
	const uint32_t a[6] = { f, o1, o2, o3, cm, wm };
	if (vgl_gxs_need(ctx, GXS_BSTENCILFUNC, a, 6)) sceGxmSetBackStencilFunc(ctx, f, o1, o2, o3, cm, wm);
}
static inline __attribute__((always_inline)) void vgl_gxs_FrontStencilRef(SceGxmContext *ctx, unsigned int r) {
	if (vgl_gxs_need(ctx, GXS_FSTENCILREF, GXS_ARGS1(r), 1)) sceGxmSetFrontStencilRef(ctx, r);
}
static inline __attribute__((always_inline)) void vgl_gxs_BackStencilRef(SceGxmContext *ctx, unsigned int r) {
	if (vgl_gxs_need(ctx, GXS_BSTENCILREF, GXS_ARGS1(r), 1)) sceGxmSetBackStencilRef(ctx, r);
}
static inline __attribute__((always_inline)) void vgl_gxs_FrontPolygonMode(SceGxmContext *ctx, SceGxmPolygonMode m) {
	if (vgl_gxs_need(ctx, GXS_FPOLYMODE, GXS_ARGS1(m), 1)) sceGxmSetFrontPolygonMode(ctx, m);
}
static inline __attribute__((always_inline)) void vgl_gxs_BackPolygonMode(SceGxmContext *ctx, SceGxmPolygonMode m) {
	if (vgl_gxs_need(ctx, GXS_BPOLYMODE, GXS_ARGS1(m), 1)) sceGxmSetBackPolygonMode(ctx, m);
}
static inline __attribute__((always_inline)) void vgl_gxs_CullMode(SceGxmContext *ctx, SceGxmCullMode m) {
	if (vgl_gxs_need(ctx, GXS_CULL, GXS_ARGS1(m), 1)) sceGxmSetCullMode(ctx, m);
}
static inline __attribute__((always_inline)) void vgl_gxs_TwoSided(SceGxmContext *ctx, SceGxmTwoSidedMode m) {
	if (vgl_gxs_need(ctx, GXS_TWOSIDED, GXS_ARGS1(m), 1)) sceGxmSetTwoSidedEnable(ctx, m);
}
static inline __attribute__((always_inline)) void vgl_gxs_FrontDepthBias(SceGxmContext *ctx, int factor, int units) {
	const uint32_t a[2] = { (uint32_t)factor, (uint32_t)units };
	if (vgl_gxs_need(ctx, GXS_FDEPTHBIAS, a, 2)) sceGxmSetFrontDepthBias(ctx, factor, units);
}
static inline __attribute__((always_inline)) void vgl_gxs_BackDepthBias(SceGxmContext *ctx, int factor, int units) {
	const uint32_t a[2] = { (uint32_t)factor, (uint32_t)units };
	if (vgl_gxs_need(ctx, GXS_BDEPTHBIAS, a, 2)) sceGxmSetBackDepthBias(ctx, factor, units);
}
static inline __attribute__((always_inline)) void vgl_gxs_Viewport(SceGxmContext *ctx, float xo, float xs, float yo, float ys, float zo, float zs) {
	const float f[6] = { xo, xs, yo, ys, zo, zs };
	uint32_t a[6];
	__builtin_memcpy(a, f, sizeof(a));
	if (vgl_gxs_need(ctx, GXS_VIEWPORT, a, 6)) sceGxmSetViewport(ctx, xo, xs, yo, ys, zo, zs);
}
static inline __attribute__((always_inline)) void vgl_gxs_RegionClip(SceGxmContext *ctx, SceGxmRegionClipMode m, unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1) {
	const uint32_t a[5] = { m, x0, y0, x1, y1 };
	if (vgl_gxs_need(ctx, GXS_REGIONCLIP, a, 5)) sceGxmSetRegionClip(ctx, m, x0, y0, x1, y1);
}
static inline __attribute__((always_inline)) int vgl_gxs_VertexTexture(SceGxmContext *ctx, unsigned int idx, const SceGxmTexture *tex) {
	if (idx < 4) { // (the shaders of the games use unit 0; the kinds of units 4 and up do not exist)
		const uint32_t *w = (const uint32_t *)tex;
		if (!vgl_gxs_need(ctx, GXS_VTEX0 + idx, w, 4))
			return 0;
	}
	return sceGxmSetVertexTexture(ctx, idx, tex);
}
static inline __attribute__((always_inline)) int vgl_gxs_VertexDefaultUBuf(SceGxmContext *ctx, const void *b) {
	if (!vgl_gxs_need(ctx, GXS_VDEFUBUF, GXS_ARGS1((uintptr_t)b), 1))
		return 0;
	return sceGxmSetVertexDefaultUniformBuffer(ctx, b);
}
static inline __attribute__((always_inline)) int vgl_gxs_FragmentDefaultUBuf(SceGxmContext *ctx, const void *b) {
	if (!vgl_gxs_need(ctx, GXS_FDEFUBUF, GXS_ARGS1((uintptr_t)b), 1))
		return 0;
	return sceGxmSetFragmentDefaultUniformBuffer(ctx, b);
}
// Uniform buffer objects: the buffer index is part of the key (eight slots, valid bit per slot); the kinds count the calls.
static inline __attribute__((always_inline)) int vgl_gxs_UBO(SceGxmContext *ctx, int frag, unsigned int idx, const void *b) {
	const int kind = frag ? GXS_FUBO : GXS_VUBO;
	uint32_t *valid = frag ? &vgl_gxs_ubo_valid_f : &vgl_gxs_ubo_valid_v;
	vgl_gxs_calls[kind]++;
	if ((vgl_gxs_mask & vgl_gxs_group[kind]) && ctx == gxm_context && idx < 8) {
		uint32_t *c = vgl_gxs_val[kind];
		if (!(vgl_gxs_valid & (1u << kind))) // (forgotten as a whole)
			*valid = 0;
		vgl_gxs_valid |= 1u << kind;
		if ((*valid & (1u << idx)) && c[idx] == (uint32_t)(uintptr_t)b) {
			vgl_gxs_skipped[kind]++;
			return 0;
		}
		c[idx] = (uint32_t)(uintptr_t)b;
		*valid |= 1u << idx;
	}
	return frag ? sceGxmSetFragmentUniformBuffer(ctx, idx, b) : sceGxmSetVertexUniformBuffer(ctx, idx, b);
}
static inline __attribute__((always_inline)) int vgl_gxs_VertexUBO(SceGxmContext *ctx, unsigned int idx, const void *b) { return vgl_gxs_UBO(ctx, 0, idx, b); }
static inline __attribute__((always_inline)) int vgl_gxs_FragmentUBO(SceGxmContext *ctx, unsigned int idx, const void *b) { return vgl_gxs_UBO(ctx, 1, idx, b); }

#define sceGxmSetFrontDepthFunc(ctx, f) vgl_gxs_FrontDepthFunc(ctx, f)
#define sceGxmSetBackDepthFunc(ctx, f) vgl_gxs_BackDepthFunc(ctx, f)
#define sceGxmSetFrontDepthWriteEnable(ctx, m) vgl_gxs_FrontDepthWrite(ctx, m)
#define sceGxmSetBackDepthWriteEnable(ctx, m) vgl_gxs_BackDepthWrite(ctx, m)
#define sceGxmSetFrontStencilFunc(ctx, ...) vgl_gxs_FrontStencilFunc(ctx, __VA_ARGS__)
#define sceGxmSetBackStencilFunc(ctx, ...) vgl_gxs_BackStencilFunc(ctx, __VA_ARGS__)
#define sceGxmSetFrontStencilRef(ctx, r) vgl_gxs_FrontStencilRef(ctx, r)
#define sceGxmSetBackStencilRef(ctx, r) vgl_gxs_BackStencilRef(ctx, r)
#define sceGxmSetFrontPolygonMode(ctx, m) vgl_gxs_FrontPolygonMode(ctx, m)
#define sceGxmSetBackPolygonMode(ctx, m) vgl_gxs_BackPolygonMode(ctx, m)
#define sceGxmSetCullMode(ctx, m) vgl_gxs_CullMode(ctx, m)
#define sceGxmSetTwoSidedEnable(ctx, m) vgl_gxs_TwoSided(ctx, m)
#define sceGxmSetFrontDepthBias(ctx, f, u) vgl_gxs_FrontDepthBias(ctx, f, u)
#define sceGxmSetBackDepthBias(ctx, f, u) vgl_gxs_BackDepthBias(ctx, f, u)
#define sceGxmSetViewport(ctx, ...) vgl_gxs_Viewport(ctx, __VA_ARGS__)
#define sceGxmSetRegionClip(ctx, ...) vgl_gxs_RegionClip(ctx, __VA_ARGS__)
#define sceGxmSetVertexTexture(ctx, i, t) vgl_gxs_VertexTexture(ctx, i, t)
#define sceGxmSetVertexDefaultUniformBuffer(ctx, b) vgl_gxs_VertexDefaultUBuf(ctx, b)
#define sceGxmSetFragmentDefaultUniformBuffer(ctx, b) vgl_gxs_FragmentDefaultUBuf(ctx, b)
#define sceGxmSetVertexUniformBuffer(ctx, i, b) vgl_gxs_VertexUBO(ctx, i, b)
#define sceGxmSetFragmentUniformBuffer(ctx, i, b) vgl_gxs_FragmentUBO(ctx, i, b)
// (a reserve binds a new default buffer itself: the cached pointer is no longer the current one)
#define sceGxmReserveVertexDefaultUniformBuffer(ctx, ...) ({ vgl_gxs_valid &= ~(1u << GXS_VDEFUBUF); sceGxmReserveVertexDefaultUniformBuffer(ctx, __VA_ARGS__); })
#define sceGxmReserveFragmentDefaultUniformBuffer(ctx, ...) ({ vgl_gxs_valid &= ~(1u << GXS_FDEFUBUF); sceGxmReserveFragmentDefaultUniformBuffer(ctx, __VA_ARGS__); })
#endif // GXM_STATE_CACHE

#endif
