// vitaGL 0047 / 0048: vgl_fast_memcmp (via vertex_program_is_mru's compare) and vgl_uniform_copy, taken from gxm_utils.c, against memcmp / memcpy.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char GLboolean;
#define GL_FALSE 0
#define GL_TRUE 1
#define vgl_fast_memcpy memcpy
#define sceClibMemcmp memcmp
#include "fixture.inc"
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
int main(void) {
	int bad = 0;
	for (int iter = 0; iter < 20000; iter++) {
		uint8_t a[160] __attribute__((aligned(4))), b[160] __attribute__((aligned(4))), d[160], e[160];
		for (int i = 0; i < 160; i++) { a[i] = rnd(); b[i] = a[i]; }
		uint32_t size = rnd() % 100;
		uint32_t off = (rnd() & 1) ? 0 : 4 * (rnd() % 4);   // vertex program attribute/stream arrays are word aligned
		if (rnd() & 1) b[off + (size ? rnd() % size : 0)] ^= 1u << (rnd() & 7);
		int want = memcmp(a + off, b + off, size) == 0;
		int got = vgl_fast_memcmp(a + off, b + off, size) ? 1 : 0;
		if (want != got) { bad++; if (bad < 5) printf("memcmp size %u off %u want %d got %d\n", size, off, want, got); }
		uint32_t so = rnd() % 4, dof = rnd() % 4;
		memset(d, 0xA5, sizeof d); memset(e, 0xA5, sizeof e);
		memcpy(e + dof, a + so, size); vgl_uniform_copy(d + dof, a + so, size);
		if (memcmp(d, e, sizeof d)) { bad++; if (bad < 5) printf("copy size %u mismatch\n", size); }
	}
	// every size 0..130 on a changed last byte
	for (uint32_t size = 1; size < 130; size++) {
		uint8_t a[160] __attribute__((aligned(4))), b[160] __attribute__((aligned(4)));
		memset(a, 7, sizeof a); memcpy(b, a, sizeof a); b[size - 1] = 8;
		if (vgl_fast_memcmp(a, b, size)) { bad++; printf("last byte size %u not seen\n", size); }
		if (!vgl_fast_memcmp(a, a, size)) { bad++; printf("equal size %u refused\n", size); }
	}
	printf(bad ? "FAIL %d\n" : "PASS\n", bad);
	return bad != 0;
}
