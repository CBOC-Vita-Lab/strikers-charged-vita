/* 0044 (MSC_VGX_LADDER_ALIAS): the bookkeeping of the shared depth blocks, with no GXM and no allocator in it so that a host test (renderer/tests/depthshare.cpp)
 * runs the very code the console runs. mem_utils.c holds the table, the memory and the surface copies.
 *
 * A block is made for a (multisample mode, capacity w x h) the first time an object asks for it and is freed with its last user. refs counts the users;
 * last_fb is the user whose scene began last, to see a change of user: it is the identity of the drawing context and is valid only between two changes of
 * the users (A2: framebuffer objects are slots of a static array, a deleted object's slot is reused by a new one of another size, and an object that is
 * resized gives up its depth and joins again; "the same address as last time" must not be read as "the same context"). So every join and every leave clears it. */
#ifndef VGL_DEPTH_SHARE_CORE_H
#define VGL_DEPTH_SHARE_CORE_H

#define VGL_DSHARE_MAX 8

typedef struct {
	void *mem; /* the block (NULL: free slot) */
	int refs; /* objects whose depth surface lives in it */
	const void *last_fb; /* the object whose scene began last (NULL: none since the last join or leave) */
	int mode; /* the multisample mode, as the caller numbers it */
	int cw, ch; /* the capacity: the stride is cw samples, ch rows */
} vgl_dshare_rec;

typedef struct {
	vgl_dshare_rec rec[VGL_DSHARE_MAX];
	unsigned made, joined, failed;
} vgl_dshare_tbl;

static inline int vgl_ds_find(const vgl_dshare_tbl *t, int mode, int cw, int ch) {
	for (int i = 0; i < VGL_DSHARE_MAX; i++)
		if (t->rec[i].mem && t->rec[i].mode == mode && t->rec[i].cw == cw && t->rec[i].ch == ch)
			return i;
	return -1;
}
static inline int vgl_ds_free_slot(const vgl_dshare_tbl *t) {
	for (int i = 0; i < VGL_DSHARE_MAX; i++)
		if (!t->rec[i].mem)
			return i;
	return -1;
}
/* a new block in slot i (no users yet) */
static inline void vgl_ds_add(vgl_dshare_tbl *t, int i, void *mem, int mode, int cw, int ch) {
	t->rec[i].mem = mem;
	t->rec[i].refs = 0;
	t->rec[i].last_fb = 0;
	t->rec[i].mode = mode;
	t->rec[i].cw = cw;
	t->rec[i].ch = ch;
	t->made++;
}
/* an object joins the block of slot i */
static inline void vgl_ds_join(vgl_dshare_tbl *t, int i) {
	t->rec[i].refs++;
	t->rec[i].last_fb = 0; /* A2 */
	t->joined++;
}
/* a depth surface at `mem` is released. 1: it was the last user of a shared block (the slot is free again; the caller frees the memory); 0: other users remain;
 * -1: not a shared block (the caller frees it as before) */
static inline int vgl_ds_release(vgl_dshare_tbl *t, const void *mem) {
	for (int i = 0; i < VGL_DSHARE_MAX; i++) {
		if (t->rec[i].mem && t->rec[i].mem == mem) {
			t->rec[i].last_fb = 0; /* A2 */
			if (--t->rec[i].refs > 0)
				return 0;
			t->rec[i].mem = 0;
			t->rec[i].refs = 0;
			return 1;
		}
	}
	return -1;
}
/* a scene of object `fb` begins on `mem`. 1: another context drew in the block last (or none is known): what is recorded about the content (the mask-keep record) is not the new
 * scene's; 0: the same context as last time, or not a shared block */
static inline int vgl_ds_scene(vgl_dshare_tbl *t, const void *mem, const void *fb) {
	for (int i = 0; i < VGL_DSHARE_MAX; i++) {
		if (t->rec[i].mem && t->rec[i].mem == mem) {
			if (t->rec[i].last_fb != fb) {
				t->rec[i].last_fb = fb;
				return 1;
			}
			return 0;
		}
	}
	return 0;
}
#endif
