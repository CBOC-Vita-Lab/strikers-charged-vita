// vitaGL 0057 / 0058: geom_fix.h (the console's text) against the allocation loop of gpu_alloc_mipmaps (the console's text) and a bounds-checked block.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX(a, b) (((a) < (b)) ? (b) : (a))
#include "geom_fix.h"
#include "prod_chain.inc"
static uint32_t rs = 777;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; if (bad < 12) { printf(__VA_ARGS__); printf("\n"); } } } while (0)

int main(void) {
	// 1. every power-of-two size, every texel size: the level count is the allocation loop's, the offsets are its jumps, every level fits its block; the level past the end is refused
	for (uint32_t w = 1; w <= 1024; w <<= 1)
		for (uint32_t h = 1; h <= 1024; h <<= 1)
			for (uint32_t bpp = 1; bpp <= 4; bpp <<= 1) {
				uint32_t jumps[16], size; int mips;
				prod_chain(w, h, bpp, &size, &mips, jumps);
				const uint32_t levels = vgl_mip_chain_levels(w, h);
				CHECK(levels == (uint32_t)mips + 1, "%ux%u levels %u, the allocation loop %d", w, h, levels, mips + 1);
				if (levels != (uint32_t)mips + 1) continue;
				uint8_t *block = malloc(size);   // exactly the allocated size: ASan sees a byte past it
				uint32_t sum = 0;
				for (uint32_t l = 0; l < levels; l++) {
					uint32_t off, stride;
					vgl_mip_level_geom(w, h, bpp, l, &off, &stride);
					CHECK(off == sum, "%ux%u bpp %u level %u starts %u, the jumps say %u", w, h, bpp, l, off, sum);
					const uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
					CHECK(stride == MAX(lw, 8) * bpp, "%ux%u level %u stride %u", w, h, l, stride);
					CHECK(vgl_mip_level_width(w, l) == lw, "%ux%u level %u width %u", w, h, l, vgl_mip_level_width(w, l));
					// glTexSubImage2D of the whole level: lh rows of lw texels at the stride
					if (off + (uint64_t)stride * (lh - 1) + lw * bpp <= size)
						for (uint32_t y = 0; y < lh; y++) memset(block + off + (uint64_t)y * stride, 0x5A, lw * bpp);
					else
						CHECK(0, "%ux%u bpp %u level %u writes past the block (%u + rows)", w, h, bpp, l, off);
					sum += jumps[l];
				}
				CHECK(sum == size, "%ux%u chain %u, block %u", w, h, sum, size);
				free(block);
			}
	// 2. the non-square cases the notes name: a full chain of 128x32 is 6 levels, level 6 and 7 (2x1, 1x1) have no room
	CHECK(vgl_mip_chain_levels(128, 32) == 6 && vgl_mip_chain_levels(32, 128) == 6 && vgl_mip_chain_levels(64, 64) == 7 && vgl_mip_chain_levels(1, 1) == 1 && vgl_mip_chain_levels(256, 1) == 1, "level counts");
	// 3. widths halve per level, from level 3 on as well (the old orig_w / (2 * level) gave 128/6 = 21 at level 3)
	CHECK(vgl_mip_level_width(128, 3) == 16 && vgl_mip_level_width(128, 4) == 8 && vgl_mip_level_width(100, 3) == 12 && vgl_mip_level_width(5, 9) == 1, "level widths");
	// 4. attribute offsets: whenever the test says they fit, the 16-bit field holds every enabled offset exactly; whenever one does not fit it says no
	for (int iter = 0; iter < 200000; iter++) {
		uint32_t offs[16]; uint8_t map[16];
		const uint32_t n = 1 + rnd() % 8, state = rnd() & 0xFFFF;
		const int big = rnd() % 3;
		for (int i = 0; i < 16; i++) offs[i] = big == 0 ? rnd() % 64 : big == 1 ? rnd() % 0x20000 : rnd() % 0x200000;
		for (uint32_t i = 0; i < n; i++) map[i] = rnd() % 16;
		int want = 1, exact = 1;
		for (uint32_t i = 0; i < n; i++) if (state & (1u << map[i])) { if (offs[map[i]] > 65535) want = 0; if ((uint16_t)offs[map[i]] != offs[map[i]]) exact = 0; }
		const int got = vgl_pack_offsets_fit(offs, state, map, n);
		CHECK(got == want, "fit %d want %d", got, want);
		CHECK(!got || exact, "fit says yes and a 16-bit field loses an offset");
	}
	printf(bad ? "FAIL %d\n" : "PASS\n", bad);
	return bad != 0;
}
