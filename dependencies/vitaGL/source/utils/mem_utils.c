/*
 * This file is part of vitaGL
 * Copyright 2017, 2018, 2019, 2020 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * mem_utils.c:
 * Utilities for memory management
 */

#include "../shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ms_init(void); // 0041

GLboolean has_cached_mem = GL_FALSE; // Flag for whether to use cached memory for mempools or not

#ifndef HAVE_CUSTOM_HEAP
static void *mempool_mspace[VGL_MEM_ALL - 1] = {}; // sceClibMspace heaps
#endif
static void *mempool_addr[VGL_MEM_ALL] = {}; // Base addresses of heap memblocks
static void *mempool_end[VGL_MEM_ALL] = {}; // Addresses of heap memblocks ends
static size_t mempool_size[VGL_MEM_ALL] = {}; // Sizes (in bytes) of heap memblocks

GLboolean vgl_has_cdlg_support = GL_TRUE;

#ifdef HAVE_WRAPPED_ALLOCATORS
void *__real_calloc(uint32_t nmember, uint32_t size);
void __real_free(void *addr);
void *__real_malloc(uint32_t size);
void *__real_memalign(uint32_t alignment, uint32_t size);
void *__real_realloc(void *ptr, uint32_t size);
#endif

#ifdef HAVE_CUSTOM_HEAP

// Lightweighted mutexes for thread-safeness
#ifndef HAVE_SINGLE_THREADED_GC
static SceKernelLwMutexWork tm_mutexes[VGL_MEM_ALL - 1] = {};
static SceKernelLwMutexWork header_mutex;
#define tm_lock_mutex(mtx) sceKernelLockLwMutex(mtx, 1, NULL);
#define tm_unlock_mutex(mtx) sceKernelUnlockLwMutex(mtx, 1);
#else
#define tm_lock_mutex(mtx)
#define tm_unlock_mutex(mtx)
#endif

// Minimum size in bytes for a block (header + footer)
#define MIN_FREE_BLOCK_SIZE (8)

// Memblock header
typedef struct tm_block_s {
	struct tm_block_s *next;
	struct tm_block_s *prev;
	uintptr_t base;
	uint32_t size;
	vglMemType type;
	GLboolean used;
} tm_block_t;

static tm_block_t *tm_alloclist[VGL_MEM_ALL - 1] = {}; // Lists of allocated blocks
static tm_block_t *tm_freelist[VGL_MEM_ALL - 1] = {}; // Lists of available free blocks
static uint32_t tm_free[VGL_MEM_ALL - 1] = {}; // Amount of raw free memory per heap type
static uint32_t tm_max_failed_alloc[VGL_MEM_ALL - 1] = {}; // Last requested allocation size in bytes that failed

#define HEADERS_PER_CHUNK (8192) // Number of headers to alloc per chunk when headers list is exhausted
static tm_block_t *tm_free_headers = NULL; // Available memblock headers list

/*
 * Get a new memblock header for an allocation.
 * Here we use malloc-ed pointers rather than appending the actual header on the block itself
 * so that these headers are fast to access by the CPU (since vitaGL uses uncached memory by default.
 */
static tm_block_t *heap_blk_new(void) {
	tm_lock_mutex(&header_mutex)
	if (!tm_free_headers) {
		vgl_log("Extending heap headers list with an extra chunk.\n");
#ifdef HAVE_WRAPPED_ALLOCATORS
		tm_block_t *chunk = __real_malloc(sizeof(tm_block_t) * HEADERS_PER_CHUNK);
#else
		tm_block_t *chunk = malloc(sizeof(tm_block_t) * HEADERS_PER_CHUNK);
#endif
		
#ifndef SKIP_ERROR_HANDLING
		if (!chunk) {
			vgl_log("%s:%d Out of memory: cannot allocate new headers for memory blocks!\n", __FILE__, __LINE__);	
			tm_unlock_mutex(&header_mutex)
			return NULL;
		}
#endif
		for (int i = 0; i < HEADERS_PER_CHUNK - 1; i++) {
			chunk[i].next = &chunk[i + 1];
		}
		chunk[HEADERS_PER_CHUNK - 1].next = NULL;
		tm_free_headers = &chunk[0];
	}
	tm_block_t *h = tm_free_headers;
	tm_free_headers = h->next;
	tm_unlock_mutex(&header_mutex)
	
	vgl_memset(h, 0, sizeof(tm_block_t));
	return h;
}

// Release a memblock header
static inline __attribute__((always_inline)) void heap_blk_release(tm_block_t *block) {
	tm_lock_mutex(&header_mutex)
	block->next = tm_free_headers;
	tm_free_headers = block;
	tm_unlock_mutex(&header_mutex)
}

// Insert a new block in the free blocks list
static void heap_blk_insert_free(tm_block_t *block) {
	block->used = GL_FALSE;
	uint32_t incoming_size = block->size;

	*(tm_block_t **)block->base = block;
	*(tm_block_t **)(block->base + block->size - 4) = block;
	
	uintptr_t right_addr = block->base + block->size;
	if (right_addr < (uintptr_t)mempool_end[block->type]) {
		tm_block_t *right = *(tm_block_t **)right_addr;
		if (right->used == GL_FALSE) {
			if (right->prev)
				right->prev->next = right->next;
			else
				tm_freelist[block->type] = right->next;
			if (right->next)
				right->next->prev = right->prev;

			block->size += right->size;
			*(tm_block_t **)(block->base + block->size - 4) = block;
			heap_blk_release(right);
		}
	}

	uintptr_t left_footer_addr = block->base - 4;
	if (left_footer_addr >= (uintptr_t)mempool_addr[block->type]) {
		tm_block_t *left = *(tm_block_t **)left_footer_addr;
		if (left->used == GL_FALSE) {
			if (left->prev)
				left->prev->next = left->next;
			else
				tm_freelist[block->type] = left->next;
			if (left->next)
				left->next->prev = left->prev;

			left->size += block->size;
			*(tm_block_t **)(left->base + left->size - 4) = left;
			heap_blk_release(block);
			block = left;
		}
	}

	tm_free[block->type] += incoming_size;

	block->prev = NULL;
	block->next = tm_freelist[block->type];
	if (tm_freelist[block->type])
		tm_freelist[block->type]->prev = block;
	tm_freelist[block->type] = block;
}

// Allocs a new block and returns it
static tm_block_t *heap_blk_alloc(int32_t type, uint32_t size, uint32_t alignment) {
	tm_block_t *curblk = tm_freelist[type];
	tm_block_t *prevblk = NULL;

	while (curblk) {
		uint32_t skip = VGL_ALIGN(curblk->base + 4, alignment) - 4 - curblk->base;
		if (skip != 0) {
			while (skip < MIN_FREE_BLOCK_SIZE) {
				skip += alignment;
			}
		}

		if (skip + size <= curblk->size) {
			tm_block_t *skipblk = NULL;
			tm_block_t *unusedblk = NULL;

			uint32_t remaining = curblk->size - skip - size;
			GLboolean has_extra_block = remaining >= MIN_FREE_BLOCK_SIZE;

			if (skip != 0) {
				skipblk = heap_blk_new();
				if (!skipblk)
					return NULL;
			}

			if (has_extra_block) {
				unusedblk = heap_blk_new();
				if (!unusedblk) {
					if (skipblk)
						heap_blk_release(skipblk);
					return NULL;
				}
			}

			if (prevblk)
				prevblk->next = curblk->next;
			else
				tm_freelist[type] = curblk->next;
			if (curblk->next)
				curblk->next->prev = prevblk;
			tm_free[type] -= curblk->size;

			if (skip != 0) {
				skipblk->type = curblk->type;
				skipblk->base = curblk->base;
				skipblk->size = skip;

				curblk->base += skip;
				curblk->size -= skip;
			}

			if (has_extra_block) {
				unusedblk->type = curblk->type;
				unusedblk->base = curblk->base + size;
				unusedblk->size = remaining;
				curblk->size = size;
			}

			curblk->used = GL_TRUE;
			*(tm_block_t **)curblk->base = curblk;
			*(tm_block_t **)(curblk->base + curblk->size - 4) = curblk;
			curblk->prev = NULL;
			curblk->next = tm_alloclist[type];
			if (tm_alloclist[type])
				tm_alloclist[type]->prev = curblk;
			tm_alloclist[type] = curblk;

			if (skip != 0)
				heap_blk_insert_free(skipblk);

			if (has_extra_block)
				heap_blk_insert_free(unusedblk);

			return curblk;
		}

		prevblk = curblk;
		curblk = curblk->next;
	}

	return NULL;
}

