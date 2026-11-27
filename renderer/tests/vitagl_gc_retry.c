// Host test of vitaGL's bounded GPU-allocation recovery (patch 0035, utils/gc_retry.h included as it is). A model: a "GPU" that is busy until finish is called, retired blocks that
// only a collector cycle may free (and only when the GPU is idle: a cycle on a busy GPU is the use-after-free this ordering prevents), a clock that only the waits advance.
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "gc_retry.h"

typedef struct {
	int gpu_busy, finishes, cycles, attempts, delays;
	uint64_t clock, delayed_us;
	int free_after_cycles;      // memory appears after this many cycles (0 never)
	uint64_t free_after_us;     // ... or after this much waiting (0 never): another thread freed it
	int cycle_result;           // what a cycle answers
	int busy_cycle_violations;  // a cycle ran while the GPU was busy
	int attempt_before_finish;
	int available;
} Model;

static void m_finish(void *c) { Model *m = c; m->gpu_busy = 0; m->finishes++; }
static int m_cycle(void *c) {
	Model *m = c;
	if (m->gpu_busy) m->busy_cycle_violations++;
	m->cycles++;
	if (m->free_after_cycles && m->cycles >= m->free_after_cycles) m->available = 1;
	return m->cycle_result;
}
static void *m_attempt(void *c) {
	Model *m = c;
	m->attempts++;
	if (m->gpu_busy) m->attempt_before_finish++;
	if (m->free_after_us && m->delayed_us >= m->free_after_us) m->available = 1;
	return m->available ? (void *)m : NULL;
}
static void m_delay(void *c, uint32_t us) { Model *m = c; m->delays++; m->delayed_us += us; m->clock += us; if (m->delays > 100000) { printf("FAIL: runaway wait (%llu us asked for, limit not honoured)\n", (unsigned long long)m->delayed_us); exit(1); } }
static uint64_t m_now(void *c) { return ((Model *)c)->clock; }

static Model fresh(void) { Model m = {0}; m.gpu_busy = 1; m.cycle_result = 2; return m; }
static void *run(Model *m, vgl_gc_stats *st, uint32_t pause_us) {
	vgl_gc_ops ops = {m, m_finish, m_cycle, m_attempt, m_delay, m_now};
	return vgl_gc_retry(&ops, st, 4096, 4, pause_us, 2000);
}

static void *worker(void *a) {
	vgl_gc_stats *st = a;
	for (int i = 0; i < 20000; i++) { Model m = fresh(); m.free_after_cycles = 2; assert(run(&m, st, 16000)); }
	return NULL;
}

int main(void) {
	int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
	// 1. memory appears after k collector cycles, k = 1..4: no wait, the GPU finished once, before the first cycle
	for (int k = 1; k <= 4; k++) {
		Model m = fresh(); vgl_gc_stats st = {0}; m.free_after_cycles = k;
		void *r = run(&m, &st, 16000);
		CHECK(r && m.cycles == k && m.finishes == 1 && m.delays == 0 && !m.busy_cycle_violations && !m.attempt_before_finish, "k=%d cycles %d finishes %d delays %d busy %d", k, m.cycles, m.finishes, m.delays, m.busy_cycle_violations);
		CHECK(st.recoveries == 1 && st.resolved == 1 && st.failed == 0 && st.pauses == 0 && st.cycles == (uint32_t)k && st.bytes == 4096, "stats k=%d", k);
	}
	// 2. only a wait helps (another thread frees after 7 ms): resolved inside the limit, in steps of 2 ms
	{
		Model m = fresh(); vgl_gc_stats st = {0}; m.free_after_us = 7000;
		void *r = run(&m, &st, 16000);
		CHECK(r && st.pauses == 1 && st.pause_us == 8000 && m.delays == 4 && st.max_us == 8000 && st.total_us == 8000, "wait: pauses %u pause_us %llu delays %d", st.pauses, (unsigned long long)st.pause_us, m.delays);
	}
	// 3. never: bounded by the limit (the old behaviour was 4 levels x 1 s); the caller gets NULL, counted failed
	{
		Model m = fresh(); vgl_gc_stats st = {0};
		void *r = run(&m, &st, 16000);
		CHECK(!r && m.delayed_us == 16000 && st.failed == 1 && st.resolved == 0 && st.pauses == 1 && st.max_us == 16000 && m.cycles == 4 && m.attempts == 4 + 8, "never: delayed %llu attempts %d", (unsigned long long)m.delayed_us, m.attempts);
	}
	// 3b. the limit is honoured for odd values (5 ms with 2 ms steps: 2+2+1) and at 1000 ms
	{
		Model m = fresh(); vgl_gc_stats st = {0};
		run(&m, &st, 5000);
		CHECK(m.delayed_us == 5000 && m.delays == 3, "5 ms: %llu in %d", (unsigned long long)m.delayed_us, m.delays);
		Model n = fresh(); vgl_gc_stats s2 = {0};
		run(&n, &s2, 1000000);
		CHECK(n.delayed_us == 1000000, "1000 ms: %llu", (unsigned long long)n.delayed_us);
	}
	// 4. limit 0: no wait at all
	{
		Model m = fresh(); vgl_gc_stats st = {0};
		void *r = run(&m, &st, 0);
		CHECK(!r && m.delays == 0 && st.pauses == 0 && m.cycles == 4, "no wait");
	}
	// 5. a cycle that failed: NULL at once, nothing tried; a cycle with no acknowledgement (collector not running: it did its own wait): no second wait
	{
		Model m = fresh(); vgl_gc_stats st = {0}; m.cycle_result = 0;
		void *r = run(&m, &st, 16000);
		CHECK(!r && m.attempts == 0 && m.delays == 0 && st.failed == 1, "cycle failed");
		Model n = fresh(); vgl_gc_stats s2 = {0}; n.cycle_result = 1;
		r = run(&n, &s2, 16000);
		CHECK(!r && n.delays == 0 && s2.pauses == 0, "legacy cycle: no extra wait");
	}
	// 6. the retry stops at the first success: later cycles are not run (memory is not collected more than needed)
	{
		Model m = fresh(); vgl_gc_stats st = {0}; m.free_after_cycles = 1;
		run(&m, &st, 16000);
		CHECK(m.cycles == 1 && m.attempts == 1, "stops at the first success");
	}
	// 7. four threads, atomic counters add up
	{
		vgl_gc_stats st = {0}; pthread_t t[4];
		for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, &st);
		for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
		CHECK(st.recoveries == 80000 && st.resolved == 80000 && st.cycles == 160000 && st.bytes == 80000ull * 4096, "threads: %u %u %u", st.recoveries, st.resolved, st.cycles);
	}
	printf(fails ? "gc_retry test: %d FAILED\n" : "gc_retry test: all checks passed\n", fails);
	return fails ? 1 : 0;
}
