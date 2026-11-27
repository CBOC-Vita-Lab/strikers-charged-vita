// np_fasttls.cpp (1.3 cpuexact13, MSC_TLS_FAST, default 1): the Vita's emulated thread-local storage without its call chain. See np_fasttls.h for the chain and the layout.
//
// Three link wraps (platform/vita/vita.mk NP_VWRAPS), every one of them a plain forward to the real function until np_tls_fast_init() has proved the fast read on this console:
//   __wrap___getreent            newlib's per-thread reent (errno, stdio, malloc's lock all start here): the value of TLS slot 0x89
//   __wrap_pte_osTlsGetValue     pthread_getspecific: the list walk of np_fasttls.h without the three calls in front of it
//   __wrap___emutls_get_address  every `thread_local` / `__thread` of the game (libgcc's own function is the fallback for the first touch of an object in a thread)
// The slot of sceKernelGetTLSAddr(0x89) is read as (TPIDRURO + offset): `mrc p15,0,r,c13,c0,3` is what np_vita_envlock.cpp already uses (no system call). The offset is not assumed:
// np_tls_fast_init() derives it from the real call, checks it for ten more keys, checks the reent pointer against the real __getreent, runs the pthread-key lookup (and the emutls
// lookup) against the real ones on this thread and on a second thread, and only then flips the switch. Any disagreement leaves the wraps forwarding (the line "[tls] ... off (...)").
// MSC_TLS_FAST=0 never flips it; MSC_TLS_FAST_VERIFY=1 runs both paths on every call and counts the differences (console check: no "[tls] MISMATCH" line, mismatches=0).
#if defined(__vita__)
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include "np_fasttls.h"

extern "C" {
void* __real___getreent(void);
void* __real_pte_osTlsGetValue(unsigned int key);
void* __real___emutls_get_address(void* obj);
int np_tls_fast_init(void);
int np_tls_fast_on(void);
void np_tls_fast_stats(unsigned long* getreent, unsigned long* pte, unsigned long* emutls, unsigned long* mismatch);
}

extern "C" { volatile uintptr_t np_tls_fast_word = 0; }   // 0 = the wraps forward; else (slot address - TPIDRURO) | 1 (the offset is a multiple of 4); np_fasttls.h reads it inline

namespace {

// ONE word per decision, so that a thread that sees "on" also sees the numbers (ARM may reorder two loads; a flag and a separate offset could be read stale):
     // 0 = the wraps forward; else (slot address - TPIDRURO) | 1 (the offset is a multiple of 4)
volatile uint32_t g_emu = 0;       // 0 = __emutls_get_address forwards; else libgcc's emutls pthread key + 1
volatile int g_verify = 0;         // MSC_TLS_FAST_VERIFY
unsigned long g_nGetreent = 0, g_nPte = 0, g_nEmutls = 0, g_nMismatch = 0;   // (verify mode only; racy counts are fine for a diagnostic)

// learn window: the pte wrapper records the keys it sees on the learning thread
volatile uint32_t g_learnTp = 0;
volatile unsigned g_learnN = 0;
volatile uint32_t g_learnKey = 0;

static inline uint32_t Tp() {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}
static inline void* ReentAt(uintptr_t fast) { return *reinterpret_cast<void* const*>(static_cast<uintptr_t>(Tp()) + (fast & ~static_cast<uintptr_t>(1))); }

void Mismatch(const char* what, const void* a, const void* b) {
    if (g_nMismatch == 0) fprintf(stderr, "[tls] MISMATCH in %s: fast %p, library %p\n", what, a, b);
    ++g_nMismatch;
}

}  // namespace

extern "C" void* __wrap___getreent(void) {
    const uintptr_t f = np_tls_fast_word;
    if (__builtin_expect(f != 0, 1)) {
        void* r = ReentAt(f);
        if (__builtin_expect(r != nullptr, 1)) {
            if (__builtin_expect(g_verify != 0, 0)) {
                ++g_nGetreent;
                void* lib = __real___getreent();
                if (lib != r) Mismatch("__getreent", r, lib);
            }
            return r;
        }
    }
    return __real___getreent();
}