// Frees a previously allocated block
static void heap_blk_free(uintptr_t base, vglMemType type) {
	tm_block_t **slot = (tm_block_t **)(base - 4);
	tm_block_t *block = *slot;
	
	tm_lock_mutex(&tm_mutexes[type])

#ifndef SKIP_ERROR_HANDLING
	if (!block || block->base + 4 != base) {
		tm_unlock_mutex(&tm_mutexes[type])
		vgl_log("%s:%d A heap overflow or double free was detected on pointer: 0x%08X!\n", __FILE__, __LINE__, base);
		return;
	}
#endif
	*slot = NULL;
	
	if (block->prev)
		block->prev->next = block->next;
	else
		tm_alloclist[type] = block->next;
	if (block->next)
		block->next->prev = block->prev;

	block->next = NULL;
	block->prev = NULL;

	heap_blk_insert_free(block);
	tm_unlock_mutex(&tm_mutexes[type])
}

// Initializes heap setup
static void heap_init(void) {
	for (int i = 0; i < VGL_MEM_ALL - 1; i++) {
		tm_alloclist[i] = NULL;
		tm_freelist[i] = NULL;
		tm_free[i] = 0;
	}
}

// Extends the heap setup with a new heap type
static void heap_extend(int32_t type, void *base, uint32_t size) {
	tm_max_failed_alloc[type] = size + 1;
	tm_block_t *block = heap_blk_new();
	block->next = NULL;
	block->type = type;
	block->base = (uintptr_t)base;
	block->size = size;
	heap_blk_insert_free(block);
}

// Allocates memory from the requested heap type
static void *heap_alloc(int32_t type, uint32_t size, uint32_t alignment) {
	tm_lock_mutex(&tm_mutexes[type])
	tm_block_t *block = heap_blk_alloc(type, size + MIN_FREE_BLOCK_SIZE, alignment);
	tm_unlock_mutex(&tm_mutexes[type])
	if (!block) {
		tm_max_failed_alloc[type] = size;
		return NULL;
	}

	return (void *)(block->base + 4);
}

// Attempt to extend an allocated block with nearby unused blocks
static GLboolean heap_blk_try_extend(vglMemType type, tm_block_t *block, uint32_t new_size) {
	uintptr_t target_base = block->base + block->size;
	if (target_base >= (uintptr_t)mempool_end[type])
		return GL_FALSE;

	tm_block_t *curblk = *(tm_block_t **)target_base;
	if (curblk->used == GL_TRUE)
		return GL_FALSE;

	uint32_t available = block->size + curblk->size;
	if (available < new_size)
		return GL_FALSE;

	uint32_t needed_from_free = new_size - block->size;
	if (curblk->prev)
		curblk->prev->next = curblk->next;
	else
		tm_freelist[type] = curblk->next;
	if (curblk->next)
		curblk->next->prev = curblk->prev;

	if (needed_from_free == curblk->size) {
		tm_free[type] -= curblk->size;
		heap_blk_release(curblk);
	} else {
		curblk->base += needed_from_free;
		curblk->size -= needed_from_free;
		tm_free[type] -= needed_from_free;
		*(tm_block_t **)curblk->base = curblk;
		curblk->prev = NULL;
		curblk->next = tm_freelist[type];
		if (tm_freelist[type])
			tm_freelist[type]->prev = curblk;
		tm_freelist[type] = curblk;
	}
	
	block->size = new_size;
	*(tm_block_t **)(block->base + block->size - 4) = block;

	return GL_TRUE;
}

// realloc equivalent for heap
static void *heap_realloc(vglMemType type, void *ptr, uint32_t size) {
	// Check for memory corruption
	uint32_t real_size = size + MIN_FREE_BLOCK_SIZE;
	tm_block_t **slot = (tm_block_t **)((uintptr_t)ptr - 4);
	tm_block_t *block = *slot;
#ifndef SKIP_ERROR_HANDLING
	if (!block || block->base + 4 != (uintptr_t)ptr) {
		vgl_log("%s:%d Corrupted block during realloc: 0x%08X!\n", __FILE__, __LINE__, (uintptr_t)ptr);
		return NULL;
	}
#endif

	// If the block size is big enough to contain the new size, just return the block itself
	if (real_size <= block->size) {
		return ptr;
	}

	// Attempt to merge nearby blocks to fullfill the request
	tm_lock_mutex(&tm_mutexes[type]);
	int extended = heap_blk_try_extend(type, block, real_size);
	tm_unlock_mutex(&tm_mutexes[type]);
	if (extended) {
		return ptr;
	}

	// Everything failed, so alloc a new block of the requested size and move data on it
	void *newptr = heap_alloc(type, size, MEM_ALIGNMENT);
	if (newptr) {
		vgl_fast_memcpy(newptr, ptr, block->size - 4);
		heap_blk_free((uintptr_t)ptr, type);
	}
	return newptr;
}
#endif

#ifndef HAVE_CUSTOM_HEAP
/* MEMFREE BEGIN */
// 1.3 MEMFREE (patch 0028): the free figure of a pool without walking its heap. sceClibMspaceMallocStats walks every chunk of the mspace, so its cost grows with the number of
// blocks (textures, vertex data, render targets) and vglMemFree / vgl_mem_get_free_space paid it on every question. Every block comes through the wrappers below, so each pool
// keeps the usable bytes and the number of its live blocks; with the allocator's per-block overhead (measured at start-up, see vgl_mf_init) the free figure is
//     free = free_when_empty - (usable_bytes + overhead * live_blocks)
// which is what capacity - current_in_use is, without the walk. Exact when the overhead is one constant (checked by the probes; a pool that fails them keeps the walk).
// Mode 2 (vglSetMemFreeMode) also walks, compares, counts differences and answers with the walked figure.
#define VGL_MF_POOLS VGL_MEM_EXTERNAL
static uint32_t mf_usable[VGL_MF_POOLS]; // sum of sceClibMspaceMallocUsableSize over the live blocks of the pool
static uint32_t mf_live[VGL_MF_POOLS]; // live blocks
static uint32_t mf_empty_free[VGL_MF_POOLS]; // capacity - current_in_use of the empty pool
static uint32_t mf_overhead[VGL_MF_POOLS]; // allocator bytes per block on top of its usable size
static uint32_t mf_inflight[VGL_MF_POOLS]; // wrapper calls between their allocator call and their count update (mode 2 skips a check made then)
static uint32_t mf_gen[VGL_MF_POOLS]; // counted operations (mode 2: a check across a change is skipped)
static uint8_t mf_ok[VGL_MF_POOLS]; // the counters can answer for this pool
static int mf_mode = 0;
static uint32_t mf_n_fast = 0, mf_n_walk = 0, mf_n_checks = 0, mf_n_bad = 0, mf_n_raced = 0;
static int32_t mf_worst = 0;

