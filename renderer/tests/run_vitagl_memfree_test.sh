#!/bin/bash
# 1.3 MSC_VGX_VGL_MEMFREE (vitaGL patch 0028): the free figure of vitaGL's memory pools from running counters instead of a walk of the heap.
# The counters, the probes and the allocator wrappers (vgl_mf_*, vgl_malloc / calloc / memalign / realloc / free, vgl_mem_get_free_space) are cut out of
# dependencies/vitaGL/source/utils/mem_utils.c (not a copy) and run over a model of the console's mspace allocator: dlmalloc-style chunks (header 4 bytes, 8-byte
# rounding, 16-byte minimum, first fit over a sorted block list, memalign with a freed lead-in, realloc in place when the next gap allows), with the statistics call
# that walks every block (the cost being removed) and a usable-size call. Checks:
#   1. 300000 random operations on three pools (malloc, calloc, memalign of 16..4096, realloc grow/shrink/move, free): after EVERY one the counter answer equals the
#      walked answer (mode 1 against capacity - current_in_use), and in mode 2 the checking path reports 0 mismatches;
#   2. three threads allocate and free at once with mode 2 on: 0 mismatches (checks across a change are counted as raced, not compared), and once they stop the
#      counters equal the walk;
#   3. an allocator whose statistics count REQUESTED bytes (no constant overhead) and one with no statistics (the host emulator) are refused by the probes and keep
#      answering by the walk (the answer is still right, and the counters are not used);
#   4. the number of block visits the walk makes grows with the live blocks while the counters answer in constant time (printed).
# BREAK=1: realloc forgets to adjust the usable bytes; BREAK=2: the per-block overhead is ignored; BREAK=3: free does not subtract. Each must fail.
set -euo pipefail
VGX=$(cd "$(dirname "$0")/.." && pwd)
SRC=${MEM_UTILS_C:-$VGX/../dependencies/vitaGL/source/utils/mem_utils.c}
BUILD=${TEST_BUILD:-$(mktemp -d)}
mkdir -p "$BUILD"
python3 - "$SRC" "$BUILD" "${BREAK:-0}" <<'PY'
import sys
src = open(sys.argv[1]).read(); brk = sys.argv[3]
def func(sig):
    i = src.index(sig); j = src.index('\n}\n', i) + 3
    return src[i:j]
top = src[src.index('GLboolean has_cached_mem'):src.index('#ifdef HAVE_WRAPPED_ALLOCATORS')]
mf = src[src.index('#ifndef HAVE_CUSTOM_HEAP\n/* MEMFREE BEGIN */'):src.index('/* MEMFREE END */')] + '/* MEMFREE END */\n#endif\n'
if brk == '1':
    t = 'vgl_mf_end(type, tr, res ? (int32_t)sceClibMspaceMallocUsableSize(res) - before : 0, 0);'; assert t in src
    mf_src = None
body = ''
# (0049+ memsites: the allocator bodies are the static *_impl functions, the public names are wrappers that take one branch on vgl_ms_on, off in this model)
for sig in ('vglMemType vgl_mem_get_type_by_addr(', 'size_t vgl_mem_get_free_space(', 'size_t vgl_malloc_usable_size(',
            'static void vgl_free_impl(', 'static void *vgl_malloc_impl(', 'static void *vgl_calloc_impl(', 'static void *vgl_memalign_impl(', 'static void *vgl_realloc_impl(',
            'void vgl_free(', 'void *vgl_malloc(', 'void *vgl_calloc(', 'void *vgl_memalign(', 'void *vgl_realloc('):
    body += func(sig) + '\n'
if brk == '1':
    t = 'vgl_mf_end(type, tr, res ? (int32_t)sceClibMspaceMallocUsableSize(res) - before : 0, 0);'; assert t in body
    body = body.replace(t, 'vgl_mf_end(type, tr, 0, 0);')
elif brk == '2':
    t = 'const uint32_t used = usable + mf_overhead[type] * live;'; assert t in mf
    mf = mf.replace(t, 'const uint32_t used = usable;')
