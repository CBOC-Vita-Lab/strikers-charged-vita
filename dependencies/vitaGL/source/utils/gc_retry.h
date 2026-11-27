/* 0036: the retry loop of a failed GPU allocation (the recovery that patch 0005 made), as plain C over a table of operations so a host test can run it with a model
 * allocator and a model clock (renderer/tests/run_vitagl_gcretry_test.sh includes this file as it is). In the library the operations are sceGxmFinish / glFinish, one
 * acknowledged collector cycle, the pool allocation, sceKernelDelayThread and the system clock.
 *
 * Before 0036 every level of the recursion that follows the collector cycles ended with sceKernelDelayThread(1000000) and one more try: a failed allocation could stop the
 * calling thread for up to FRAME_PURGE_FREQ seconds. Now: finish the GPU once (nothing that was drawn may be freed under it), then up to max_cycles collector cycles
 * with a try after each (a cycle frees every retired block of its bucket, the deferred texture / buffer / target frees, no waiting), and only if that did not give the
 * memory a timed wait of at most pause_us in steps of step_us, a try after each step (memory another thread gives back). pause_us 0: no wait at all. */
#ifndef VGL_GC_RETRY_H
#define VGL_GC_RETRY_H

#include <stddef.h>
#include <stdint.h>

typedef struct vgl_gc_stats {
	uint32_t recoveries; // allocations that came here (every pool had refused)
	uint32_t resolved; // ... that got their memory
	uint32_t failed; // ... that did not (the caller sees NULL)
	uint32_t pauses; // recoveries that needed the timed wait
	uint32_t legacy; // collector not running yet: one plain wait instead of a cycle
	uint32_t cycles; // collector cycles forced
	uint32_t max_us; // longest recovery, the GPU finish and the waits included
	uint64_t total_us; // time spent in recoveries on the failing threads
	uint64_t pause_us; // ... of which in the timed wait
	uint64_t bytes; // bytes asked by the allocations that came here
} vgl_gc_stats;

typedef struct vgl_gc_ops {
	void *ctx;
	void (*finish)(void *ctx); // the GPU is idle after this (first cycle only)
	int (*cycle)(void *ctx); // one collector cycle: 0 failed, 1 ran without an acknowledgement (no wait done), 2 acknowledged
	void *(*attempt)(void *ctx); // the allocation in every pool
	void (*delay_us)(void *ctx, uint32_t us);
	uint64_t (*now_us)(void *ctx);
} vgl_gc_ops;

static inline void *vgl_gc_retry(const vgl_gc_ops *ops, vgl_gc_stats *st, size_t size, int max_cycles, uint32_t pause_us, uint32_t step_us) {
	const uint64_t t0 = ops->now_us(ops->ctx);
	uint32_t cycles = 0;
	int last = 2;
	void *res = NULL;
	__atomic_add_fetch(&st->recoveries, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&st->bytes, (uint64_t)size, __ATOMIC_RELAXED);
	for (int n = 1; n <= max_cycles; n++) {
		if (n == 1)
			ops->finish(ops->ctx);
		last = ops->cycle(ops->ctx);
		if (!last)
			break;
		cycles++;
		res = ops->attempt(ops->ctx);
		if (res)
			break;
	}
	uint64_t waited = 0;
	if (!res && last == 2 && pause_us) {
		const uint64_t p0 = ops->now_us(ops->ctx);
		if (step_us == 0)
			step_us = 1000;
		for (;;) {
			const uint64_t el = ops->now_us(ops->ctx) - p0;
			if (el >= pause_us)
				break;
			const uint64_t left = pause_us - el;
			ops->delay_us(ops->ctx, left < step_us ? (uint32_t)left : step_us);
			res = ops->attempt(ops->ctx);
			if (res)
				break;
		}
		waited = ops->now_us(ops->ctx) - p0;
		__atomic_add_fetch(&st->pauses, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&st->pause_us, waited, __ATOMIC_RELAXED);
	}
	const uint64_t total = ops->now_us(ops->ctx) - t0;
	__atomic_add_fetch(&st->cycles, cycles, __ATOMIC_RELAXED);
	__atomic_add_fetch(res ? &st->resolved : &st->failed, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&st->total_us, total, __ATOMIC_RELAXED);
	uint32_t m = __atomic_load_n(&st->max_us, __ATOMIC_RELAXED);
	while (total > m && !__atomic_compare_exchange_n(&st->max_us, &m, (uint32_t)total, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
	return res;
}

#endif