void vglSetMemFreeMode(int mode) {
	mf_mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

void vglGetMemFreeStats(uint32_t *fast, uint32_t *walked, uint32_t *checks, uint32_t *mismatches, uint32_t *raced, int32_t *worst, uint32_t *pools) {
	if (fast)
		*fast = mf_n_fast;
	if (walked)
		*walked = mf_n_walk;
	if (checks)
		*checks = mf_n_checks;
	if (mismatches)
		*mismatches = mf_n_bad;
	if (raced)
		*raced = mf_n_raced;
	if (worst)
		*worst = mf_worst;
	if (pools) {
		uint32_t m = 0;
		for (int i = 0; i < VGL_MF_POOLS; i++)
			m |= mf_ok[i] ? (1u << i) : 0;
		*pools = m;
	}
}

static size_t vgl_mf_walk(vglMemType type) {
	SceClibMspaceStats stats;
	sceClibMspaceMallocStats(mempool_mspace[type], &stats);
	return stats.capacity - stats.current_in_use;
}

// Called when a pool's mspace has just been made (nothing allocated from it yet): measures the allocator's overhead with probe blocks and decides whether the counters can be used.
// A probe is a block of its own size (malloc and memalign); after each one current_in_use must have grown by usable size + one constant, and after all are freed it must be back.
static void vgl_mf_init(vglMemType type) {
	mf_ok[type] = 0;
	mf_usable[type] = mf_live[type] = mf_inflight[type] = mf_gen[type] = 0;
	void *ms = mempool_mspace[type];
	if (!ms)
		return;
	SceClibMspaceStats st;
	st.capacity = 0;
	st.current_in_use = 0;
	sceClibMspaceMallocStats(ms, &st);
	const uint32_t cap = st.capacity, base = st.current_in_use;
	if (!cap || base > cap)
		return; // (no statistics: the host emulator) -> the walk stays
	static const uint32_t sizes[] = { 1, 24, 100, 1000, 4096, 65536 };
	static const uint32_t aligns[] = { 0, 0, 16, 256, 4096, 0 }; // (0: malloc)
	void *blk[sizeof(sizes) / sizeof(sizes[0])] = {};
	uint32_t ov = 0, got = 0, in_use = base;
	GLboolean good = GL_TRUE;
	for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && good; i++) {
		blk[i] = aligns[i] ? sceClibMspaceMemalign(ms, aligns[i], sizes[i]) : sceClibMspaceMalloc(ms, sizes[i]);
		if (!blk[i])
			continue; // (a pool too small for this probe: the others decide)
		SceClibMspaceStats a;
		sceClibMspaceMallocStats(ms, &a);
		const uint32_t u = sceClibMspaceMallocUsableSize(blk[i]);
		const uint32_t delta = a.current_in_use - in_use;
		in_use = a.current_in_use;
		if (delta < u || a.capacity != cap) {
			good = GL_FALSE;
			break;
		}
		if (!got++)
			ov = delta - u;
		else if (delta - u != ov)
			good = GL_FALSE;
	}
	for (uint32_t i = sizeof(sizes) / sizeof(sizes[0]); i-- > 0;)
		if (blk[i])
			sceClibMspaceFree(ms, blk[i]);
	SceClibMspaceStats e;
	sceClibMspaceMallocStats(ms, &e);
	if (!good || got < 3 || e.current_in_use != base || e.capacity != cap) {
		vgl_log("%s:%d: pool %d: free figure kept on the heap walk (%u probes, overhead %u, in use %u of %u -> %u)\n", __FILE__, __LINE__, (int)type, got, ov, base, cap, e.current_in_use);
		return;
	}
	mf_empty_free[type] = cap - base;
	mf_overhead[type] = ov;
	mf_ok[type] = 1;
}

static inline GLboolean vgl_mf_begin(vglMemType type) {
	if ((unsigned)type >= VGL_MF_POOLS || !mf_ok[type])
		return GL_FALSE;
	__atomic_fetch_add(&mf_inflight[type], 1, __ATOMIC_SEQ_CST);
	return GL_TRUE;
}

// `du`: usable bytes gained (negative: lost); `dl`: blocks gained.
static inline void vgl_mf_end(vglMemType type, GLboolean tracked, int32_t du, int32_t dl) {
	if (!tracked)
		return;
	__atomic_fetch_add(&mf_usable[type], (uint32_t)du, __ATOMIC_SEQ_CST);
	__atomic_fetch_add(&mf_live[type], (uint32_t)dl, __ATOMIC_SEQ_CST);
	__atomic_fetch_add(&mf_gen[type], 1, __ATOMIC_SEQ_CST);
	__atomic_fetch_sub(&mf_inflight[type], 1, __ATOMIC_SEQ_CST);
}

static inline void *vgl_mf_took(vglMemType type, GLboolean tracked, void *ptr) {
	vgl_mf_end(type, tracked, ptr && tracked ? (int32_t)sceClibMspaceMallocUsableSize(ptr) : 0, ptr ? 1 : 0);
	return ptr;
}

static size_t vgl_mf_free(vglMemType type) {
	const uint32_t i0 = __atomic_load_n(&mf_inflight[type], __ATOMIC_SEQ_CST), g0 = __atomic_load_n(&mf_gen[type], __ATOMIC_SEQ_CST);
	const uint32_t live = __atomic_load_n(&mf_live[type], __ATOMIC_SEQ_CST), usable = __atomic_load_n(&mf_usable[type], __ATOMIC_SEQ_CST);
	const uint32_t used = usable + mf_overhead[type] * live;
	const size_t fast = used < mf_empty_free[type] ? mf_empty_free[type] - used : 0;
	if (mf_mode < 2) {
		mf_n_fast++;
		return fast;
	}
	const size_t walked = vgl_mf_walk(type);
	mf_n_walk++;
	const uint32_t i1 = __atomic_load_n(&mf_inflight[type], __ATOMIC_SEQ_CST), g1 = __atomic_load_n(&mf_gen[type], __ATOMIC_SEQ_CST);
	if (i0 || i1 || g0 != g1) {
		mf_n_raced++;
		return fast;
	}
	mf_n_checks++;
	if (walked != fast) {
		const int32_t d = (int32_t)fast - (int32_t)walked;
		if (!mf_n_bad++ || (d < 0 ? -d : d) > (mf_worst < 0 ? -mf_worst : mf_worst))
			mf_worst = d;
		if (mf_n_bad <= 8)
			vgl_log("%s:%d: MEMFREE pool %d: counters say %u free, the walk says %u (%d, %u blocks, %u usable bytes, overhead %u)\n", __FILE__, __LINE__, (int)type, (unsigned)fast,
				(unsigned)walked, d, live, usable, mf_overhead[type]);
	}
	return walked;
}
/* MEMFREE END */
#endif

#ifdef PHYCONT_ON_DEMAND
void *vgl_alloc_phycont_block(uint32_t size) {
	size = VGL_ALIGN(size, 1024 * 1024);
	SceUID blk = sceKernelAllocMemBlock("phycont_blk", has_cached_mem ? SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);

	if (blk < 0)
		return NULL;

	void *res;
	sceKernelGetMemBlockBase(blk, &res);
	sceGxmMapMemory(res, size, SCE_GXM_MEMORY_ATTRIB_RW);

	return res;
}
#endif

