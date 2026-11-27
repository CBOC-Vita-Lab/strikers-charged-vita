/*
 * rt_kinds.h (vitaGL 0043, Strikers 1.3): the table that keeps GPU time per kind of scene. A kind is the size of the target the scene drew to and whether it
 * was the display. The table has RT_KIND_SLOTS slots, filled in order of first use; a scene whose kind finds no free slot goes to the extra "other" entry
 * (index RT_KIND_SLOTS). Plain C with no vitaGL or sceGxm types, so that a host test can compile it (native/tools/vitagl/rt_kinds_test.c).
 */
#ifndef RT_KINDS_H
#define RT_KINDS_H
#include <stdint.h>

#define RT_KIND_SLOTS 16

typedef struct {
	uint16_t w, h;
	uint8_t display;
	uint8_t used;
} rt_kind_key;

// The slot of (w, h, display) in tab[RT_KIND_SLOTS], claiming a free one when the kind is new; RT_KIND_SLOTS (= "other") when all are taken by other kinds.
static inline uint32_t rt_kind_slot(rt_kind_key *tab, uint32_t w, uint32_t h, uint32_t display) {
	const uint16_t kw = (uint16_t)w, kh = (uint16_t)h;
	const uint8_t kd = display ? 1 : 0;
	for (uint32_t i = 0; i < RT_KIND_SLOTS; i++) {
		if (!tab[i].used) {
			tab[i].w = kw;
			tab[i].h = kh;
			tab[i].display = kd;
			tab[i].used = 1;
			return i;
		}
		if (tab[i].w == kw && tab[i].h == kh && tab[i].display == kd)
			return i;
	}
	return RT_KIND_SLOTS;
}
#endif
