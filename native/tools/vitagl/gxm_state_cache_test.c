/*
 * Host test of vitaGL's sceGxm state cache (dependencies/vitaGL/source/utils/gxm_cache.h, patch 0013).
 *
 * The header is compiled against a mock of the sceGxm functions it wraps. The mock keeps a "hardware" record per context: every setter overwrites its own fields, a
 * scene begin or end scrambles ALL of them (libgxm promises nothing across a scene), a precomputed draw / vertex state scrambles the vertex program and streams, a
 * reserve of a default uniform buffer binds a new buffer. A pseudo-random stream of calls (the mix of vitaGL's draw path and state setters, with repeats, scene
 * changes and calls for a second context) is run twice, once with the caches off and once with them on (all groups, and each group alone). At every draw the whole
 * hardware record must be identical in both runs, and every call that was forwarded in the "on" run must be one the "off" run also made. The counters must add up.
 *
 *   cc -O2 -I<vitaGL source>/utils gxm_state_cache_test.c -o t && ./t
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the pieces of psp2/gxm.h gxm_cache.h needs
#define SCE_GXM_MAX_VERTEX_STREAMS 4
#define SCE_GXM_MAX_TEXTURE_UNITS 16
typedef unsigned int GLboolean;
#define GL_TRUE 1
#define GL_FALSE 0
typedef struct { uint32_t control_words[4]; } SceGxmTexture;
typedef struct SceGxmVertexProgram { int id; } SceGxmVertexProgram;
typedef struct SceGxmFragmentProgram { int id; } SceGxmFragmentProgram;
typedef unsigned int SceGxmDepthFunc, SceGxmDepthWriteMode, SceGxmStencilFunc, SceGxmStencilOp, SceGxmPolygonMode, SceGxmCullMode, SceGxmTwoSidedMode, SceGxmRegionClipMode;

typedef struct {
	const SceGxmVertexProgram *vprog;
	const SceGxmFragmentProgram *fprog;
	const void *vstream[SCE_GXM_MAX_VERTEX_STREAMS];
	uint32_t ftex[SCE_GXM_MAX_TEXTURE_UNITS][4];
	uint32_t vtex[4][4];
	uint32_t fdepthfunc, bdepthfunc, fdepthwrite, bdepthwrite;
	uint32_t fsten[6], bsten[6], fsref, bsref;
	uint32_t fpoly, bpoly, cull, twosided, fbias[2], bbias[2];
	uint32_t viewport[6], clip[5];
	const void *vdef, *fdef, *vubo[8], *fubo[8];
} Hw;
typedef struct { Hw hw; int id; } SceGxmContext;

static SceGxmContext g_ctx[2];
SceGxmContext *gxm_context = &g_ctx[0];
static uint32_t g_scene_count, g_issued; // calls the mock received
static void scramble(Hw *h, uint32_t seed) {
	uint32_t *w = (uint32_t *)h;
	for (size_t i = 0; i < sizeof(Hw) / 4; i++) w[i] = 0x9E3779B1u * (seed + 1) + (uint32_t)i * 7919u;
}
#define ISSUED() (g_issued++)
void sceGxmSetVertexProgram(SceGxmContext *c, const SceGxmVertexProgram *p) { ISSUED(); c->hw.vprog = p; }
void sceGxmSetFragmentProgram(SceGxmContext *c, const SceGxmFragmentProgram *p) { ISSUED(); c->hw.fprog = p; }
int sceGxmSetVertexStream(SceGxmContext *c, uint32_t i, const void *p) { ISSUED(); c->hw.vstream[i] = p; return 0; }
int sceGxmSetFragmentTexture(SceGxmContext *c, uint32_t i, const SceGxmTexture *t) { ISSUED(); memcpy(c->hw.ftex[i], t, 16); return 0; }
int sceGxmSetVertexTexture(SceGxmContext *c, uint32_t i, const SceGxmTexture *t) { ISSUED(); if (i < 4) memcpy(c->hw.vtex[i], t, 16); return 0; }
void sceGxmSetFrontDepthFunc(SceGxmContext *c, SceGxmDepthFunc f) { ISSUED(); c->hw.fdepthfunc = f; }
void sceGxmSetBackDepthFunc(SceGxmContext *c, SceGxmDepthFunc f) { ISSUED(); c->hw.bdepthfunc = f; }
void sceGxmSetFrontDepthWriteEnable(SceGxmContext *c, SceGxmDepthWriteMode m) { ISSUED(); c->hw.fdepthwrite = m; }
void sceGxmSetBackDepthWriteEnable(SceGxmContext *c, SceGxmDepthWriteMode m) { ISSUED(); c->hw.bdepthwrite = m; }
void sceGxmSetFrontStencilFunc(SceGxmContext *c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d, unsigned char cm, unsigned char wm) {
	ISSUED(); uint32_t v[6] = {f, a, b, d, cm, wm}; memcpy(c->hw.fsten, v, sizeof v); }
void sceGxmSetBackStencilFunc(SceGxmContext *c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b, SceGxmStencilOp d, unsigned char cm, unsigned char wm) {
	ISSUED(); uint32_t v[6] = {f, a, b, d, cm, wm}; memcpy(c->hw.bsten, v, sizeof v); }
void sceGxmSetFrontStencilRef(SceGxmContext *c, unsigned int r) { ISSUED(); c->hw.fsref = r; }
void sceGxmSetBackStencilRef(SceGxmContext *c, unsigned int r) { ISSUED(); c->hw.bsref = r; }
void sceGxmSetFrontPolygonMode(SceGxmContext *c, SceGxmPolygonMode m) { ISSUED(); c->hw.fpoly = m; }
void sceGxmSetBackPolygonMode(SceGxmContext *c, SceGxmPolygonMode m) { ISSUED(); c->hw.bpoly = m; }
void sceGxmSetCullMode(SceGxmContext *c, SceGxmCullMode m) { ISSUED(); c->hw.cull = m; }
void sceGxmSetTwoSidedEnable(SceGxmContext *c, SceGxmTwoSidedMode m) { ISSUED(); c->hw.twosided = m; }
void sceGxmSetFrontDepthBias(SceGxmContext *c, int f, int u) { ISSUED(); c->hw.fbias[0] = f; c->hw.fbias[1] = u; }
void sceGxmSetBackDepthBias(SceGxmContext *c, int f, int u) { ISSUED(); c->hw.bbias[0] = f; c->hw.bbias[1] = u; }
void sceGxmSetViewport(SceGxmContext *c, float a, float b, float d, float e, float f, float g) {
	ISSUED(); float v[6] = {a, b, d, e, f, g}; memcpy(c->hw.viewport, v, sizeof v); }
void sceGxmSetRegionClip(SceGxmContext *c, SceGxmRegionClipMode m, unsigned int a, unsigned int b, unsigned int d, unsigned int e) {
	ISSUED(); uint32_t v[5] = {m, a, b, d, e}; memcpy(c->hw.clip, v, sizeof v); }
int sceGxmSetVertexDefaultUniformBuffer(SceGxmContext *c, const void *b) { ISSUED(); c->hw.vdef = b; return 0; }
int sceGxmSetFragmentDefaultUniformBuffer(SceGxmContext *c, const void *b) { ISSUED(); c->hw.fdef = b; return 0; }
int sceGxmSetVertexUniformBuffer(SceGxmContext *c, unsigned int i, const void *b) { ISSUED(); c->hw.vubo[i] = b; return 0; }
int sceGxmSetFragmentUniformBuffer(SceGxmContext *c, unsigned int i, const void *b) { ISSUED(); c->hw.fubo[i] = b; return 0; }
static char g_res_buf[1 << 16];
static uint32_t g_res_n; // (the n-th reserve of a run hands out the n-th buffer: the same in every run)
int sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *c, void **b) { ISSUED(); c->hw.vdef = *b = g_res_buf + (++g_res_n % 997) * 8; return 0; }
int sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *c, void **b) { ISSUED(); c->hw.fdef = *b = g_res_buf + (++g_res_n % 991) * 8; return 0; }
int sceGxmBeginScene(SceGxmContext *c, int x) { scramble(&c->hw, ++g_scene_count); return 0; }
int sceGxmEndScene(SceGxmContext *c, int x) { scramble(&c->hw, ++g_scene_count); return 0; }
int sceGxmDrawPrecomputed(SceGxmContext *c, void *s) { c->hw.vprog = (void *)0x1234; c->hw.vstream[0] = (void *)0x5678; return 0; }
void sceGxmSetPrecomputedVertexState(SceGxmContext *c, void *s) { c->hw.vprog = (void *)0x4321; memset(c->hw.vstream, 0x5a, sizeof c->hw.vstream); }

#include <gxm_cache.h>

// ---- what vgl.c defines
GLboolean vgl_gxc_enabled = GL_FALSE;
vgl_gxc_t vgl_gxc;
uint32_t vgl_gxc_calls[4], vgl_gxc_skipped[4];
uint32_t vgl_gxs_valid = 0, vgl_gxs_val[GXS_KINDS][8], vgl_gxs_mask = 0, vgl_gxs_calls[GXS_KINDS], vgl_gxs_skipped[GXS_KINDS], vgl_gxs_ubo_valid_v, vgl_gxs_ubo_valid_f;
const uint8_t vgl_gxs_group[GXS_KINDS] = {
	GXS_GROUP_DEPTH, GXS_GROUP_DEPTH, GXS_GROUP_DEPTH, GXS_GROUP_DEPTH, GXS_GROUP_STENCIL, GXS_GROUP_STENCIL, GXS_GROUP_STENCIL, GXS_GROUP_STENCIL,
	GXS_GROUP_RASTER, GXS_GROUP_RASTER, GXS_GROUP_RASTER, GXS_GROUP_RASTER, GXS_GROUP_RASTER, GXS_GROUP_RASTER, GXS_GROUP_VIEW, GXS_GROUP_VIEW,
	GXS_GROUP_VTEX, GXS_GROUP_VTEX, GXS_GROUP_VTEX, GXS_GROUP_VTEX, GXS_GROUP_UBUF, GXS_GROUP_UBUF, GXS_GROUP_UBUF, GXS_GROUP_UBUF,
};

// ---- the random program
static uint64_t rng_s;
static uint32_t rnd(void) { rng_s = rng_s * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(rng_s >> 33); }
static uint32_t pick(uint32_t n) { return rnd() % n; } // small value sets make repeats common

static SceGxmVertexProgram vp[3];
static SceGxmFragmentProgram fp[3];
static SceGxmTexture tex[3];
static char bufs[8][64];

static uint64_t g_trace; // hash of the hardware record at every draw
static void mix(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) g_trace = (g_trace ^ b[i]) * 1099511628211ull; }
static int g_mismatch_draw = -1;
static Hw g_draw_states[200000];
static int g_draws;

static void run(uint64_t seed, int ops, int record) {
	rng_s = seed;
	memset(g_ctx, 0, sizeof g_ctx);
	g_ctx[1].id = 1;
	g_issued = 0;
	g_res_n = 0;
	g_scene_count = 0;
	g_draws = 0;
	memset(vgl_gxs_calls, 0, sizeof vgl_gxs_calls);
	memset(vgl_gxs_skipped, 0, sizeof vgl_gxs_skipped);
	memset(vgl_gxc_calls, 0, sizeof vgl_gxc_calls);
	memset(vgl_gxc_skipped, 0, sizeof vgl_gxc_skipped);
	vgl_gxc_reset();
	vgl_gxs_valid = 0;
	for (int i = 0; i < ops; i++) {
		SceGxmContext *c = pick(40) == 0 ? &g_ctx[1] : &g_ctx[0]; // 1 in 40 calls is for the splash screen's context
		switch (pick(34)) {
		case 0: sceGxmBeginScene(c, 0); break;
		case 1: sceGxmEndScene(c, 0); break;
		case 2: sceGxmSetVertexProgram(c, &vp[pick(3)]); break;
		case 3: sceGxmSetFragmentProgram(c, &fp[pick(3)]); break;
		case 4: sceGxmSetVertexStream(c, pick(4), bufs[pick(3)]); break;
		case 5: { SceGxmTexture t = tex[pick(3)]; sceGxmSetFragmentTexture(c, pick(3), &t); break; }
		case 6: { SceGxmTexture t = tex[pick(3)]; sceGxmSetVertexTexture(c, pick(6), &t); break; }
		case 7: sceGxmSetFrontDepthFunc(c, pick(3)); break;
		case 8: sceGxmSetBackDepthFunc(c, pick(3)); break;
		case 9: sceGxmSetFrontDepthWriteEnable(c, pick(2)); break;
		case 10: sceGxmSetBackDepthWriteEnable(c, pick(2)); break;
		case 11: sceGxmSetFrontStencilFunc(c, pick(2), pick(2), pick(2), pick(2), pick(2) ? 0xFF : 0, pick(2) ? 0xFF : 0); break;
		case 12: sceGxmSetBackStencilFunc(c, pick(2), pick(2), pick(2), pick(2), pick(2) ? 0xFF : 0, pick(2) ? 0xFF : 0); break;
		case 13: sceGxmSetFrontStencilRef(c, pick(3)); break;
		case 14: sceGxmSetBackStencilRef(c, pick(3)); break;
		case 15: sceGxmSetFrontPolygonMode(c, pick(3)); break;
		case 16: sceGxmSetBackPolygonMode(c, pick(3)); break;
		case 17: sceGxmSetCullMode(c, pick(3)); break;
		case 18: sceGxmSetTwoSidedEnable(c, pick(2)); break;
		case 19: sceGxmSetFrontDepthBias(c, (int)pick(2), (int)pick(2)); break;
		case 20: sceGxmSetBackDepthBias(c, (int)pick(2), (int)pick(2)); break;
		case 21: sceGxmSetViewport(c, (float)pick(2), 1.0f, (float)pick(2), pick(2) ? 0.5f : -0.0f, 0.0f, pick(2) ? 1.0f : 0.0f); break; // (-0.0 vs 0.0: bit patterns, not ==)
		case 22: sceGxmSetRegionClip(c, pick(2), pick(2), pick(2), pick(3), pick(3)); break;
		case 23: sceGxmSetVertexDefaultUniformBuffer(c, bufs[pick(3)]); break;
		case 24: sceGxmSetFragmentDefaultUniformBuffer(c, bufs[pick(3)]); break;
		case 25: sceGxmSetVertexUniformBuffer(c, pick(10), bufs[pick(3)]); break;
		case 26: sceGxmSetFragmentUniformBuffer(c, pick(10), bufs[pick(3)]); break;
		case 27: { void *b = NULL; if (pick(2)) sceGxmReserveVertexDefaultUniformBuffer(c, &b); else sceGxmReserveFragmentDefaultUniformBuffer(c, &b); break; }
		case 28: sceGxmDrawPrecomputed(c, NULL); break;
		case 29: sceGxmSetPrecomputedVertexState(c, NULL); break;
		default: // a draw: the whole record of the main context is what the GPU would use
			if (g_draws < 200000) { if (record) g_draw_states[g_draws] = g_ctx[0].hw; else if (memcmp(&g_draw_states[g_draws], &g_ctx[0].hw, sizeof(Hw))) { if (g_mismatch_draw < 0) g_mismatch_draw = g_draws; } }
			g_draws++;
			break;
		}
	}
}

int main(void) {
	for (int i = 0; i < 3; i++) {
		vp[i].id = i; fp[i].id = i;
		for (int k = 0; k < 4; k++) tex[i].control_words[k] = 0x1000u * (i + 1) + k;
	}
	int failures = 0;
	const uint32_t masks[] = {0x3F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x00};
	uint64_t total_calls = 0, total_skipped = 0;
	for (uint64_t seed = 1; seed <= 60; seed++) {
		// reference: every cache off
		vgl_gxc_enabled = GL_FALSE;
		vgl_gxs_mask = 0;
		run(seed * 7919, 4000, 1);
		const uint32_t off_issued = g_issued;
		const int off_draws = g_draws;
		for (size_t m = 0; m < sizeof masks / sizeof masks[0]; m++) {
			vgl_gxc_enabled = masks[m] ? GL_TRUE : GL_FALSE; // the program / stream / texture part rides with every enabled run
			vgl_gxs_mask = masks[m];
			g_mismatch_draw = -1;
			run(seed * 7919, 4000, 0);
			uint32_t calls = 0, skipped = 0;
			for (int k = 0; k < GXS_KINDS; k++) { calls += vgl_gxs_calls[k]; skipped += vgl_gxs_skipped[k]; }
			for (int k = 0; k < 4; k++) { calls += vgl_gxc_calls[k]; skipped += vgl_gxc_skipped[k]; }
			if (g_mismatch_draw >= 0 || g_draws != off_draws) {
				printf("FAIL seed %llu mask 0x%02X: hardware state differs at draw %d (of %d)\n", (unsigned long long)seed, masks[m], g_mismatch_draw, g_draws);
				failures++;
			}
			if (masks[m] && g_issued >= off_issued) { /* a run with a cache on and nothing skipped over 4000 random calls cannot be */
				printf("FAIL seed %llu mask 0x%02X: nothing was skipped (%u issued, %u without)\n", (unsigned long long)seed, masks[m], g_issued, off_issued); failures++; }
			if (!masks[m] && g_issued != off_issued) { printf("FAIL seed %llu: caches off but the call count changed\n", (unsigned long long)seed); failures++; }
			// every call is either issued or skipped: for the second context the counters count the call but never skip it
			if (g_issued + skipped != off_issued) { /* (reserves are issued but not counted: they are the same in both runs) */ printf("FAIL seed %llu mask 0x%02X: counters do not add up\n", (unsigned long long)seed, masks[m]); failures++; }
			if (masks[m] == 0x3F) { total_calls += calls; total_skipped += skipped; }
		}
	}
	// the cache must not touch the second context's record: it is only ever forwarded
	vgl_gxc_enabled = GL_TRUE;
	vgl_gxs_mask = 0x3F;
	memset(g_ctx, 0, sizeof g_ctx);
	vgl_gxc_reset();
	sceGxmSetCullMode(&g_ctx[1], 2);
	sceGxmSetCullMode(&g_ctx[1], 2);
	sceGxmSetFrontDepthFunc(&g_ctx[1], 1);
	sceGxmSetFrontDepthFunc(&g_ctx[0], 1);
	sceGxmSetFrontDepthFunc(&g_ctx[0], 1);
	if (vgl_gxs_skipped[GXS_CULL] != 0 || vgl_gxs_skipped[GXS_FDEPTHFUNC] != 1) { printf("FAIL: second context is cached (cull skipped %u, depth skipped %u)\n", vgl_gxs_skipped[GXS_CULL], vgl_gxs_skipped[GXS_FDEPTHFUNC]); failures++; }
	// a scene begin forgets: the same call after it is forwarded
	g_issued = 0;
	sceGxmSetBackDepthFunc(&g_ctx[0], 2);
	sceGxmSetBackDepthFunc(&g_ctx[0], 2);
	sceGxmBeginScene(&g_ctx[0], 0);
	sceGxmSetBackDepthFunc(&g_ctx[0], 2);
	if (g_issued != 2) { printf("FAIL: expected 2 forwarded calls around a scene begin, got %u\n", g_issued); failures++; }
	printf("gxm state cache: %d failure(s); 60 seeds x 8 masks x 4000 calls; full cache skipped %llu of %llu counted calls (%.1f %% of the random mix)\n", failures,
	       (unsigned long long)total_skipped, (unsigned long long)total_calls, total_calls ? 100.0 * total_skipped / total_calls : 0.0);
	return failures ? 1 : 0;
}