void vgl_mem_init(size_t size_ram, size_t size_cdram, size_t size_phycont, size_t size_cdlg) {
	ms_init(); // 0041 MSC_VGL_MEMSITES (before the pools' first block)
	vgl_has_cdlg_support = GL_TRUE;

	if (!has_cached_mem && size_ram > 0xC800000) // Vita has a smaller address mapping for uncached mem
		size_ram = 0xC800000;

	mempool_size[VGL_MEM_VRAM] = VGL_ALIGN(size_cdram, 256 * 1024);
	mempool_size[VGL_MEM_RAM] = VGL_ALIGN(size_ram, 4 * 1024);
#ifdef PHYCONT_ON_DEMAND
	mempool_size[VGL_MEM_PHYCONT] = 0;
#else
	mempool_size[VGL_MEM_PHYCONT] = VGL_ALIGN(size_phycont, 1024 * 1024);
#endif
	mempool_size[VGL_MEM_BUDGET] = VGL_ALIGN(size_cdlg, 4 * 1024);

#ifdef HAVE_CUSTOM_HEAP
	// Initialize heap
#ifndef HAVE_SINGLE_THREADED_GC
	sceKernelCreateLwMutex(&header_mutex, "header mutex alloc", 0, 0, NULL);
#endif
	heap_init();
#endif

	SceUID mempool_id[VGL_MEM_ALL - 1] = {}; // UIDs of heap memblocks
	if (mempool_size[VGL_MEM_VRAM]) {
		mempool_id[VGL_MEM_VRAM] = sceKernelAllocMemBlock("cdram_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, mempool_size[VGL_MEM_VRAM], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
		sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_VRAM], "cdram heap mutex", 0, 0, NULL);
#endif
	}
	if (has_cached_mem) {
		if (mempool_size[VGL_MEM_RAM]) {
			mempool_id[VGL_MEM_RAM] = sceKernelAllocMemBlock("ram_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mempool_size[VGL_MEM_RAM], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_RAM], "ram heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id[VGL_MEM_PHYCONT] = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_BUDGET]) {
			mempool_id[VGL_MEM_BUDGET] = sceKernelAllocMemBlock("cdlg_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW, mempool_size[VGL_MEM_BUDGET], NULL);
			vgl_has_cdlg_support = GL_FALSE;
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_BUDGET], "cdlg heap mutex", 0, 0, NULL);
#endif
		}
	} else {
		if (mempool_size[VGL_MEM_RAM]) {
			mempool_id[VGL_MEM_RAM] = sceKernelAllocMemBlock("ram_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, mempool_size[VGL_MEM_RAM], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_RAM], "ram heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id[VGL_MEM_PHYCONT] = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
		if (mempool_size[VGL_MEM_BUDGET]) {
			mempool_id[VGL_MEM_BUDGET] = sceKernelAllocMemBlock("cdlg_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW, mempool_size[VGL_MEM_BUDGET], NULL);
			vgl_has_cdlg_support = GL_FALSE;
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_BUDGET], "cdlg heap mutex", 0, 0, NULL);
#endif
		}
	}
	for (int i = 0; i < VGL_MEM_EXTERNAL; i++) {
		if (mempool_size[i]) {
			mempool_addr[i] = NULL;
			sceKernelGetMemBlockBase(mempool_id[i], &mempool_addr[i]);

			if (mempool_addr[i]) {
				mempool_end[i] = (void *)((uintptr_t)mempool_addr[i] + mempool_size[i]);
				sceGxmMapMemory(mempool_addr[i], mempool_size[i], SCE_GXM_MEMORY_ATTRIB_RW);
#ifndef HAVE_CUSTOM_HEAP
				mempool_mspace[i] = sceClibMspaceCreate(mempool_addr[i], mempool_size[i]);
				vgl_mf_init((vglMemType)i); // 1.3 MEMFREE (patch 0028)
#else
				heap_extend(i, mempool_addr[i], mempool_size[i]);
#endif
			}
		}
	}

#ifdef PHYCONT_ON_DEMAND
	// Getting total available phycont mem
	uint32_t phycont_size;
	if (system_app_mode) {
		SceAppMgrBudgetInfo info;
		info.size = sizeof(SceAppMgrBudgetInfo);
		sceAppMgrGetBudgetInfo(&info);
		phycont_size = info.total_phycont_mem;
	} else {
		SceKernelFreeMemorySizeInfo info;
		info.size = sizeof(SceKernelFreeMemorySizeInfo);
		sceKernelGetFreeMemorySize(&info);
		phycont_size = info.size_phycont;
	}
	mempool_size[VGL_MEM_PHYCONT] = phycont_size;
#endif

	// Mapping newlib heap into sceGxm
#ifdef HAVE_WRAPPED_ALLOCATORS
	void *dummy = __real_malloc(1);
	__real_free(dummy);
#else
	void *dummy = malloc(1);
	free(dummy);
#endif
	

	SceKernelMemBlockInfo info;
	info.size = sizeof(SceKernelMemBlockInfo);
	sceKernelGetMemBlockInfoByAddr(dummy, &info);
	sceGxmMapMemory(info.mappedBase, info.mappedSize, SCE_GXM_MEMORY_ATTRIB_RW);

	mempool_size[VGL_MEM_EXTERNAL] = info.mappedSize;
	mempool_addr[VGL_MEM_EXTERNAL] = info.mappedBase;
	mempool_end[VGL_MEM_EXTERNAL] = (void *)((uintptr_t)mempool_addr[VGL_MEM_EXTERNAL] + mempool_size[VGL_MEM_EXTERNAL]);
}

#ifndef PHYCONT_ON_DEMAND
void vgl_mem_provide_phycont(size_t size_phycont) {
	if (mempool_size[VGL_MEM_PHYCONT])
		return;

	SceUID mempool_id;
	mempool_size[VGL_MEM_PHYCONT] = VGL_ALIGN(size_phycont, 1024 * 1024);
	if (has_cached_mem) {
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
	} else {
		if (mempool_size[VGL_MEM_PHYCONT]) {
			mempool_id = sceKernelAllocMemBlock("phycont_mempool", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, mempool_size[VGL_MEM_PHYCONT], NULL);
#if defined(HAVE_CUSTOM_HEAP) && !defined(HAVE_SINGLE_THREADED_GC)
			sceKernelCreateLwMutex(&tm_mutexes[VGL_MEM_PHYCONT], "phycont heap mutex", 0, 0, NULL);
#endif
		}
	}
	
	mempool_addr[VGL_MEM_PHYCONT] = NULL;
	sceKernelGetMemBlockBase(mempool_id, &mempool_addr[VGL_MEM_PHYCONT]);
	if (mempool_addr[VGL_MEM_PHYCONT]) {
		mempool_end[VGL_MEM_PHYCONT] = (void *)((uintptr_t)mempool_addr[VGL_MEM_PHYCONT] + mempool_size[VGL_MEM_PHYCONT]);
		sceGxmMapMemory(mempool_addr[VGL_MEM_PHYCONT], mempool_size[VGL_MEM_PHYCONT], SCE_GXM_MEMORY_ATTRIB_RW);
#ifndef HAVE_CUSTOM_HEAP
		mempool_mspace[VGL_MEM_PHYCONT] = sceClibMspaceCreate(mempool_addr[VGL_MEM_PHYCONT], mempool_size[VGL_MEM_PHYCONT]);
		vgl_mf_init(VGL_MEM_PHYCONT); // 1.3 MEMFREE (patch 0028)
#else
		heap_extend(VGL_MEM_PHYCONT, mempool_addr[VGL_MEM_PHYCONT], mempool_size[VGL_MEM_PHYCONT]);
#endif
	}
}
#endif