elif brk == '3':
    t = 'vgl_mf_end(type, tr, -u, -1);'; assert t in body
    body = body.replace(t, 'vgl_mf_end(type, tr, 0, 0);')
open(sys.argv[2] + '/fixture.inc', 'w').write(top + mf + body)
PY
cat > "$BUILD/test.c" <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
typedef unsigned char GLboolean;
#define GL_TRUE 1
#define GL_FALSE 0
typedef enum { VGL_MEM_VRAM, VGL_MEM_RAM, VGL_MEM_PHYCONT, VGL_MEM_BUDGET, VGL_MEM_EXTERNAL, VGL_MEM_ALL } vglMemType;
#define vgl_log(...) (g_quiet ? (void)0 : (void)printf(__VA_ARGS__))
static int g_quiet = 1;
int vgl_ms_on = 0; void *vgl_ms_site = NULL; uint8_t vgl_ms_kind = 0;   /* the allocation-site table (MSC_VGL_MEMSITES) is off in this model */
static void ms_note_at(void *ptr, size_t size, int pool, void *site, int kind) {}
static int ms_forget(void *ptr, size_t *size, void **site, int *kind, int *pool) { return 0; }
static void ms_failed_note(int pool, size_t size) {}
typedef void *SceClibMspace;
typedef uint32_t SceSize;
typedef struct { SceSize capacity, unk, peak_in_use, current_in_use; } SceClibMspaceStats;
#define vgl_fast_memcpy memcpy
#define VGL_ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))

