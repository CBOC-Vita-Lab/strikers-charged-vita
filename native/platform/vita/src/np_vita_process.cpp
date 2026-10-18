// Process-level parameters of a Vita executable of the native port: heap size, main-thread stack
// and name, the stack size of threads created through pthread without attributes, the title
// identity, and the game's memory arenas.
//
// Differences from the recompiled build's vita_process.cpp (one/vita/src): no guest address space
// to reserve before the C heap exists (the game runs on native pointers: the _init_vita_heap wrap
// and kubridge's fixed mappings are gone), a smaller C heap (the game allocates from its own
// arenas, NpAllocArena), the same three wraps for VitaSDK's runtime (std::mutex before its
// constructor, remove() on directories, thread stacks).
#if defined(__vita__)

#include "np_vita.h"
#include "np_vita_internal.h"

#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {

// newlib's heap is ONE memory block that the C runtime takes before main() and it is committed
// in full: every MiB here is a MiB of the application's budget whether malloc ever hands it out
// or not. What lives in it: libstdc++/newlib, the SDK layer's tables, the renderer's CPU-side
// data (decoded textures in flight, command buffers, shader sources: vitaGL's own pools are
// separate blocks), thread bookkeeping. The game's own allocations are NOT here: they go to the
// Wii-style arenas (NpAllocArena).
#ifndef NP_VITA_HEAP_MIB
#define NP_VITA_HEAP_MIB 64u
#endif
unsigned int _newlib_heap_size_user = NP_VITA_HEAP_MIB * 1024u * 1024u;

// Which executable this is, in one word, and the title id its package is built with (lab builds
// report both; vita_lab.cpp). MSCN00001 until the native port replaces the recompiled app
// (MSCH00002): build with -DNP_TITLE_ID='"MSCH00002"'.
#if defined(NP_VITA_TAG)
extern const char kMscVitaVariantTag[] = NP_VITA_TAG;
#else
extern const char kMscVitaVariantTag[] = "np";
#endif
#if defined(NP_TITLE_ID)
extern const char kMscVitaTitleId[] = NP_TITLE_ID;
#else
extern const char kMscVitaTitleId[] = "MSCN00001";
#endif

// The main thread runs the whole game frame (simulation, animation, frame building): the Wii's
// call chains plus the SDK layer underneath. The default main stack is 256 KiB; the game's own
// stack was 64 KiB-class on the Wii, 4 MiB leaves a wide margin for code compiled at -O2 with
// larger frames. vita-elf-create looks these symbols up by name.
// The game objects are linked with --gc-sections (np/Makefile OPT has -ffunction-sections -fdata-sections) and nothing in the
// program refers to these two variables: the linker dropped them and the main thread ran on the default 256 KiB stack (first
// Vita3K run of the full game, 9 Oct 22:33; this SDK's GCC ignores the `retain` attribute). Makefile.vpk therefore links
// with -Wl,-u,sceUserMainThreadStackSize -Wl,-u,sceUserMainThreadName, which makes them link roots.
unsigned int sceUserMainThreadStackSize = 4u * 1024u * 1024u;
extern const char sceUserMainThreadName[] = "np_game";   // extern: a const has internal linkage otherwise

// VitaSDK's pthread layer gives a thread created without attributes a 32 KiB stack and std::thread
// never passes attributes. The link wraps pthread_create (-Wl,--wrap=pthread_create) so that
// attribute-less threads (the SDK layer's std::thread helpers) get 1 MiB and move to the helper
// role's core (vita_threads.cpp).
int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*entry)(void*), void* arg) {
    if (attr != nullptr) return __real_pthread_create(thread, attr, entry, arg);
    const bool wrapped = VitaThreads::WrapHelperThread(&entry, &arg);
    pthread_attr_t sized;
    pthread_attr_init(&sized);
    pthread_attr_setstacksize(&sized, 1024 * 1024);
    const int result = __real_pthread_create(thread, &sized, entry, arg);
    pthread_attr_destroy(&sized);
    if (result != 0 && wrapped) std::free(arg);   // (the trampoline's start record)
    return result;
}

