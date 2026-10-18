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
 */
#ifndef _GXM_CACHE_H_
#define _GXM_CACHE_H_

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

static inline __attribute__((always_inline)) void vgl_gxc_reset(void) {
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
	if (vgl_gxc_enabled) {
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
	if (vgl_gxc_enabled) {
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
	if (vgl_gxc_enabled && idx < SCE_GXM_MAX_VERTEX_STREAMS) {
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
	if (vgl_gxc_enabled && idx < SCE_GXM_MAX_TEXTURE_UNITS) {
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

#endif
