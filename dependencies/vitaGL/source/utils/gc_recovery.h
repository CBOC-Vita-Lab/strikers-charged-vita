/* Internal to gxm.c. The ordinary frame path stays asynchronous. */
#ifndef VGL_GC_RECOVERY_H
#define VGL_GC_RECOVERY_H

static uint32_t gc_requested;
static uint32_t gc_completed;
static int gc_ready;
static int gc_broken;
static const char *volatile gc_failure_operation;
static volatile int gc_failure_error;

static __attribute__((noreturn, noinline)) void gc_failure(const char *operation, int error) {
	if (!__atomic_exchange_n(&gc_broken, 1, __ATOMIC_ACQ_REL)) {
		gc_failure_operation = operation;
		gc_failure_error = error;
	}
	// A dead collector cannot accept more retirement entries. Exit without heap,
	// scene or log work; the app's exception UI can otherwise leave threads running.
	// The diagnostic fields are best effort for a debugger, not a persistent log.
	sceKernelExitProcess(1);
	__builtin_trap();
}

int vgl_gc_failed(void) {
	// An observer must not keep running while the failing thread is preempted.
	if (__atomic_load_n(&gc_broken, __ATOMIC_ACQUIRE))
		gc_failure("observed failure", -1);
	return 0;
}

static int gc_return_credit(void) {
	int res = sceKernelSignalSema(gc_mutex[1], 1);
	if (res < 0) gc_failure("return credit", res);
	return res >= 0;
}

static int gc_enqueue(uint32_t *ticket) {
	int res;
	for (;;) {
		if (vgl_gc_failed()) return 0;
		unsigned int timeout = 100000;
		res = sceKernelWaitSema(gc_mutex[1], 1, &timeout);
		if (res != (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT) break;
	}
	if (res < 0) {
		gc_failure("take credit", res);
		return 0;
	}
	if (vgl_gc_failed()) {
		gc_return_credit();
		return 0;
	}
	// Every submitted bucket job is counted, including pre-ready legacy retries.
	uint32_t target = __atomic_add_fetch(&gc_requested, 1, __ATOMIC_RELAXED);
	res = sceKernelSignalSema(gc_mutex[0], 1);
	if (res < 0) {
		gc_failure("post request", res);
		gc_return_credit();
		return 0;
	}
	if (ticket) *ticket = target;
	return 1;
}

static void gc_publish_completion(void) {
	if (gc_return_credit())
		__atomic_add_fetch(&gc_completed, 1, __ATOMIC_RELEASE);
}

static void gc_mark_ready(int started) {
	if (gc_mutex[0] < 0)
		gc_failure("create request semaphore", gc_mutex[0]);
	else if (gc_mutex[1] < 0)
		gc_failure("create credit semaphore", gc_mutex[1]);
	else if (started < 0)
		gc_failure("start worker", started);
	else if (!vgl_gc_failed())
		__atomic_store_n(&gc_ready, 1, __ATOMIC_RELEASE);
}

int vgl_gc_retry_pause(void) {
	if (vgl_gc_failed()) return 0;
	int res = sceKernelDelayThread(1000000);
	if (res < 0) gc_failure("delayed retry", res);
	return !vgl_gc_failed();
}

// 0: synchronization failure; 1: legacy delayed collection; 2: acknowledged.
int vgl_gc_recover(void) {
	// Allocations can fail before init_gxm has created a queue or a worker.
	if (!__atomic_load_n(&gc_ready, __ATOMIC_ACQUIRE))
		return vgl_gc_retry_pause() ? 1 : 0;
	uint32_t target;
	if (!gc_enqueue(&target)) return 0;
	while (!vgl_gc_failed()) {
		uint32_t done = __atomic_load_n(&gc_completed, __ATOMIC_ACQUIRE);
		// Credits plus one unpublished completion bound the distance well below
		// half the sequence space, including when the counter wraps through zero.
		// This counts completed bucket advances, not ownership of a GC request.
		if ((uint32_t)(done - target) < UINT32_C(0x80000000)) return 2;
		int res = sceKernelDelayThread(1000);
		if (res < 0) gc_failure("completion wait", res);
	}
	return 0;
}

#endif