// ---------------------------------------------------------------------------
// Three places where VitaSDK's C/C++ runtime is not what portable code expects.
// All found by the first runs in Vita3K (5 Oct); none is specific to the
// emulator. The link redirects the named functions here (-Wl,--wrap=...).
//
// 1. std::mutex and std::condition_variable are not usable before their
//    constructors have run. VitaSDK's <pthread.h> defines
//    _GTHREAD_USE_MUTEX_INIT_FUNC, so std::mutex is not the constant-initialised
//    object the C++ standard describes: its constructor calls
//    pthread_mutex_init, and until then the object is all zero, which
//    pthread-embedded answers with EINVAL from lock/trylock (std::mutex::lock
//    then throws std::system_error) and with a null dereference in unlock.
//    The runtime was written for platforms where a mutex of static storage
//    duration works from the first instruction of the program: its thousands of
//    registration constructors, and this layer's early boot (init_priority 101,
//    vita_main.cpp), lock mutexes that belong to translation units initialised
//    later. So an all-zero mutex or condition variable is given the value of the
//    static initialiser here, which pthread-embedded turns into a real object on
//    first use. When the owner's constructor runs later it replaces that object
//    with a fresh one; the first is leaked (once per such mutex; never while
//    locked, because static initialisation is single-threaded and no lock is
//    held across another translation unit's constructors).
namespace {
inline void AdoptMutex(pthread_mutex_t* mutex) {
    if (mutex && *mutex == nullptr) __sync_bool_compare_and_swap(mutex, static_cast<pthread_mutex_t>(nullptr), PTHREAD_MUTEX_INITIALIZER);
}
inline void AdoptCond(pthread_cond_t* cond) {
    if (cond && *cond == nullptr) __sync_bool_compare_and_swap(cond, static_cast<pthread_cond_t>(nullptr), PTHREAD_COND_INITIALIZER);
}
} // namespace
int __real_pthread_mutex_lock(pthread_mutex_t*);
int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    AdoptMutex(mutex);
    return __real_pthread_mutex_lock(mutex);
}
int __real_pthread_mutex_trylock(pthread_mutex_t*);
int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    AdoptMutex(mutex);
    return __real_pthread_mutex_trylock(mutex);
}
int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int __wrap_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    AdoptCond(cond);
    return __real_pthread_cond_wait(cond, mutex);
}
int __real_pthread_cond_timedwait(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
int __wrap_pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* until) {
    AdoptCond(cond);
    return __real_pthread_cond_timedwait(cond, mutex, until);
}
int __real_pthread_cond_signal(pthread_cond_t*);
int __wrap_pthread_cond_signal(pthread_cond_t* cond) {
    AdoptCond(cond);
    return __real_pthread_cond_signal(cond);
}
int __real_pthread_cond_broadcast(pthread_cond_t*);
int __wrap_pthread_cond_broadcast(pthread_cond_t* cond) {
    AdoptCond(cond);
    return __real_pthread_cond_broadcast(cond);
}

// 2. remove() does not remove directories. POSIX: remove(path) is unlink for a
//    file and rmdir for a directory. VitaSDK's newlib only calls sceIoRemove,
//    which fails on a directory, so std::filesystem::remove(directory) fails,
//    and std::filesystem::remove_all cannot work at all: on this target
//    libstdc++ has neither unlinkat nor a file type from readdir, so it removes
//    every entry with remove() and only treats the entry as a directory when
//    that fails with EPERM or EISDIR. With any other error it reports failure;
//    with ENOENT (which is what Vita3K's sceIoRemove returns for a directory
//    that is not empty) its loop never ends. The runtime uses remove_all at
//    start-up (pruning Logs/ folders older than four days) and remove on
//    directories in the NAND layer.
//    Here: an empty directory is removed; one that is not empty fails with
//    EISDIR, which is the answer libstdc++ recurses on.
int __real_remove(const char*);
int __wrap_remove(const char* path) {
    if (__real_remove(path) == 0) return 0;
    const int error = errno;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errno = error;
        return -1;
    }
    if (rmdir(path) == 0) return 0;
    errno = EISDIR;
    return -1;
}

} // extern "C"

// ---- arenas ---------------------------------------------------------------------------------
// A USER_RW block of its own: counted in the memory report, outside the C heap, 4 KiB aligned
// (the system's granularity); a larger alignment is served by over-allocating. The block is not
// zeroed by the system: the game's allocator writes its own fill.
extern "C" void* NpAllocArena(size_t bytes, size_t alignment, const char* name) {
    if (alignment < 4096) alignment = 4096;
    const size_t request = (bytes + alignment - 1) & ~(alignment - 1);
    const SceUID uid = sceKernelAllocMemBlock(name ? name : "np_arena", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, static_cast<SceSize>(request), nullptr);
    if (uid < 0) {
        std::fprintf(stderr, "[vita] arena \"%s\" (%u KiB): sceKernelAllocMemBlock failed 0x%08X\n", name ? name : "?",
                     static_cast<unsigned>(bytes / 1024u), static_cast<unsigned>(uid));
        return nullptr;
    }
    void* base = nullptr;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) return nullptr;
    std::fprintf(stderr, "[vita] arena \"%s\": %u KiB at %p\n", name ? name : "?", static_cast<unsigned>(request / 1024u), base);
    if (VitaMem::RegisterArena) VitaMem::RegisterArena(name, base, request);
    return base;
}

#endif // __vita__