vglMemType vgl_mem_get_type_by_addr(void *addr) {
	if (addr >= mempool_addr[VGL_MEM_VRAM] && addr < mempool_end[VGL_MEM_VRAM])
		return VGL_MEM_VRAM;
	else if (addr >= mempool_addr[VGL_MEM_RAM] && addr < mempool_end[VGL_MEM_RAM])
		return VGL_MEM_RAM;
#ifndef PHYCONT_ON_DEMAND
	else if (addr >= mempool_addr[VGL_MEM_PHYCONT] && addr < mempool_end[VGL_MEM_PHYCONT])
		return VGL_MEM_PHYCONT;
#endif
	else if (addr >= mempool_addr[VGL_MEM_BUDGET] && addr < mempool_end[VGL_MEM_BUDGET])
		return VGL_MEM_BUDGET;
	else if (addr >= mempool_addr[VGL_MEM_EXTERNAL] && addr < mempool_end[VGL_MEM_EXTERNAL])
		return VGL_MEM_EXTERNAL;
#ifdef PHYCONT_ON_DEMAND
	return VGL_MEM_PHYCONT;
#else
	return -1;
#endif
}

size_t vgl_mem_get_free_space(vglMemType type) {
	if (type == VGL_MEM_EXTERNAL) {
		return 0;
#ifdef PHYCONT_ON_DEMAND
	} else if (type == VGL_MEM_PHYCONT) {
		if (system_app_mode) {
			SceAppMgrBudgetInfo info;
			info.size = sizeof(SceAppMgrBudgetInfo);
			sceAppMgrGetBudgetInfo(&info);
			return info.free_phycont_mem;
		} else {
			SceKernelFreeMemorySizeInfo info;
			info.size = sizeof(SceKernelFreeMemorySizeInfo);
			sceKernelGetFreeMemorySize(&info);
			return info.size_phycont;
		}
#endif
	} else if (type == VGL_MEM_ALL) {
		size_t size = 0;
		for (int i = 0; i < VGL_MEM_EXTERNAL; i++) {
			size += vgl_mem_get_free_space(i);
		}
		return size;
#ifdef HAVE_CUSTOM_HEAP
	} else {
		return tm_free[type];
	}
#else
	} else if (mempool_size[type]) {
		if (mf_mode && mf_ok[type]) // 1.3 MEMFREE (patch 0028): from the counters
			return vgl_mf_free(type);
		mf_n_walk++;
		return vgl_mf_walk(type);
	} else
		return 0;
#endif
}

size_t vgl_mem_get_total_space(vglMemType type) {
	if (type == VGL_MEM_ALL) {
		size_t size = 0;
		for (int i = 0; i < VGL_MEM_EXTERNAL; i++) {
			size += vgl_mem_get_total_space(i);
		}
		return size;
	} else {
		return mempool_size[type];
	}
}

size_t vgl_malloc_usable_size(void *ptr) {
	vglMemType type = vgl_mem_get_type_by_addr(ptr);
	if (type == VGL_MEM_EXTERNAL)
		return malloc_usable_size(ptr);
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT) {
		SceKernelMemBlockInfo info;
		info.size = sizeof(SceKernelMemBlockInfo);
		sceKernelGetMemBlockInfoByAddr(ptr, &info);
		return info.mappedSize;
	}
#endif
#ifdef HAVE_CUSTOM_HEAP
	tm_block_t **hdr = (tm_block_t **)((uintptr_t)ptr - 4);
	tm_block_t *block = *hdr;
	return block->size - MIN_FREE_BLOCK_SIZE;
#else
	return sceClibMspaceMallocUsableSize(ptr);
#endif
}

static void vgl_free_impl(void *ptr) {
	if (!ptr)
		return;

	vglMemType type = vgl_mem_get_type_by_addr(ptr);
	if (type == VGL_MEM_EXTERNAL)
#ifdef HAVE_WRAPPED_ALLOCATORS
		__real_free(ptr);
#else
		free(ptr);
#endif
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT) {
		sceGxmUnmapMemory(ptr);
		sceKernelFreeMemBlock(sceKernelFindMemBlockByAddr(ptr, 0));
	}
#endif
#ifdef HAVE_CUSTOM_HEAP
	else {
		heap_blk_free((uintptr_t)ptr, type);
		tm_max_failed_alloc[type] = tm_free[type] + 1;
	}
#else
	else if (mempool_mspace[type]) {
		const GLboolean tr = vgl_mf_begin(type); // 1.3 MEMFREE (patch 0028)
		const int32_t u = tr ? (int32_t)sceClibMspaceMallocUsableSize(ptr) : 0;
		sceClibMspaceFree(mempool_mspace[type], ptr);
		vgl_mf_end(type, tr, -u, -1);
	}
#endif
}

static void *vgl_malloc_impl(size_t size, vglMemType type) {
	if (type == VGL_MEM_EXTERNAL)
#ifdef HAVE_WRAPPED_ALLOCATORS
		return __real_malloc(size);
#else
		return malloc(size);
#endif
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT)
		return vgl_alloc_phycont_block(size);
#endif
#ifdef HAVE_CUSTOM_HEAP
	else if (size <= tm_max_failed_alloc[type])
		return heap_alloc(type, size, MEM_ALIGNMENT);
#else
	else if (mempool_mspace[type]) {
		const GLboolean tr = vgl_mf_begin(type); // 1.3 MEMFREE (patch 0028)
		return vgl_mf_took(type, tr, sceClibMspaceMalloc(mempool_mspace[type], size));
	}
#endif
	return NULL;
}

static void *vgl_calloc_impl(size_t num, size_t size, vglMemType type) {
	if (type == VGL_MEM_EXTERNAL)
#ifdef HAVE_WRAPPED_ALLOCATORS
		return __real_calloc(num, size);
#else
		return calloc(num, size);
#endif
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT) {
		void *ret = vgl_alloc_phycont_block(num * size);
		if (ret) {
			sceClibMemset(ret, 0, num * size);
		}
		return ret;
	}
#endif
#ifdef HAVE_CUSTOM_HEAP
	else if (num * size <= tm_max_failed_alloc[type]) {
		void *ret = heap_alloc(type, num * size, MEM_ALIGNMENT);
		if (ret) {
			sceClibMemset(ret, 0, num * size);
		}
		return ret;
	}
#else
	else if (mempool_mspace[type]) {
		const GLboolean tr = vgl_mf_begin(type); // 1.3 MEMFREE (patch 0028)
		return vgl_mf_took(type, tr, sceClibMspaceCalloc(mempool_mspace[type], num, size));
	}
#endif
	return NULL;
}

static void *vgl_memalign_impl(size_t alignment, size_t size, vglMemType type) {
	if (type == VGL_MEM_EXTERNAL)
#ifdef HAVE_WRAPPED_ALLOCATORS
		return __real_memalign(alignment, size);
#else
		return memalign(alignment, size);
#endif
#ifdef PHYCONT_ON_DEMAND
	else if (type == VGL_MEM_PHYCONT)
		return vgl_alloc_phycont_block(size);
#endif
#ifdef HAVE_CUSTOM_HEAP
	else if (size <= tm_max_failed_alloc[type])
		return heap_alloc(type, size, alignment);
#else
	else if (mempool_mspace[type]) {
		const GLboolean tr = vgl_mf_begin(type); // 1.3 MEMFREE (patch 0028)
		return vgl_mf_took(type, tr, sceClibMspaceMemalign(mempool_mspace[type], alignment, size));
	}
#endif
	return NULL;
}