// ---- the model of the console's mspace: blocks (offset, chunk size) sorted by offset; chunk = max(16, round8(request + 4)), usable = chunk - 4;
// current_in_use = sum of chunks + 32 (the allocator's own top chunk bookkeeping, there from the start)
#define MAXB 70000
typedef struct { uint32_t off, chunk; } blk_t;
typedef struct { uint8_t *base; uint32_t size; blk_t b[MAXB]; int n; int style; /* 0 dlmalloc-like, 1 statistics count requested bytes, 2 no statistics */ uint64_t reqsum; pthread_mutex_t m; uint64_t visits; } ms_t;
static uint32_t chunk_of(uint32_t req) { uint32_t c = (req + 4 + 7) & ~7u; return c < 16 ? 16 : c; }
static int find_gap(ms_t *m, uint32_t chunk, uint32_t align, uint32_t *at) {
    uint32_t pos = 0; int i;
    for (i = 0; i <= m->n; i++) {
        uint32_t end = i < m->n ? m->b[i].off : m->size;
        uint32_t p = pos;
        if (align > 8) { // memalign: the payload (offset + 4) aligned; the lead-in, if any, must be a free chunk of 16 at least
            uint32_t pay = (uint32_t)(((uintptr_t)m->base + p + 4 + align - 1) & ~(uintptr_t)(align - 1)) - (uint32_t)(uintptr_t)m->base;
            uint32_t off = pay - 4;
            while (off != p && off - p < 16) { pay += align; off = pay - 4; }
            p = off;
        }
        if (p + chunk <= end) { *at = p; return i; }
        pos = i < m->n ? m->b[i].off + m->b[i].chunk : pos;
    }
    return -1;
}
static void *ms_alloc(ms_t *m, uint32_t req, uint32_t align) {
    if (req > 0x7fffffffu) return NULL;
    pthread_mutex_lock(&m->m);
    uint32_t c = chunk_of(req), at; int i = find_gap(m, c, align, &at);
    void *r = NULL;
    if (i >= 0 && m->n < MAXB) {
        memmove(&m->b[i + 1], &m->b[i], (m->n - i) * sizeof(blk_t));
        m->b[i].off = at; m->b[i].chunk = c; m->n++; m->reqsum += req;
        r = m->base + at + 4;
    }
    pthread_mutex_unlock(&m->m);
    return r;
}
static int ms_index(ms_t *m, void *p) { // (lock held) binary search by offset
    uint32_t off = (uint32_t)((uint8_t *)p - 4 - m->base); int lo = 0, hi = m->n - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (m->b[mid].off == off) return mid; if (m->b[mid].off < off) lo = mid + 1; else hi = mid - 1; }
    return -1;
}
SceClibMspace sceClibMspaceCreate(void *base, SceSize size) { ms_t *m = calloc(1, sizeof *m); m->base = base; m->size = size; pthread_mutex_init(&m->m, NULL); return m; }
void *sceClibMspaceMalloc(SceClibMspace s, SceSize n) { return ms_alloc(s, n, 0); }
void *sceClibMspaceCalloc(SceClibMspace s, SceSize a, SceSize b) { void *p = ms_alloc(s, a * b, 0); if (p) memset(p, 0, a * b); return p; }
void *sceClibMspaceMemalign(SceClibMspace s, SceSize al, SceSize n) { return ms_alloc(s, n, al); }
void sceClibMspaceFree(SceClibMspace s, void *p) {
    ms_t *m = s; pthread_mutex_lock(&m->m); int i = ms_index(m, p);
    if (i < 0) { puts("MODEL: free of an unknown block"); exit(2); }
    m->reqsum -= m->b[i].chunk - 4; // (styles 1: the request is not kept; the usable size stands in for it)
    memmove(&m->b[i], &m->b[i + 1], (m->n - i - 1) * sizeof(blk_t)); m->n--; pthread_mutex_unlock(&m->m);
}
SceSize sceClibMspaceMallocUsableSize(void *p);
static ms_t *g_ms[4];
static ms_t *ms_of(void *p) { for (int i = 0; i < 4; i++) if (g_ms[i] && (uint8_t *)p >= g_ms[i]->base && (uint8_t *)p < g_ms[i]->base + g_ms[i]->size) return g_ms[i]; return NULL; }
SceSize sceClibMspaceMallocUsableSize(void *p) { ms_t *m = ms_of(p); pthread_mutex_lock(&m->m); int i = ms_index(m, p); uint32_t u = i < 0 ? 0 : m->b[i].chunk - 4; pthread_mutex_unlock(&m->m); return u; }
void *sceClibMspaceRealloc(SceClibMspace s, void *p, SceSize n) {
    ms_t *m = s; pthread_mutex_lock(&m->m); int i = ms_index(m, p); uint32_t c = chunk_of(n);
    uint32_t next = i + 1 < m->n ? m->b[i + 1].off : m->size;
    if (c <= m->b[i].chunk || m->b[i].off + c <= next) { // in place (a shrink keeps the chunk when the rest would be below the minimum)
        if (c < m->b[i].chunk && m->b[i].chunk - c >= 16) m->b[i].chunk = c; else if (c > m->b[i].chunk) m->b[i].chunk = c;
        pthread_mutex_unlock(&m->m); return p;
    }
    pthread_mutex_unlock(&m->m);
    void *q = ms_alloc(m, n, 0); if (!q) return NULL;
    pthread_mutex_lock(&m->m); i = ms_index(m, p); uint32_t keep = m->b[i].chunk - 4; pthread_mutex_unlock(&m->m);
    memcpy(q, p, keep < n ? keep : n); sceClibMspaceFree(m, p); return q;
}
static uint64_t g_walk_calls, g_walk_visits;
void sceClibMspaceMallocStats(SceClibMspace s, SceClibMspaceStats *st) { // the walk
    ms_t *m = s; pthread_mutex_lock(&m->m); uint64_t sum = 32;
    for (int i = 0; i < m->n; i++) sum += m->b[i].chunk;
    g_walk_calls++; g_walk_visits += m->n;
    if (m->style == 2) { st->capacity = 0; st->current_in_use = 0; }
    else { st->capacity = m->size; st->unk = m->size; st->current_in_use = m->style == 1 ? (uint32_t)(m->reqsum + 32) : (uint32_t)sum; }
    pthread_mutex_unlock(&m->m);
}
#include "fixture.inc"