extern "C" void* __wrap_pte_osTlsGetValue(unsigned int key) {
    if (__builtin_expect(g_learnTp != 0, 0) && g_learnTp == Tp()) {
        ++g_learnN;
        g_learnKey = key;
    }
    const uintptr_t f = np_tls_fast_word;
    if (__builtin_expect(f != 0, 1)) {
        void* r = ReentAt(f);
        if (__builtin_expect(r != nullptr, 1)) {
            void* v = npfasttls::PteGet(r, key);
            if (__builtin_expect(g_verify != 0, 0)) {
                ++g_nPte;
                void* lib = __real_pte_osTlsGetValue(key);
                if (lib != v) Mismatch("pte_osTlsGetValue", v, lib);
            }
            return v;
        }
    }
    return __real_pte_osTlsGetValue(key);
}

extern "C" void* __wrap___emutls_get_address(void* obj) {
    const uint32_t ek = g_emu;
    const uintptr_t f = np_tls_fast_word;
    if (__builtin_expect(ek != 0 && f != 0, 1)) {
        void* r = ReentAt(f);
        if (__builtin_expect(r != nullptr, 1)) {
            void* p = npfasttls::EmutlsPeek(npfasttls::PteGet(r, ek - 1), reinterpret_cast<const size_t*>(obj)[2]);
            if (__builtin_expect(p != nullptr, 1)) {
                if (__builtin_expect(g_verify != 0, 0)) {
                    ++g_nEmutls;
                    void* lib = __real___emutls_get_address(obj);
                    if (lib != p) Mismatch("__emutls_get_address", p, lib);
                }
                return p;
            }
        }
    }
    return __real___emutls_get_address(obj);
}

namespace {

__thread int t_probeA = 0;   // emutls objects used only by the start-up check (their own accesses go through the wrap above, forwarding until the switch is flipped)
__thread int t_probeB = 0;
// (noinline: the compiler treats the address of a thread-local as constant within a function and would fold the repeated lookups into one)
__attribute__((noinline)) int* ProbeA() { __asm__ volatile("" ::: "memory"); return &t_probeA; }
__attribute__((noinline)) int* ProbeB() { __asm__ volatile("" ::: "memory"); return &t_probeB; }

// the per-thread part of the check: offset from the real slot address, ten more keys, the reent pointer against the real __getreent
bool CheckThread(intptr_t* offOut, const char** why) {
    const uint32_t tp = Tp();
    if (tp == 0) { *why = "TPIDRURO is 0"; return false; }
    const uintptr_t a = reinterpret_cast<uintptr_t>(sceKernelGetTLSAddr(0x89));
    const intptr_t off = static_cast<intptr_t>(a) - static_cast<intptr_t>(tp);
    if (a == 0 || (off & 3) != 0 || off < -0x10000 || off > 0x10000) { *why = "slot 0x89 is not near TPIDRURO"; return false; }
    static const int keys[] = {0, 1, 0x10, 0x7f, 0x80, 0x88, 0x8a, 0x8b, 0xff, 0x20};
    for (int k : keys) {
        const uintptr_t ak = reinterpret_cast<uintptr_t>(sceKernelGetTLSAddr(k));
        if (static_cast<intptr_t>(ak) != static_cast<intptr_t>(tp) + off + 4 * (k - 0x89)) { *why = "slots are not TPIDRURO + off + 4*key"; return false; }
    }
    void* lib = __real___getreent();
    void* fast = *reinterpret_cast<void* const*>(a);
    if (fast == nullptr || fast != lib) { *why = "reent pointer differs from the library's"; return false; }
    *offOut = off;
    return true;
}

struct ThreadArg {
    intptr_t off;
    volatile int done;
    int ok;
    const char* why;
};

void* ThreadCheck(void* p) {
    ThreadArg* t = static_cast<ThreadArg*>(p);
    intptr_t off = 0;
    t->ok = CheckThread(&off, &t->why) && off == t->off;
    if (t->ok) {   // a pthread key value of this thread, set and read back through the list walk and through the library
        pthread_key_t key;
        static int marker;
        if (pthread_key_create(&key, nullptr) == 0) {
            pthread_setspecific(key, &marker);
            void* reent = *reinterpret_cast<void* const*>(static_cast<uintptr_t>(Tp()) + static_cast<uintptr_t>(off));
            void* viaList = npfasttls::PteGet(reent, *reinterpret_cast<unsigned int*>(key));   // (pthread_key_t is a pointer to the key number: pthread_getspecific does ldr r0,[r0])
            void* viaLib = __real_pte_osTlsGetValue(*reinterpret_cast<unsigned int*>(key));
            if (viaList != &marker || viaLib != &marker) { t->ok = 0; t->why = "second thread: key lookup differs"; }
            pthread_setspecific(key, nullptr);
            pthread_key_delete(key);
        }
    }
    t->done = 1;
    return nullptr;
}

}  // namespace