static void *vgl_realloc_impl(void *ptr, size_t size) {
	if (!ptr) {
#ifdef HAVE_WRAPPED_ALLOCATORS
		return __real_malloc(size);
#else
		return malloc(size);
#endif
	}
	vglMemType type = vgl_mem_get_type_by_addr(ptr);
	if (type == VGL_MEM_EXTERNAL) {
#ifdef HAVE_WRAPPED_ALLOCATORS
		return __real_realloc(ptr, size);
#else
		return realloc(ptr, size);
#endif
	}
#ifdef PHYCONT_ON_DEMAND
	if (type == VGL_MEM_PHYCONT) {
		size_t old_size = vgl_malloc_usable_size(ptr);
		if (old_size >= size) {
			return ptr;
		}
		void *res = vgl_alloc_phycont_block(size);
		if (res) {
			vgl_fast_memcpy(res, ptr, old_size);
			vgl_free(ptr);
			return res;
		}
	}
#endif
#ifndef HAVE_CUSTOM_HEAP
	{
		const GLboolean tr = vgl_mf_begin(type); // 1.3 MEMFREE (patch 0028): the block's usable size before and after (it may move, grow or shrink in place)
		const int32_t before = tr ? (int32_t)sceClibMspaceMallocUsableSize(ptr) : 0;
		void *res = sceClibMspaceRealloc(mempool_mspace[type], ptr, size);
		vgl_mf_end(type, tr, res ? (int32_t)sceClibMspaceMallocUsableSize(res) - before : 0, 0);
		return res;
	}
#else
	return heap_realloc(type, ptr, size);
#endif
}

/* MEMSITES BEGIN */
// 0041 MSC_VGL_MEMSITES (code default 0): who holds the memory of each pool. With the switch on, every block a pool gives out (and every block gpu_utils' use_extra_mem fallback takes
// from the newlib heap, "spill") is kept in one table by address: its requested size, the call site that asked (the outermost caller of gpu_alloc_mapped_aligned_*, or of vgl_malloc,
// calloc, memalign, realloc) and the kind the allocating routine names (gpu / cpu / raw: the entry only; tex, mip, pal: gpu_utils' texture routines; usse: shader code memory).
// vglMemSiteDump(tag) prints it as "[memsite]" lines per pool and site; the port's NpVitaMemSites (vita_memreport.cpp) adds a check against its own pool counters.
// With the switch off the wrappers below take one branch on vgl_ms_on and store nothing; the table (64 KiB) is made only when the switch is on. The kind of the block (colour, depth,
// stencil, vertex buffer, uniform ring ...) is the function the site address lies in: vglMemSiteDump prints each site as an offset from vglMemSiteDump itself, nm of the executable names it.
#define MS_SLOTS 7936 // 7936 * 8 bytes + 256 sites * 8 bytes = 64 KiB
#define MS_SITES 256
#define MS_POOLS 5 // VGL_MEM_VRAM .. VGL_MEM_EXTERNAL (the spills)
typedef struct {
	uint32_t ptr; // 0: free
	uint32_t meta; // requested size in 4-byte units << 8 | site index
} ms_slot_t;
typedef struct {
	uint32_t addr;
	uint8_t pool, kind, pad[2];
} ms_site_t;
typedef struct {
	void *site;
	uint8_t kind;
} ms_ctx_t;

int vgl_ms_on = 0; // read by gpu_utils.c's entry points; set once, by vgl_mem_init (MSC_VGL_MEMSITES) or vglSetMemSites
void *vgl_ms_site = NULL; // the outermost caller of an allocation routine in progress
uint8_t vgl_ms_kind = 0; // the kind that routine names

static ms_slot_t *ms_tab = NULL;
static ms_site_t *ms_sites = NULL;
static uint32_t ms_nsites = 0, ms_objs = 0, ms_objs_peak = 0, ms_lost = 0, ms_site_over = 0, ms_clipped = 0, ms_dumps = 0;
static uint32_t ms_pool_blocks[MS_POOLS];
static uint64_t ms_pool_bytes[MS_POOLS];
static uint32_t ms_failed[MS_POOLS];
static uint64_t ms_failed_bytes[MS_POOLS];
static uint32_t ms_spill_total = 0;
static uint64_t ms_spill_peak = 0;
static volatile int ms_lock = 0, ms_dumping = 0;
static uint8_t ms_event_done[4];
static const char *const ms_pool_name[MS_POOLS] = { "CDRAM", "main", "PHYCONT", "CDLG", "heap spill" };
static const char *const ms_kind_name[] = { "raw", "gpu", "cpu", "tex", "mip", "pal", "usse" };
#define MS_RAW 0
#define MS_GPU 1
#define MS_CPU 2
#define MS_TEX 3
#define MS_MIP 4
#define MS_PAL 5
#define MS_USSE 6
#define MS_KINDS 7

static inline void ms_acquire(void) {
	while (__sync_lock_test_and_set(&ms_lock, 1)) {
	}
}
static inline void ms_release(void) {
	__sync_lock_release(&ms_lock);
}
static inline uint32_t ms_home(uint32_t p) {
	return (uint32_t)(((uint64_t)((p >> 3) * 2654435761u) * MS_SLOTS) >> 32);
}

void vglSetMemSites(int on) {
	if (on && !ms_tab) {
		void *mem = calloc(1, MS_SLOTS * sizeof(ms_slot_t) + MS_SITES * sizeof(ms_site_t));
		if (!mem)
			return;
		ms_tab = (ms_slot_t *)mem;
		ms_sites = (ms_site_t *)((uint8_t *)mem + MS_SLOTS * sizeof(ms_slot_t));
	}
	vgl_ms_on = on && ms_tab ? 1 : 0;
}

int vglMemSitesOn(void) {
	return vgl_ms_on;
}

static void ms_init(void) {
	const char *e = getenv("MSC_VGL_MEMSITES");
	if (e && *e && atoi(e) != 0)
		vglSetMemSites(1);
}

// The routines of gpu_utils.c enter with MS_ENTER (a cleanup variable restores the context on every return); the outermost caller's address stays, a texture routine's kind beats the entry's.
ms_ctx_t vgl_ms_enter(uint8_t kind, void *ra) {
	ms_ctx_t old = { vgl_ms_site, vgl_ms_kind };
	if (!vgl_ms_site)
		vgl_ms_site = ra;
	if (kind >= MS_TEX || !vgl_ms_kind)
		vgl_ms_kind = kind;
	return old;
}
void vgl_ms_leave(ms_ctx_t *old) {
	vgl_ms_site = old->site;
	vgl_ms_kind = old->kind;
}

static uint32_t ms_site_id(void *site, int pool, int kind) {
	const uint32_t a = (uint32_t)(uintptr_t)site;
	for (uint32_t i = 0; i < ms_nsites; i++)
		if (ms_sites[i].addr == a && ms_sites[i].pool == pool && ms_sites[i].kind == kind)
			return i;
	if (ms_nsites < MS_SITES - 1) {
		ms_sites[ms_nsites].addr = a;
		ms_sites[ms_nsites].pool = pool;
		ms_sites[ms_nsites].kind = kind;
		return ms_nsites++;
	}
	ms_site_over++; // the last index takes the rest
	ms_sites[MS_SITES - 1].pool = pool;
	ms_sites[MS_SITES - 1].kind = kind;
	ms_nsites = MS_SITES;
	return MS_SITES - 1;
}

