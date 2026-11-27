/*
 * wrcopy.c (0045, MSC_VITA_WRCOPY): copies of frame data INTO the uncached pools (the stream buffers, the uniform ring).
 *
 * The circular pool and the uniform ring live in uncached RAM (SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE). The CPU's writes there are not merged into bursts by
 * sceClibMemcpy's word copy (0.25 GB/s measured on 5 Oct: 3.9 ms per MB; a Vice open-play picture writes ~1.4 MB: 1.05 MB of stream and ~0.36 MB of ring). Whole 64 byte
 * stores from NEON (two 32 byte aligned vst1, or one 128 byte vstm) leave the write buffer as full lines. The bytes written are the same either way.
 *
 * vglSetWriteCopy(mode): 0 sceClibMemcpy (default), 1 two aligned 32 byte NEON stores per 64 bytes, 2 one 16-register vstm per 128 bytes,
 * 3 sceDmacMemcpy for copies of 8 KiB or more between user addresses (what upstream's vgl_memcpy does for texture data; the DMA engine writes the uncached RAM itself).
 * vglWriteCopyVariant(mode, dst, src, n): one copy by that variant (the start-up benchmark and its verification, in the game's platform layer).
 * Copies under VGL_WRCOPY_MIN bytes keep sceClibMemcpy (the call is cheaper than the set-up). The destination's head is copied to a 64 byte boundary by sceClibMemcpy.
 */
#include <stdint.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/processmgr.h>
#include "wrcopy.h"
#ifndef vgl_fast_memcpy
#define vgl_fast_memcpy sceClibMemcpy // (as utils/mem_utils.h without DISABLE_CLIB)
#endif

#define VGL_WRCOPY_MIN 192u
#ifndef VGL_WR_ADDR
#define VGL_WR_ADDR(p) ((uint32_t)(uintptr_t)(p)) // (the host test of 0061 replaces it by a fake address map)
#endif

static volatile int vgl_wr_mode = 0;
uint32_t vgl_wr_stat[8]; // [0..3] copies and bytes made by a NEON variant, copies and bytes made wholly by sceClibMemcpy (under 192 bytes, mode 0, the FB=0 fallback; for the log); 0061: [4] copies and [5] bytes by the DMA engine, [6] copies of 8 KiB+ that were not given to it
                         // (an address at or above 0x81000000: user memory, the C heap, where every std::vector lives), [7] DMA calls that returned an error

static inline void wr_neon64(uint8_t *d, const uint8_t *s, uint32_t blocks) {
	__asm__ volatile(
		"1:\n\t"
		"pld [%1, #256]\n\t"
		"vld1.8 {d0-d3}, [%1]!\n\t"
		"vld1.8 {d4-d7}, [%1]!\n\t"
		"subs %2, %2, #1\n\t"
		"vst1.64 {d0-d3}, [%0:256]!\n\t"
		"vst1.64 {d4-d7}, [%0:256]!\n\t"
		"bne 1b\n\t"
		: "+r"(d), "+r"(s), "+r"(blocks)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "memory", "cc");
}

static inline void wr_vstm128(uint8_t *d, const uint8_t *s, uint32_t blocks) {
	__asm__ volatile(
		"1:\n\t"
		"pld [%1, #256]\n\t"
		"vld1.8 {d0-d3}, [%1]!\n\t"
		"vld1.8 {d4-d7}, [%1]!\n\t"
		"vld1.8 {d8-d11}, [%1]!\n\t"
		"vld1.8 {d12-d15}, [%1]!\n\t"
		"subs %2, %2, #1\n\t"
		"vstmia %0!, {d0-d15}\n\t"
		"bne 1b\n\t"
		: "+r"(d), "+r"(s), "+r"(blocks)
		:
		: "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15", "memory", "cc");
}

// 0061: what mode 3 does with a copy the DMA engine does not take (under 8 KiB, an address at or above 0x81000000, or a DMA error). 0045 handed it to sceClibMemcpy, which writes the uncached
// pools at 0.34-0.41 GB/s (768 byte copies 408-410 MB/s on the console, 6121 / 6122) against 0.60-0.78 GB/s for the 64 byte NEON stores of mode 1. 1 (default): mode 1's copy. 0: sceClibMemcpy as 0045.
// The bytes written are the same either way. DMA stays for 8 KiB and more between two addresses below 0x81000000.
static volatile int vgl_wr_fb = 1;
void vglSetWriteCopyFallback(int on) {
	vgl_wr_fb = on ? 1 : 0;
}
int vglGetWriteCopyFallback(void) {
	return vgl_wr_fb;
}