#define POOL (1u << 24)
static uint8_t arena[3][POOL + 8192];
static void make_pools(int style) {
    memset(mempool_size, 0, sizeof mempool_size);
    for (int t = 0; t < 3; t++) {
        uint8_t *base = (uint8_t *)VGL_ALIGN((uintptr_t)arena[t], 4096);
        mempool_addr[t] = base; mempool_size[t] = POOL; mempool_end[t] = base + POOL;
        mempool_mspace[t] = sceClibMspaceCreate(base, POOL);
        g_ms[t] = mempool_mspace[t]; ((ms_t *)g_ms[t])->style = style;
        vgl_mf_init((vglMemType)t);
    }
    mempool_addr[3] = mempool_end[3] = NULL; mempool_mspace[3] = NULL; mempool_size[3] = 0;
}
static unsigned rnd(void) { static unsigned s = 987654321; s = s * 1664525u + 1013904223u; return s >> 6; }
static long bad;
#define EXPECT(c, ...) do { if (!(c)) { if (bad++ < 6) { printf(__VA_ARGS__); putchar('\n'); } } } while (0)
static void same(const char *when) {
    for (int t = 0; t < 3; t++) {
        const size_t fast = vgl_mem_get_free_space((vglMemType)t), walk = vgl_mf_walk((vglMemType)t);
        EXPECT(fast == walk, "%s: pool %d: counters %zu, walk %zu", when, t, fast, walk);
    }
}
typedef struct { void *p; vglMemType t; } live_t;
static live_t live[20000];
static int nlive;
static uint64_t steps;
static void one_op(unsigned r, int checkEach) {
    const vglMemType t = (vglMemType)(rnd() % 3);
    const unsigned op = r % 16;
    if ((op < 7 || nlive == 0) && nlive < 20000) {
        const unsigned sz = (rnd() % 4 == 0) ? 1 + rnd() % 200000 : 1 + rnd() % 900;
        void *p;
        if (op % 4 == 0) p = vgl_calloc(1, sz, t);
        else if (op % 4 == 1) { static const unsigned al[] = {16, 64, 256, 4096}; p = vgl_memalign(al[rnd() % 4], sz, t); }
        else p = vgl_malloc(sz, t);
        if (p) { live[nlive].p = p; live[nlive].t = t; nlive++; }
    } else if (op < 11) {
        const int k = rnd() % nlive; void *q = vgl_realloc(live[k].p, 1 + (rnd() % 3 ? rnd() % 3000 : rnd() % 300000));
        if (q) live[k].p = q;
    } else {
        const int k = rnd() % nlive; vgl_free(live[k].p); live[k] = live[--nlive];
    }
    ++steps;
    if (checkEach) same("after op");
}
typedef struct { int id; } targ;
static volatile int stop_threads;
static void *worker(void *a) {
    unsigned s = 12345u + 77u * (unsigned)(uintptr_t)a; void *mine[300]; int n = 0;
    while (!stop_threads) {
        s = s * 1664525u + 1013904223u; vglMemType t = (vglMemType)((s >> 8) % 3);
        if (n < 300 && ((s >> 12) & 3)) { void *p = (s >> 20) & 1 ? vgl_malloc(1 + (s >> 3) % 5000, t) : vgl_memalign(256, 1 + (s >> 3) % 5000, t); if (p) mine[n++] = p; }
        else if (n) { int k = (s >> 5) % n; vgl_free(mine[k]); mine[k] = mine[--n]; }
    }
    while (n) { vgl_free(mine[--n]); }
    return NULL;
}
int main(void) {
    // 1. random operations, every answer against the walk
    make_pools(0);
    for (int t = 0; t < 3; t++) EXPECT(mf_ok[t], "pool %d refused by the probes (overhead %u)", t, mf_overhead[t]);
    EXPECT(mf_overhead[0] == 4, "measured overhead %u, the model has 4", mf_overhead[0]);
    vglSetMemFreeMode(1);
    same("empty");
    for (int i = 0; i < 300000; i++) one_op(rnd(), 1);
    printf("1. %llu operations, %ld wrong answers; live blocks %u, %u, %u; counters answered %u, walks made %u\n", (unsigned long long)steps, bad, mf_live[0], mf_live[1], mf_live[2], mf_n_fast, mf_n_walk);
    while (nlive) { vgl_free(live[--nlive].p); }
    same("drained");
    for (int t = 0; t < 3; t++) EXPECT(mf_live[t] == 0 && mf_usable[t] == 0, "pool %d: %u blocks, %u bytes left after freeing everything", t, mf_live[t], mf_usable[t]);
    // mode 2 on the same traffic: the checking path itself
    vglSetMemFreeMode(2); const uint32_t c0 = mf_n_checks;
    for (int i = 0; i < 60000; i++) { one_op(rnd(), 0); if (i % 3 == 0) { for (int t = 0; t < 3; t++) (void)vgl_mem_get_free_space((vglMemType)t); } }
    EXPECT(mf_n_bad == 0, "mode 2 found %u differences (worst %d)", mf_n_bad, mf_worst);
    EXPECT(mf_n_checks - c0 > 50000, "mode 2 made only %u checks", mf_n_checks - c0);
    printf("   mode 2: %u checks, %u differences\n", mf_n_checks - c0, mf_n_bad);
    while (nlive) { vgl_free(live[--nlive].p); }
    // 2. three threads at once, mode 2
    vglSetMemFreeMode(2); const uint32_t r0 = mf_n_raced, k0 = mf_n_checks;
    pthread_t th[3]; stop_threads = 0;
    for (long i = 0; i < 3; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < 20000; i++) for (int t = 0; t < 3; t++) (void)vgl_mem_get_free_space((vglMemType)t);
    stop_threads = 1; for (int i = 0; i < 3; i++) pthread_join(th[i], NULL);
    vglSetMemFreeMode(1); same("after the threads");
    EXPECT(mf_n_bad == 0, "threads: %u differences", mf_n_bad);
    printf("2. three threads: %u checks compared, %u skipped as raced, %u differences; equal at rest\n", mf_n_checks - k0, mf_n_raced - r0, mf_n_bad);
    // 3. allocators the probes must refuse
    for (int style = 1; style <= 2; style++) {
        make_pools(style); vglSetMemFreeMode(1);
        for (int t = 0; t < 3; t++) EXPECT(!mf_ok[t], "style %d: pool %d was accepted", style, t);
        const uint32_t f0 = mf_n_fast;
        void *a = vgl_malloc(5000, VGL_MEM_VRAM), *b = vgl_memalign(256, 777, VGL_MEM_RAM);
        for (int t = 0; t < 3; t++) { const size_t fast = vgl_mem_get_free_space((vglMemType)t); EXPECT(fast == vgl_mf_walk((vglMemType)t), "style %d: pool %d answer %zu", style, t, fast); }
        EXPECT(mf_n_fast == f0, "style %d: counters answered", style);
        vgl_free(a); vgl_free(b);
    }
    printf("3. a statistics call counting requested bytes, and one with no statistics: probes refuse, the walk answers\n");
    // 4. cost: block visits per answer
    make_pools(0); vglSetMemFreeMode(1);
    for (int n = 100; n <= 6400; n *= 4) {
        while (mf_live[0] < (uint32_t)n) (void)vgl_malloc(64 + rnd() % 2000, VGL_MEM_VRAM);
        g_walk_calls = g_walk_visits = 0;
        for (int i = 0; i < 100; i++) (void)vgl_mem_get_free_space(VGL_MEM_VRAM);
        const uint64_t viaCounters = g_walk_visits;
        vglSetMemFreeMode(0);
        for (int i = 0; i < 100; i++) (void)vgl_mem_get_free_space(VGL_MEM_VRAM);
        vglSetMemFreeMode(1);
        printf("4. %5u live blocks: 100 answers visit %llu blocks with the counters, %llu with the walk\n", mf_live[0], (unsigned long long)viaCounters, (unsigned long long)g_walk_visits);
        EXPECT(viaCounters == 0, "the counter answers walked the heap");
    }
    if (bad) { printf("FAIL: %ld wrong\n", bad); return 1; }
    puts("vitaGL MEMFREE: PASS");
    return 0;
}
C
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter ${SAN:-} -I"$BUILD" "$BUILD/test.c" -o "$BUILD/mf_test" -lpthread
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} "$BUILD/mf_test"