static void ms_note_at(void *ptr, size_t size, int pool, void *site, int kind) {
	if (!ptr || pool < 0 || pool >= MS_POOLS)
		return;
	uint32_t units = (uint32_t)((size + 3) >> 2);
	if (size > 0x3FFFFFFCu || units > 0xFFFFFFu) {
		units = 0xFFFFFFu;
		ms_clipped++;
	}
	ms_acquire();
	const uint32_t key = (uint32_t)(uintptr_t)ptr;
	uint32_t i = ms_home(key), n = 0;
	while (ms_tab[i].ptr && ms_tab[i].ptr != key && n < MS_SLOTS) {
		i = i + 1 == MS_SLOTS ? 0 : i + 1;
		n++;
	}
	if (ms_tab[i].ptr == key) { // the address is handed out again and its free was not seen: the old entry goes
		const ms_site_t *o = &ms_sites[ms_tab[i].meta & 0xFF];
		ms_pool_bytes[o->pool] -= (uint64_t)(ms_tab[i].meta >> 8) * 4;
		ms_pool_blocks[o->pool]--;
		ms_objs--;
	} else if (n >= MS_SLOTS || ms_objs >= MS_SLOTS - MS_SLOTS / 8) {
		ms_lost++; // table full: this block is not kept (its free is then not found)
		ms_release();
		return;
	}
	const uint32_t id = ms_site_id(site, pool, kind);
	ms_tab[i].ptr = key;
	ms_tab[i].meta = units << 8 | id;
	ms_pool_bytes[pool] += (uint64_t)units * 4;
	ms_pool_blocks[pool]++;
	if (++ms_objs > ms_objs_peak)
		ms_objs_peak = ms_objs;
	if (pool == VGL_MEM_EXTERNAL) {
		ms_spill_total++;
		if (ms_pool_bytes[pool] > ms_spill_peak)
			ms_spill_peak = ms_pool_bytes[pool];
	}
	ms_release();
}

// Forgets ptr; returns 1 and its size, site and kind when it was kept.
static int ms_forget(void *ptr, size_t *size, void **site, int *kind, int *pool) {
	int found = 0;
	ms_acquire();
	const uint32_t key = (uint32_t)(uintptr_t)ptr;
	uint32_t i = ms_home(key), n = 0;
	while (ms_tab[i].ptr && n < MS_SLOTS) {
		if (ms_tab[i].ptr == key) {
			const ms_site_t *o = &ms_sites[ms_tab[i].meta & 0xFF];
			const uint64_t bytes = (uint64_t)(ms_tab[i].meta >> 8) * 4;
			if (size)
				*size = (size_t)bytes;
			if (site)
				*site = (void *)(uintptr_t)o->addr;
			if (kind)
				*kind = o->kind;
			if (pool)
				*pool = o->pool;
			ms_pool_bytes[o->pool] -= bytes;
			ms_pool_blocks[o->pool]--;
			ms_objs--;
			uint32_t hole = i, j = i; // backward-shift deletion (linear probing)
			for (;;) {
				j = j + 1 == MS_SLOTS ? 0 : j + 1;
				if (!ms_tab[j].ptr)
					break;
				const uint32_t home = ms_home(ms_tab[j].ptr);
				if ((j + MS_SLOTS - home) % MS_SLOTS >= (j + MS_SLOTS - hole) % MS_SLOTS) {
					ms_tab[hole] = ms_tab[j];
					hole = j;
				}
			}
			ms_tab[hole].ptr = 0;
			ms_tab[hole].meta = 0;
			found = 1;
			break;
		}
		i = i + 1 == MS_SLOTS ? 0 : i + 1;
		n++;
	}
	ms_release();
	return found;
}

// Spills: a block of the newlib heap a GPU allocation fell back on (gpu_utils.c).
void vgl_ms_note_spill(void *ptr, size_t size) {
	if (ptr)
		ms_note_at(ptr, size, VGL_MEM_EXTERNAL, vgl_ms_site ? vgl_ms_site : __builtin_return_address(0), vgl_ms_kind);
}

// Blocks kept inside [base, end): bytes and count (the port compares them with the counters its own wrapper of the allocator keeps).
int vglMemSiteRange(uintptr_t base, uintptr_t end, uint64_t *bytes, uint32_t *blocks) {
	uint64_t b = 0;
	uint32_t c = 0;
	if (!vgl_ms_on) {
		*bytes = 0;
		*blocks = 0;
		return 0;
	}
	ms_acquire();
	for (uint32_t i = 0; i < MS_SLOTS; i++)
		if (ms_tab[i].ptr >= base && ms_tab[i].ptr < end) {
			b += (uint64_t)(ms_tab[i].meta >> 8) * 4;
			c++;
		}
	ms_release();
	*bytes = b;
	*blocks = c;
	return 1;
}

void vglMemSiteDump(const char *tag) {
	if (!vgl_ms_on || __sync_lock_test_and_set(&ms_dumping, 1))
		return;
	struct {
		uint32_t bytes, blocks, largest;
	} agg[MS_SITES];
	memset(agg, 0, sizeof(agg));
	uint32_t nsites, objs, peak, lost, over, clipped;
	uint32_t pblocks[MS_POOLS], failed[MS_POOLS];
	uint64_t pbytes[MS_POOLS], fbytes[MS_POOLS], spill_peak;
	ms_site_t sites[MS_SITES];
	ms_acquire();
	for (uint32_t i = 0; i < MS_SLOTS; i++)
		if (ms_tab[i].ptr) {
			const uint32_t id = ms_tab[i].meta & 0xFF, bytes = (ms_tab[i].meta >> 8) * 4;
			agg[id].bytes += bytes;
			agg[id].blocks++;
			if (bytes > agg[id].largest)
				agg[id].largest = bytes;
		}
	nsites = ms_nsites;
	objs = ms_objs;
	peak = ms_objs_peak;
	lost = ms_lost;
	over = ms_site_over;
	clipped = ms_clipped;
	spill_peak = ms_spill_peak;
	for (int p = 0; p < MS_POOLS; p++) {
		pblocks[p] = ms_pool_blocks[p];
		pbytes[p] = ms_pool_bytes[p];
		failed[p] = ms_failed[p];
		fbytes[p] = ms_failed_bytes[p];
	}
	memcpy(sites, ms_sites, sizeof(sites));
	ms_release();
	const uintptr_t ref = (uintptr_t)&vglMemSiteDump;
	fprintf(stderr, "[memsite] %s: dump %u; %u blocks live (most %u), %u sites, %u not kept (table full), %u sites over the 255 (shared slot), %u sizes clipped; %u spills to the newlib heap so far, %u KiB held now (most %u KiB); dump reference %p (vglMemSiteDump)\n",
		tag ? tag : "", ++ms_dumps, objs, peak, nsites, lost, over, clipped, ms_spill_total, (unsigned)(pbytes[VGL_MEM_EXTERNAL] / 1024), (unsigned)(spill_peak / 1024), (void *)ref);
	for (int p = 0; p < MS_POOLS; p++) {
		fprintf(stderr, "[memsite] %s: pool %d (%s): %llu bytes (%u KiB) in %u blocks by site; allocations refused %u (%u KiB asked)\n", tag ? tag : "", p, ms_pool_name[p], (unsigned long long)pbytes[p],
			(unsigned)(pbytes[p] / 1024), pblocks[p], failed[p], (unsigned)(fbytes[p] / 1024));
		uint16_t order[MS_SITES];
		uint32_t n = 0;
		for (uint32_t s = 0; s < nsites; s++)
			if (sites[s].pool == p && agg[s].blocks) {
				uint32_t k = n++;
				for (; k > 0 && agg[order[k - 1]].bytes < agg[s].bytes; k--)
					order[k] = order[k - 1];
				order[k] = (uint16_t)s;
			}
		uint64_t rest = 0;
		uint32_t rest_n = 0, rest_blocks = 0;
		for (uint32_t k = 0; k < n; k++) {
			const uint32_t s = order[k];
			if (k >= 28) {
				rest += agg[s].bytes;
				rest_n++;
				rest_blocks += agg[s].blocks;
				continue;
			}
			fprintf(stderr, "[memsite] %s: pool %d %-4s site %08x (ref%+d): %5u blocks %7u KiB, largest %u KiB\n", tag ? tag : "", p, ms_kind_name[sites[s].kind < MS_KINDS ? sites[s].kind : 0], sites[s].addr,
				(int)((intptr_t)sites[s].addr - (intptr_t)ref), agg[s].blocks, (agg[s].bytes + 1023) / 1024, (agg[s].largest + 1023) / 1024);
		}
		if (rest_n)
			fprintf(stderr, "[memsite] %s: pool %d and %u more sites: %u blocks %u KiB\n", tag ? tag : "", p, rest_n, rest_blocks, (unsigned)(rest / 1024));
	}
	__sync_lock_release(&ms_dumping);
}

