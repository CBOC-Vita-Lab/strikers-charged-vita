// pthread-backed fake of the sce kernel calls used by vita/np_plat_vita.cpp (see psp2/kernel/*.h here).
// It mimics the semantics the Vita file relies on: counting semaphores with a microsecond timeout (error code on
// timeout), non-recursive light-weight mutexes, thread objects that stay joinable until deleted, ExitThread keeping
// the object and ExitDeleteThread removing it, a priority that can be read back, memory blocks.
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <map>
#include <mutex>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

namespace {
std::mutex g_mu;
int g_next = 100;
struct Sema { sem_t s; };
struct Thr {
    pthread_t th;
    bool started = false, exited = false;
    SceKernelThreadEntry entry = 0;
    char arg[64];
    SceSize arglen = 0;
    int prio = 0, mask = 0, stack = 0;
    char name[32];
    int status = 0;
    void* stackBase = 0;
};
std::map<int, Sema*> g_semas;
std::map<int, Thr*> g_threads;
std::map<int, void*> g_blocks;
std::map<int, size_t> g_sizes;
__thread int tl_id = 0;
Thr* thr(int id) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_threads.find(id);
    return it == g_threads.end() ? 0 : it->second;
}
}  // namespace

extern "C" {
SceUID sceKernelCreateSema(const char*, SceUInt, int initVal, int, SceKernelSemaOptParam*) {
    Sema* s = new Sema;
    sem_init(&s->s, 0, (unsigned)initVal);
    std::lock_guard<std::mutex> l(g_mu);
    int id = g_next++;
    g_semas[id] = s;
    return id;
}
int sceKernelDeleteSema(SceUID id) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_semas.find(id);
    if (it == g_semas.end()) return -1;
    sem_destroy(&it->second->s);
    delete it->second;
    g_semas.erase(it);
    return 0;
}
static Sema* sema(int id) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_semas.find(id);
    return it == g_semas.end() ? 0 : it->second;
}
int sceKernelSignalSema(SceUID id, int n) {
    Sema* s = sema(id);
    if (!s) return -1;
    for (int i = 0; i < n; ++i) sem_post(&s->s);
    return 0;
}
int sceKernelWaitSema(SceUID id, int n, SceUInt* timeout) {
    Sema* s = sema(id);
    if (!s) return -1;
    for (int i = 0; i < n; ++i) {
        if (!timeout) {
            while (sem_wait(&s->s) != 0 && errno == EINTR) {}
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            unsigned long long ns = (unsigned long long)ts.tv_nsec + (unsigned long long)*timeout * 1000ull;
            ts.tv_sec += (time_t)(ns / 1000000000ull);
            ts.tv_nsec = (long)(ns % 1000000000ull);
            int r;
            while ((r = sem_timedwait(&s->s, &ts)) != 0 && errno == EINTR) {}
            if (r != 0) return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
    }
    return 0;
}

int sceKernelCreateLwMutex(SceKernelLwMutexWork* w, const char*, unsigned int, int, const SceKernelLwMutexOptParam*) {
    pthread_mutex_t* m = (pthread_mutex_t*)w;   // 32 bytes hold a pthread_mutex_t on i386 (24)
    static_assert(sizeof(pthread_mutex_t) <= sizeof(SceKernelLwMutexWork), "work area");
    pthread_mutex_init(m, 0);
    return 0;
}
int sceKernelDeleteLwMutex(SceKernelLwMutexWork* w) { return pthread_mutex_destroy((pthread_mutex_t*)w); }
int sceKernelLockLwMutex(SceKernelLwMutexWork* w, int, unsigned int*) { return pthread_mutex_lock((pthread_mutex_t*)w); }
int sceKernelTryLockLwMutex(SceKernelLwMutexWork* w, int) { return pthread_mutex_trylock((pthread_mutex_t*)w) == 0 ? 0 : -1; }
int sceKernelUnlockLwMutex(SceKernelLwMutexWork* w, int) { return pthread_mutex_unlock((pthread_mutex_t*)w); }

static void* thread_main(void* p) {
    Thr* t = (Thr*)p;
    {
        std::lock_guard<std::mutex> l(g_mu);
        for (auto& kv : g_threads) if (kv.second == t) tl_id = kv.first;
    }
    int r = t->entry(t->arglen, t->arg);
    t->status = r;
    t->exited = true;
    return 0;
}
SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int prio, SceSize stackSize, SceUInt, int mask, const SceKernelThreadOptParam*) {
    if (stackSize & 0xfff) return -2;   // the real call wants a multiple of 4 KiB
    Thr* t = new Thr;
    memset(t->arg, 0, sizeof t->arg);
    strncpy(t->name, name ? name : "", sizeof t->name - 1);
    t->name[sizeof t->name - 1] = 0;
    t->entry = entry;
    t->prio = prio;
    t->mask = mask;
    t->stack = (int)stackSize;
    std::lock_guard<std::mutex> l(g_mu);
    int id = g_next++;
    g_threads[id] = t;
    return id;
}
int sceKernelStartThread(SceUID id, SceSize len, void* argp) {
    Thr* t = thr(id);
    if (!t || t->started) return -1;
    t->arglen = len;
    if (len > sizeof t->arg) return -3;
    if (len) memcpy(t->arg, argp, len);
    t->started = true;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (size_t)t->stack < 65536 ? 65536 : (size_t)t->stack);
    int rc = pthread_create(&t->th, &a, thread_main, t);
    pthread_attr_destroy(&a);
    return rc == 0 ? 0 : -4;
}
int sceKernelWaitThreadEnd(SceUID id, int* stat, SceUInt*) {
    Thr* t = thr(id);
    if (!t || !t->started) return -1;
    pthread_join(t->th, 0);
    if (stat) *stat = t->status;
    return 0;
}
int sceKernelDeleteThread(SceUID id) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_threads.find(id);
    if (it == g_threads.end()) return -1;
    delete it->second;
    g_threads.erase(it);
    return 0;
}
int sceKernelExitThread(int status) {
    Thr* t = thr(tl_id);
    if (t) { t->status = status; t->exited = true; }
    pthread_exit(0);
    return 0;
}
int sceKernelExitDeleteThread(int status) {
    int id = tl_id;
    (void)status;
    pthread_t self = pthread_self();
    pthread_detach(self);   // nobody will join a self-deleting thread
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_threads.find(id);
        if (it != g_threads.end()) { delete it->second; g_threads.erase(it); }
    }   // released before pthread_exit: the program has no exceptions, so no destructor runs on the way out
    pthread_exit(0);
    return 0;
}
SceUID sceKernelGetThreadId(void) {
    if (tl_id) return tl_id;
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    static int mainid = 1;
    return mainid;   // the process's main thread
}
int sceKernelGetThreadInfo(SceUID id, SceKernelThreadInfo* info) {
    memset(((char*)info) + sizeof(SceSize), 0, sizeof(*info) - sizeof(SceSize));
    Thr* t = id == 1 ? 0 : thr(id);
    info->threadId = id;
    if (t) {
        info->currentPriority = t->prio;
        info->currentCpuAffinityMask = t->mask;
        info->stackSize = t->stack;
        pthread_attr_t a;
        void* addr = 0; size_t sz = 0;
        if (pthread_getattr_np(t->th, &a) == 0) { pthread_attr_getstack(&a, &addr, &sz); pthread_attr_destroy(&a); }
        info->stack = addr;
    } else {
        info->currentPriority = 0x10000100;   // the main thread
        info->stackSize = 1 << 20;
        info->stack = 0;
    }
    return 0;
}
int sceKernelChangeThreadPriority(SceUID id, int prio) {
    Thr* t = thr(id);
    if (t) t->prio = prio;
    return 0;
}
int sceKernelChangeThreadCpuAffinityMask(SceUID id, int mask) {
    Thr* t = thr(id);
    if (t) t->mask = mask;
    return 0;
}
int sceKernelDelayThread(SceUInt us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000;
    nanosleep(&ts, 0);
    return 0;
}
SceInt64 sceKernelGetSystemTimeWide(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (SceInt64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

SceUID sceKernelAllocMemBlock(const char*, SceKernelMemBlockType, SceSize size, SceKernelAllocMemBlockOpt*) {
    if (size & 0xfff) return -2;
    // The Vita's user memory sits in 0x81000000..0x9FFFFFFF (what the audio mixer's address rule needs): hint there.
    // Deliberately filled with garbage, not zero: the Vita file must clear the block itself.
    static uintptr_t hint = 0x84000000u;
    std::lock_guard<std::mutex> l(g_mu);
    void* p = MAP_FAILED;
#ifdef MAP_FIXED_NOREPLACE
    p = mmap((void*)hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED && (uintptr_t)p != hint) { munmap(p, size); p = MAP_FAILED; }
#endif
    if (p == MAP_FAILED) p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    hint = ((uintptr_t)p + size + 0xfffffu) & ~(uintptr_t)0xfffffu;
    memset(p, 0xA5, size);
    int id = g_next++;
    g_blocks[id] = p;
    g_sizes[id] = size;
    return id;
}
int sceKernelGetMemBlockBase(SceUID id, void** base) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_blocks.find(id);
    if (it == g_blocks.end()) return -1;
    *base = it->second;
    return 0;
}
int sceKernelFreeMemBlock(SceUID id) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_blocks.find(id);
    if (it == g_blocks.end()) return -1;
    munmap(it->second, g_sizes[id]);
    g_blocks.erase(it);
    g_sizes.erase(id);
    return 0;
}
}  // extern "C"
