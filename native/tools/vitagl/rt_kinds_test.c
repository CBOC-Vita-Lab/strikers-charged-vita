/* vitaGL 0043 rt_kinds.h: the per-kind slot table. 16 kinds get slots in order of first use, the 17th and later go to "other"; a known kind always finds its slot.  Run: run_rt_kinds_test.sh */
#include <stdio.h>
#include <string.h>
#include "rt_kinds.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
	rt_kind_key tab[RT_KIND_SLOTS];
	memset(tab, 0, sizeof tab);
	for (uint32_t i = 0; i < 17; i++) { // 17 distinct sizes
		const uint32_t s = rt_kind_slot(tab, 100 + i * 16, 64 + i * 8, 0);
		CHECK(s == (i < RT_KIND_SLOTS ? i : (uint32_t)RT_KIND_SLOTS));
	}
	for (uint32_t i = 0; i < 17; i++) { // asked again: the same answer, nothing new claimed
		const uint32_t s = rt_kind_slot(tab, 100 + i * 16, 64 + i * 8, 0);
		CHECK(s == (i < RT_KIND_SLOTS ? i : (uint32_t)RT_KIND_SLOTS));
	}
	CHECK(rt_kind_slot(tab, 100, 64, 1) == RT_KIND_SLOTS); // the display flag is part of the kind: a new kind, and the table is full
	memset(tab, 0, sizeof tab);
	CHECK(rt_kind_slot(tab, 704, 400, 0) == 0);
	CHECK(rt_kind_slot(tab, 704, 400, 1) == 1);
	CHECK(rt_kind_slot(tab, 704, 400, 7) == 1); // any non-zero display value is one kind
	CHECK(rt_kind_slot(tab, 990, 1139, 0) == 2);
	CHECK(rt_kind_slot(tab, 704, 400, 0) == 0);
	CHECK(tab[2].w == 990 && tab[2].h == 1139 && tab[2].used);
	printf(fails ? "rt_kinds_test: %d FAILED\n" : "rt_kinds_test: ok\n", fails);
	return fails != 0;
}