void vglWriteCopyVariant(int mode, void *dst, const void *src, uint32_t n) {
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	if (mode == 3) {
		if (n >= 0x2000u) {
			if (VGL_WR_ADDR(s) < 0x81000000u && VGL_WR_ADDR(d) < 0x81000000u && n <= 0x81000000u - VGL_WR_ADDR(s) && n <= 0x81000000u - VGL_WR_ADDR(d)) {
				if (sceDmacMemcpy(d, s, n) >= 0) {
					vgl_wr_stat[4]++;
					vgl_wr_stat[5] += n;
					return;
				}
				vgl_wr_stat[7]++;
			} else
				vgl_wr_stat[6]++;
		}
		if (!vgl_wr_fb) {
			vgl_wr_stat[2]++;
			vgl_wr_stat[3] += n;
			sceClibMemcpy(d, s, n);
			return;
		}
		mode = 1;
	}
	if (mode != 1 && mode != 2) {
		vgl_wr_stat[2]++;
		vgl_wr_stat[3] += n;
		sceClibMemcpy(d, s, n);
		return;
	}
	const uint32_t unit = mode == 1 ? 64u : 128u;
	uint32_t head = (uint32_t)(-(intptr_t)d) & 63u;
	if (n < head + unit) {
		vgl_wr_stat[2]++;
		vgl_wr_stat[3] += n;
		sceClibMemcpy(d, s, n);
		return;
	}
	vgl_wr_stat[0]++; // 0061: counted where the copy is made: a copy whose bulk is NEON stores is [0]/[1], a copy made wholly by sceClibMemcpy [2]/[3], by the DMA engine [4]/[5]
	vgl_wr_stat[1] += n;
	if (head) {
		sceClibMemcpy(d, s, head);
		d += head;
		s += head;
		n -= head;
	}
	const uint32_t blocks = n >> (mode == 1 ? 6 : 7);
	if (mode == 1)
		wr_neon64(d, s, blocks);
	else
		wr_vstm128(d, s, blocks);
	d += blocks * unit;
	s += blocks * unit;
	n -= blocks * unit;
	if (n)
		sceClibMemcpy(d, s, n);
}

void vglSetWriteCopy(int mode) {
	vgl_wr_mode = (mode >= 1 && mode <= 3) ? mode : 0;
}

int vglGetWriteCopy(void) {
	return vgl_wr_mode;
}

void vglWriteCopyStats(uint32_t *out4) {
	for (int i = 0; i < 4; i++)
		out4[i] = vgl_wr_stat[i];
}
void vglWriteCopyStats8(uint32_t *out8) { // 0061: the four above and the DMA counters
	for (int i = 0; i < 8; i++)
		out8[i] = vgl_wr_stat[i];
}

// 0051 (MSC_VITA_TEXCOPY): the contiguous level-0 copy of a texture upload (gpu_alloc_texture, glTexSubImage2D). Mode 0 is exactly what these two sites did before (vgl_fast_memcpy).
// The destination is GPU-mapped video memory, where the CPU's stores run at about a third of the DMA engine's speed (339-363 against 866-935 MB/s, 17 Nov console, 8 KiB+).
static volatile int vgl_tex_mode = 0;
static uint32_t vgl_tex_stat[6]; // dma copies, dma bytes, dma us, cpu copies, differences (mode 2), refusals
void vglSetTextureCopyDma(int mode) {
	vgl_tex_mode = (mode == 1 || mode == 2) ? mode : 0;
}
void vglTextureCopyStats(uint32_t *out6) {
	for (int i = 0; i < 6; i++)
		out6[i] = vgl_tex_stat[i];
}
void vgl_tex_copy(void *dst, const void *src, uint32_t n) {
	const int m = vgl_tex_mode;
	if (m == 0) {
		vgl_fast_memcpy(dst, src, n);
		return;
	}
	if (n >= 0x2000u && VGL_WR_ADDR(src) < 0x81000000u && VGL_WR_ADDR(dst) < 0x81000000u && n <= 0x81000000u - VGL_WR_ADDR(src) && n <= 0x81000000u - VGL_WR_ADDR(dst)) {
		const uint64_t t0 = sceKernelGetProcessTimeWide();
		if (sceDmacMemcpy(dst, src, n) >= 0) {
			vgl_tex_stat[0]++;
			vgl_tex_stat[1] += n;
			vgl_tex_stat[2] += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
			if (m == 2 && memcmp(dst, src, n) != 0) {
				vgl_tex_stat[4]++;
				sceClibMemcpy(dst, src, n);
			}
			return;
		}
		vgl_tex_stat[5]++;
	}
	vgl_tex_stat[3]++;
	vgl_fast_memcpy(dst, src, n);
}

void vgl_wr_copy(void *dst, const void *src, uint32_t n) {
	const int m = vgl_wr_mode;
	if (m == 0 || n < VGL_WRCOPY_MIN) {
		vgl_wr_stat[2]++;
		vgl_wr_stat[3] += n;
		sceClibMemcpy(dst, src, n);
		return;
	}
	vglWriteCopyVariant(m, dst, src, n); // (0061: the variant counts the copy by the branch it takes)
}