extern "C" int np_tls_fast_on(void) { return np_tls_fast_word != 0; }

extern "C" void np_tls_fast_stats(unsigned long* getreent, unsigned long* pte, unsigned long* emutls, unsigned long* mismatch) {
    if (getreent) *getreent = g_nGetreent;
    if (pte) *pte = g_nPte;
    if (emutls) *emutls = g_nEmutls;
    if (mismatch) *mismatch = g_nMismatch;
}

// MSC_TLS_FAST (default 1), MSC_TLS_FAST_VERIFY (default 0). Call once, on the game's main thread, after the environment is final.
extern "C" int np_tls_fast_init(void) {
    const char* e = getenv("MSC_TLS_FAST");
    if (e && e[0] == '0') { fprintf(stderr, "[tls] fast thread-local lookup: off (MSC_TLS_FAST=0)\n"); return 0; }
    const char* why = "";
    intptr_t off = 0;
    if (!CheckThread(&off, &why)) { fprintf(stderr, "[tls] fast thread-local lookup: off (%s)\n", why); return 0; }
    // a second thread: the offset must be the same there
    ThreadArg ta = {off, 0, 0, ""};
    pthread_t th;
    if (pthread_create(&th, nullptr, ThreadCheck, &ta) != 0) { fprintf(stderr, "[tls] fast thread-local lookup: off (no second thread for the check)\n"); return 0; }
    pthread_join(th, nullptr);
    if (!ta.ok) { fprintf(stderr, "[tls] fast thread-local lookup: off (%s)\n", ta.why ? ta.why : "second thread"); return 0; }
    // (1) pthread_getspecific and __getreent on this thread
    np_tls_fast_word = static_cast<uintptr_t>(off) | 1u;
    {
        pthread_key_t key;
        static int marker;
        if (pthread_key_create(&key, nullptr) == 0) {
            pthread_setspecific(key, &marker);
            void* v = pthread_getspecific(key);   // through the wrapped pte_osTlsGetValue
            pthread_setspecific(key, nullptr);
            pthread_key_delete(key);
            if (v != &marker) { np_tls_fast_word = 0; fprintf(stderr, "[tls] fast thread-local lookup: off (pthread_getspecific through the fast path returned %p, not the stored pointer)\n", v); return 0; }
        }
    }
    // (2) emutls: the pthread key of libgcc's emutls is learned from one real call on a probe object (exactly one pthread_getspecific), then the fast lookup must return the library's pointers
    np_tls_fast_word = 0;   // the learning calls go to the library
    g_emu = 0;
    int* const realA = ProbeA();   // first touches: libgcc creates its key, this thread's array and both entries
    int* const realB = ProbeB();
    *realA = 1;
    *realB = 2;
    g_learnN = 0;
    g_learnTp = Tp();
    int* const again = ProbeB();   // known object, array big enough: the library calls pthread_getspecific(emutls_key) once
    g_learnTp = 0;
    const unsigned seen = g_learnN;
    const uint32_t key = g_learnKey;
    np_tls_fast_word = static_cast<uintptr_t>(off) | 1u;
    int emuOk = 0;
    if (seen == 1 && again == realB && key < 0x7fffffffu) {
        g_emu = key + 1;
        if (ProbeA() == realA && ProbeB() == realB && *ProbeA() == 1 && *ProbeB() == 2) emuOk = 1;
        else g_emu = 0;
    }
    fprintf(stderr, "[tls] fast thread-local lookup: on (slot = TPIDRURO%+ld; 11 slots and a second thread agree); getreent and pthread_getspecific wrapped%s\n", static_cast<long>(off),
            emuOk ? ", emutls wrapped (key learned)" : ", emutls NOT wrapped (key not learned)");
    const char* v = getenv("MSC_TLS_FAST_VERIFY");
    if (v && v[0] && v[0] != '0') {
        g_verify = 1;
        fprintf(stderr, "[tls] verify mode: every wrapped call is compared with the library's (mismatches are printed as \"[tls] MISMATCH\")\n");
    }
    return 1;
}
#endif  // __vita__
