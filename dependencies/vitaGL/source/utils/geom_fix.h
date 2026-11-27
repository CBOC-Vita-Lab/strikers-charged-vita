/* 0058 / 0059: the arithmetic of two places where vitaGL's own text went wrong, with no GXM and no allocator in it, so that a host test
 * (renderer/tests/vitagl_geom_fix.c) runs the very text the console runs.
 *
 * Mip chains (textures.c _glTexSubImage2D, utils/gpu_utils.c gpu_alloc_mipmaps). The linear chain gpu_alloc_mipmaps lays out puts level j after the levels before
 * it, each MAX(w, 8) * h * bpp bytes (w and h rounded up to a power of two and halved per level), and stops where the SMALLER side reaches 1: a 128 x 32 texture has
 * 6 levels (128x32 .. 4x1), not the 8 of a square chain. A level past that has no room in the block.
 *
 * Interleaved attributes in one buffer (custom_shaders.c). A GXM vertex attribute's offset is 16 bits; an attribute's offset into a buffer (glVertexAttribPointer
 * with a buffer bound) is not. */
#ifndef VGL_GEOM_FIX_H
#define VGL_GEOM_FIX_H

#include <stdint.h>

#define VGL_GEOM_MAX(a, b) (((a) < (b)) ? (b) : (a))

/* Levels in the chain of a w x h texture (both already powers of two): halve both while the smaller side is above 1. Same count as the loop in gpu_alloc_mipmaps
 * (its `mips` plus the last level). */
static inline uint32_t vgl_mip_chain_levels(uint32_t w, uint32_t h) {
	uint32_t n = 1;
	while (w > 1 && h > 1) {
		w >>= 1;
		h >>= 1;
		n++;
	}
	return n;
}

/* Where level `level` (0 .. vgl_mip_chain_levels - 1) starts in the block, and its row stride, both in bytes. The stride is the one gpu_alloc_mipmaps writes the level
 * with (rows of the level's width rounded up to 8 texels). */
static inline void vgl_mip_level_geom(uint32_t po2_w, uint32_t po2_h, uint32_t bpp, uint32_t level, uint32_t *offset, uint32_t *stride) {
	uint32_t off = 0, w = po2_w, h = po2_h;
	for (uint32_t j = 0; j < level; j++) {
		off += VGL_GEOM_MAX(w, 8) * h * bpp;
		w >>= 1;
		h >>= 1;
	}
	*offset = off;
	*stride = VGL_GEOM_MAX(w, 8) * bpp; /* (w is a power of two: rounding up to 8 is MAX(w, 8)) */
}

/* Width of level `level` as GL sizes it: halved per level, never below 1. */
static inline uint32_t vgl_mip_level_width(uint32_t orig_w, uint32_t level) {
	const uint32_t w = level >= 31 ? 0 : orig_w >> level;
	return w ? w : 1;
}

/* One buffer, one vertex stream (the packed-VBO draw): each enabled attribute of `map` (n of them, `state` the enabled bits) is placed by a GXM attribute offset, which is
 * 16 bits, and the offset the draw has for it is its distance into the buffer (glVertexAttribPointer with a buffer bound), which is not. Returns 1 when every enabled
 * offset fits. Otherwise 0: the draw takes a stream of its own for each attribute (stream pointer = buffer + offset, attribute offset 0), which places every attribute
 * exactly where the offset says. */
static inline int vgl_pack_offsets_fit(const uint32_t *offs, uint32_t state, const uint8_t *map, uint32_t n) {
	for (uint32_t i = 0; i < n; i++)
		if ((state & (1u << map[i])) && offs[map[i]] > 0xFFFFu)
			return 0;
	return 1;
}

#endif