// The first refusal of a pool and the first time every pool refused (the recovery of gpu_utils.c): one dump each, with what was asked.
static void ms_failed_note(int pool, size_t size) {
	ms_acquire();
	ms_failed[pool]++;
	ms_failed_bytes[pool] += size;
	ms_release();
	if (!ms_event_done[0]) {
		ms_event_done[0] = 1;
		char tag[96];
		snprintf(tag, sizeof(tag), "first FAILED (pool %d %s, %u KiB asked)", pool, ms_pool_name[pool], (unsigned)(size / 1024));
		vglMemSiteDump(tag);
	}
}
void vgl_ms_recovery(size_t size) {
	if (ms_event_done[1])
		return;
	ms_event_done[1] = 1;
	char tag[96];
	snprintf(tag, sizeof(tag), "first recovery (every pool refused %u KiB)", (unsigned)(size / 1024));
	vglMemSiteDump(tag);
}

void *vgl_malloc(size_t size, vglMemType type) {
	void *res = vgl_malloc_impl(size, type);
	if (__builtin_expect(vgl_ms_on, 0) && type < VGL_MEM_EXTERNAL) {
		if (res)
			ms_note_at(res, size, type, vgl_ms_site ? vgl_ms_site : __builtin_return_address(0), vgl_ms_kind);
		else if (mempool_size[type])
			ms_failed_note(type, size);
	}
	return res;
}
void *vgl_calloc(size_t num, size_t size, vglMemType type) {
	void *res = vgl_calloc_impl(num, size, type);
	if (__builtin_expect(vgl_ms_on, 0) && type < VGL_MEM_EXTERNAL) {
		if (res)
			ms_note_at(res, num * size, type, vgl_ms_site ? vgl_ms_site : __builtin_return_address(0), vgl_ms_kind);
		else if (mempool_size[type])
			ms_failed_note(type, num * size);
	}
	return res;
}
void *vgl_memalign(size_t alignment, size_t size, vglMemType type) {
	void *res = vgl_memalign_impl(alignment, size, type);
	if (__builtin_expect(vgl_ms_on, 0) && type < VGL_MEM_EXTERNAL) {
		if (res)
			ms_note_at(res, size, type, vgl_ms_site ? vgl_ms_site : __builtin_return_address(0), vgl_ms_kind);
		else if (mempool_size[type])
			ms_failed_note(type, size);
	}
	return res;
}
void vgl_free(void *ptr) {
	if (__builtin_expect(vgl_ms_on, 0) && ptr)
		ms_forget(ptr, NULL, NULL, NULL, NULL); // (before the free: the allocator may hand the address out again at once)
	vgl_free_impl(ptr);
}
void *vgl_realloc(void *ptr, size_t size) {
	if (__builtin_expect(!vgl_ms_on, 1) || !ptr)
		return vgl_realloc_impl(ptr, size);
	size_t old_size = 0;
	void *site = NULL;
	int kind = 0, pool = 0;
	const int kept = ms_forget(ptr, &old_size, &site, &kind, &pool);
	void *res = vgl_realloc_impl(ptr, size);
	if (kept) {
		if (res)
			ms_note_at(res, size, pool, site, kind);
		else
			ms_note_at(ptr, old_size, pool, site, kind); // (unchanged)
	}
	return res;
}
/* MEMSITES END */

/* 0044: depth surface blocks shared by framebuffer objects (vglFramebufferDepthShare). The bookkeeping is depth_share_core.h (host-tested); here are the memory and the surface
 * copies. vgl_depth_release is what mark_depth_stencil_as_dirty calls for every depth block: a shared block goes to the collector with its last user. */
#include "depth_share_core.h"
static vgl_dshare_tbl vgl_dshare;
static SceGxmDepthStencilSurface vgl_dshare_surf[VGL_DSHARE_MAX];

void vgl_depth_release(void *p) {
	if (vgl_ds_release(&vgl_dshare, p) == 0)
		return; // other users remain
	mark_as_dirty(p);
}

GLboolean vgl_depth_share_get(int cw, int ch, SceGxmMultisampleMode mode, SceGxmDepthStencilSurface *out, const void *fb) {
	int i = vgl_ds_find(&vgl_dshare, (int)mode, cw, ch);
	if (i < 0) {
		i = vgl_ds_free_slot(&vgl_dshare);
		if (i < 0) {
			vgl_dshare.failed++;
			return GL_FALSE;
		}
		SceGxmDepthStencilSurface s;
		vgl_memset(&s, 0, sizeof(s));
		if (!init_depth_stencil_buffer_ms(cw, ch, &s, GL_FALSE, mode) || !s.depthData) {
			vgl_dshare.failed++;
			return GL_FALSE;
		}
		vgl_ds_add(&vgl_dshare, i, s.depthData, (int)mode, cw, ch);
		vgl_dshare_surf[i] = s;
		vgl_log("%s:%d 0044 depth block %dx%d (mode %d) made\n", __FILE__, __LINE__, cw, ch, (int)mode);
	}
	*out = vgl_dshare_surf[i];
	vgl_ds_join(&vgl_dshare, i);
	return GL_TRUE;
}

// A scene begins on a shared block: if another object drew in it last, what the mask-keep record knows of its content is not the new picture's (the memory is the same, the size is not)
void vgl_depth_share_scene(const void *mem, const void *fb) {
	if (vgl_ds_scene(&vgl_dshare, mem, fb))
		vgl_mk_forget(mem);
}

// Counters for the front end: blocks made, objects joined, joins refused (no slot or no memory), blocks live now
void vglDepthShareStats(uint32_t *out4) {
	uint32_t live = 0;
	for (int i = 0; i < VGL_DSHARE_MAX; i++)
		if (vgl_dshare.rec[i].mem)
			live++;
	out4[0] = vgl_dshare.made;
	out4[1] = vgl_dshare.joined;
	out4[2] = vgl_dshare.failed;
	out4[3] = live;
}
