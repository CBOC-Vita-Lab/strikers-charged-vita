// dtl.cpp: live differential tester, host32 (see docs/wave3-NPG2.md).
// A: the native function, B: the recompiler's translation of the retail function, both run in forked children of the live game on
// identical state; the pages each one dirties are compared (floats with a tolerance).
#define _GNU_SOURCE 1
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include "abi_bridge.h"

struct DtlFn { uint32_t addr; const char* name; const char* nat; const char* kinds; int hasThis; char ret; int root; const char* ptypes; int pairedret; };
struct DtlData { uint32_t addr; int size; const char* nat; int isVt; };
struct DtlSym { uint32_t addr; uint32_t size; const char* name; };
struct DtlVt { const char* nat; uint32_t size; };
struct DtlField { uint32_t off; const char* name; };
struct DtlLocal { uint32_t addr; int size; const char* name; };
extern "C" void NpMemSetGameThread(int on) __attribute__((weak));
extern "C" int NpMemGetGameThread() __attribute__((weak));

#include "dtl_gen.inc"
#include "dtl_tables.inc"
#include "dtl_bf.inc"

// ------------------------------------------------------------------------------------------------------------------------
namespace dtl {
uintptr_t g_vt_lo = ~(uintptr_t)0, g_vt_hi = 0, g_shadow_base = 0, g_gstack_base = 0, g_gstack_size = 0;
struct Seg { uint32_t lo, hi; uint8_t* nat; int isVt; const char* name; };
struct ObjSym { uintptr_t addr; uint32_t size; std::string name; };
struct VtMapT { uintptr_t nat; uint32_t nbytes; int nslots; uint8_t map[128]; };       // retail virtual slot -> native slot of one class
struct GState {       // allocated on first use: no static constructors (the selftest runs from a constructor)
    std::vector<std::pair<uintptr_t, uintptr_t>> vtv;
    std::vector<Seg> segs;
    std::unordered_map<uint32_t, const DtlFn*> byaddr;
    std::unordered_map<uintptr_t, const DtlFn*> bynat;
    std::unordered_map<uint32_t, void (*)(CpuContext*)> tr;
    std::unordered_map<uint32_t, void*> natfn;
    std::vector<ObjSym> objsyms;
    std::vector<VtMapT> vtmaps;
    uint32_t warned[400]; int nwarned = 0;     // fixed storage: the children must not allocate from the game heap
};
static GState& G() { static GState* s = new GState; return *s; }
#define g_vtv (G().vtv)
#define g_segs (G().segs)
#define g_byaddr (G().byaddr)
#define g_bynat (G().bynat)
#define g_tr (G().tr)
#define g_natfn (G().natfn)
#define g_warned (G().warned)
#define g_objsyms (G().objsyms)
#define g_vtmaps (G().vtmaps)
static uint8_t* g_shadow = nullptr;
static uint8_t* g_gstack = nullptr; static size_t g_gstack_len = 256 * 1024;
static int g_mode = 1;                   // 1: whole retail closure (translated where available), 2/3/4: only g_tested is translated
static bool g_enabled = false;
static bool g_calls_trace = false;
// everything the children write while they run lives in these page-aligned blocks, which the page comparison excludes
struct alignas(4096) Priv { bool in_child; int which; uint32_t tested; CpuContext ctx; uint8_t pad[4096]; };
static Priv g_priv;
#define g_in_child (g_priv.in_child)
#define g_which (g_priv.which)
#define g_tested (g_priv.tested)

struct Shared {
    volatile uint32_t trace_n;
    volatile uint32_t trace[512];
    volatile uint32_t regs[8];
    volatile int xwarn;
    volatile uint32_t xl_n; volatile uint32_t xl[16][2];   // last static accesses of the retail child (retail address, native address)
    volatile uint32_t status[2];     // 0 running, 1 done, 2 died with signal, 3 died with dtl error
    volatile int sig[2];
    volatile uint64_t fault_addr[2], fault_pc[2];
    volatile uint32_t ret_eax[2], ret_edx[2];
    volatile double ret_f[2];
    char msg[2][256];
};
static Shared* g_sh = nullptr;

bool in_vtable(const uint8_t* p) {
    uintptr_t a = (uintptr_t)p;
    size_t lo = 0, hi = g_vtv.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_vtv[m].second <= a) lo = m + 1; else hi = m; }
    return lo < g_vtv.size() && g_vtv[lo].first <= a;
}

// p = vptr + retail displacement lies inside a native vtable (address point +8): the native slot that holds the function the retail slot names. The Itanium vtable has two destructor entries (complete,
// deleting) where MWCC has one, so every slot after the destructor is shifted: the map (built in init() by method names) corrects that.
const uint8_t* vt_resolve(const uint8_t* p) {
    p -= 8;
    size_t lo = 0, hi = g_vtmaps.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_vtmaps[m].nat + g_vtmaps[m].nbytes <= (uintptr_t)p) lo = m + 1; else hi = m; }
    if (lo < g_vtmaps.size() && g_vtmaps[lo].nat <= (uintptr_t)p) {
        const VtMapT& v = g_vtmaps[lo]; uintptr_t o = (uintptr_t)p - v.nat;
        if (o >= 8 && ((o - 8) & 3) == 0 && (int)((o - 8) / 4) < v.nslots) return (const uint8_t*)(v.nat + 8 + 4 * v.map[(o - 8) / 4]);
    }
    return p;
}

static const char* retail_name(uint32_t a, uint32_t* off) {
    // g_rsyms is sorted by address
    size_t n = sizeof(g_rsyms) / sizeof(g_rsyms[0]) - 1, lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_rsyms[m].addr <= a) lo = m + 1; else hi = m; }
    if (lo == 0) return nullptr;
    const DtlSym& s = g_rsyms[lo - 1];
    if (a >= s.addr + (s.size ? s.size : 1) && a - s.addr > 0x10000) return nullptr;
    *off = a - s.addr; return s.name;
}

static const Seg* find_seg(uint32_t a) {
    size_t lo = 0, hi = g_segs.size();
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_segs[m].lo <= a) lo = m + 1; else hi = m; }
    if (lo == 0) return nullptr;
    const Seg& s = g_segs[lo - 1];
    return a < s.hi ? &s : nullptr;
}

static bool mutable_static(uint32_t a) { return (a >= 0x804F4120u && a < 0x806E3060u) || (a >= 0x806E7BA0u && a < 0x806E7C00u); }

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    ssize_t r = write(2, buf, strlen(buf)); (void)r;
}

[[noreturn]] static void die(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] static void die(const char* fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_sh) { int w = g_in_child ? g_which : 0; strncpy(g_sh->msg[w], buf, 255); g_sh->status[w] = 3; }
    say("[dtl] FATAL %s\n", buf);
    _exit(97);
}

uint8_t* xlate_static_impl(uint32_t a, uint32_t n, bool write);
static inline uint8_t* xl_note(uint32_t a, uint8_t* nat) { if (g_sh && g_in_child) { uint32_t k = g_sh->xl_n++ & 15; g_sh->xl[k][0] = a; g_sh->xl[k][1] = (uint32_t)(uintptr_t)nat; } return nat; }
uint8_t* xlate_static(uint32_t a, uint32_t n, bool write) { return xl_note(a, xlate_static_impl(a, n, write)); }
uint8_t* xlate_static_impl(uint32_t a, uint32_t n, bool write) {
    const Seg* s = find_seg(a);
    if (s) {
        if (a + n > s->hi) {
            if (g_sh && g_sh->xwarn++ < 20) say("[dtl] access crosses native static %s (%08x+%u > %08x)\n", s->name, a, n, s->hi);
        }
        return s->nat + (a - s->lo);
    }
    if (mutable_static(a)) {
        uint32_t off = 0; const char* nm = retail_name(a, &off);
        uint32_t key = nm ? (uint32_t)(uintptr_t)nm : a;
        if (std::find(g_warned, g_warned + G().nwarned, key) == g_warned + G().nwarned && G().nwarned < 400) {
            g_warned[G().nwarned++] = key;
            say("[dtl] UNMAPPED mutable static %s %s at %08x (%s+0x%x)\n", write ? "write" : "read", nm ? nm : "?", a, nm ? nm : "?", off);
        }
    }
    return g_shadow + (a - kStatLo);
}

static uint32_t map_ptr_native(uint32_t v) {
    if (v - kStatLo >= kStatLen) return v;
    const Seg* s = find_seg(v);
    if (s) return (uint32_t)(uintptr_t)(s->nat + (s->isVt ? 8 : 0) + (v - s->lo));
    auto it = g_byaddr.find(v);
    if (it != g_byaddr.end()) { auto f = g_natfn.find(v); if (f != g_natfn.end() && f->second) return (uint32_t)(uintptr_t)f->second; }
    return (uint32_t)(uintptr_t)(g_shadow + (v - kStatLo));
}

// ---- bit-field view: the translation sees the objects of classes with bit-fields in the MWCC (MSB first) layout, native memory keeps the clang layout (see gen_bf.py) ----------------------------------
static bool g_bf_off = false; bool* g_bf_onp = &g_bf_off;     // the flag the accessors read; points into g_bfs (excluded from the page comparison) once init() ran
struct BfInst { uint32_t addr; const BfUnit* u; };
struct VpEntry { uint32_t vptr; const BfClass* c; };
static const uint32_t kBfH = 1u << 20, kBfCap = 1u << 20;
struct SynthFn { DtlFn f; char kinds[16]; char name[128]; };
struct BfState { SynthFn synth[128]; int nsynth; bool on; uint32_t n, a1, vp_lo, vp_hi; int nvp, noffs; uint16_t offs[32]; VpEntry vp[64]; BfInst inst[kBfCap]; uint32_t hash[kBfH]; };
static BfState* g_bfs = nullptr;       // mmap'd in init(): the registry of the retail child must not show up as dirty pages
#define g_bfi (g_bfs->inst)
#define g_bfn (g_bfs->n)
#define g_bfh (g_bfs->hash)
#define g_vp (g_bfs->vp)
#define g_nvp (g_bfs->nvp)
#define g_vp_lo (g_bfs->vp_lo)
#define g_vp_hi (g_bfs->vp_hi)
#define g_bf_a1 (g_bfs->a1)
#define g_bfs_noffs (g_bfs->noffs)
#define g_bfs_offs (g_bfs->offs)
static inline const BfUnit* bf_find(uint32_t a) {
    uint32_t h = (a * 2654435761u) >> 12;
    for (;;) { uint32_t e = g_bfh[h]; if (!e) return nullptr; if (g_bfi[e - 1].addr == a) return g_bfi[e - 1].u; h = (h + 1) & (kBfH - 1); }
}
static void bf_register(uint32_t a, const BfUnit* u) {
    if (bf_find(a) || g_bfn >= kBfCap) return;
    g_bfi[g_bfn] = { a, u }; uint32_t h = (a * 2654435761u) >> 12; while (g_bfh[h]) h = (h + 1) & (kBfH - 1); g_bfh[h] = ++g_bfn;
}
static inline uint32_t fmask(uint32_t n) { return n >= 32 ? ~0u : ((1u << n) - 1); }
// The unit's bit permutation: a field bit at native position p sits at retail position W - lo - n + (p - lo); the retail positions that are not field positions of the native layout
// take the remaining native field positions (a bijection, so padding bits survive a view/unview round trip unchanged).
static void bf_perm(const BfUnit* u, uint8_t* pi) {
    uint32_t W = u->W; int sigma[32]; for (uint32_t i = 0; i < W; i++) { pi[i] = (uint8_t)i; sigma[i] = -1; }
    uint32_t F = 0, S = 0;
    for (int i = 0; i < u->nf; i++) { uint32_t lo = u->f[i].lo, n = u->f[i].n; for (uint32_t j = 0; j < n; j++) { sigma[lo + j] = (int)(W - lo - n + j); F |= 1u << (lo + j); S |= 1u << (W - lo - n + j); } }
    uint32_t a = S & ~F, f = F & ~S;                         // retail field positions that are not native field positions / native field positions that are not retail field positions
    for (uint32_t p = 0; p < W; p++) if (sigma[p] >= 0) pi[p] = (uint8_t)sigma[p];
    uint32_t qa = 0; for (uint32_t q = 0; q < W; q++) if (a & (1u << q)) { while (qa < W && !(f & (1u << qa))) qa++; if (qa < W) { pi[q] = (uint8_t)qa; qa++; } }
}
static uint32_t bf_n2r(const BfUnit* u, uint32_t c) { uint8_t pi[32]; bf_perm(u, pi); uint32_t r = 0; for (uint32_t p = 0; p < u->W; p++) if (c & (1u << p)) r |= 1u << pi[p]; return r; }
static uint32_t bf_r2n(const BfUnit* u, uint32_t r) { uint8_t pi[32]; bf_perm(u, pi); uint32_t c = 0; for (uint32_t p = 0; p < u->W; p++) if (r & (1u << pi[p])) c |= 1u << p; return c; }
static void bf_object(uint32_t a, const BfClass* c);
static inline bool bf_in_arena(uint32_t a) { return a - g_bf_a1 < 0x1800000u || a - 0x90000000u < 0x4000000u; }
// objects that appeared after the registry was built (a native callee constructed them): a word at (address - unit offset) holding the vptr of a tabulated class
static const BfUnit* bf_probe(uint32_t a) {
    for (int i = 0; i < g_bfs_noffs; i++) {
        uint32_t b = a - g_bfs_offs[i]; if (!bf_in_arena(b)) continue;
        uint32_t w = *(const uint32_t*)(uintptr_t)b; if (w - g_vp_lo > g_vp_hi - g_vp_lo) continue;
        for (int k = 0; k < g_nvp; k++) if (g_vp[k].vptr == w) { const BfClass* c = g_vp[k].c; bool has = false; for (int j = 0; j < c->nunits; j++) if (c->u[j].off == g_bfs_offs[i]) has = true; if (has) { bf_object(b, c); return bf_find(a); } }
    }
    return nullptr;
}
static inline uint32_t bf_apply(uintptr_t p, uint32_t n, uint32_t v, bool toNative) {
    uint32_t a = (uint32_t)p;
    if (!g_bfs->on || !bf_in_arena(a)) return v;
    for (uint32_t k = 0; k < n; k++) {
        const BfUnit* u = bf_find(a + k); if (!u) u = bf_probe(a + k); if (!u) continue;
        uint32_t nb = u->W / 8; if (k + nb > n) continue;
        uint32_t mask = nb == 4 ? ~0u : ((1u << (8 * nb)) - 1), c = (v >> (8 * k)) & mask;
        uint32_t r = toNative ? bf_r2n(u, c) : bf_n2r(u, c);
        v = (v & ~(mask << (8 * k))) | ((r & mask) << (8 * k));
    }
    return v;
}
uint32_t bf_view(uintptr_t p, uint32_t n, uint32_t v) { return bf_apply(p, n, v, false); }
uint32_t bf_unview(uintptr_t p, uint32_t n, uint32_t v) { return bf_apply(p, n, v, true); }
static void bf_object(uint32_t a, const BfClass* c) { for (int i = 0; i < c->nunits; i++) bf_register(a + c->u[i].off, &c->u[i]); }
void bf_note_store(uintptr_t p, uint32_t v) {
    if (v - g_vp_lo > g_vp_hi - g_vp_lo) return;
    for (int i = 0; i < g_nvp; i++) if (g_vp[i].vptr == v) { bf_object((uint32_t)p, g_vp[i].c); return; }
}
static void bf_build(const std::vector<uintptr_t>& pages) {
    memset(g_bfh, 0, sizeof(g_bfs->hash)); g_bfn = 0; g_bfs->on = false; g_bf_a1 = 0x80000000u;
    if (const char* e = getenv("NP_MEM1_BASE")) g_bf_a1 = (uint32_t)strtoul(e, nullptr, 16);
    g_nvp = 0; g_vp_lo = ~0u; g_vp_hi = 0;
    for (const BfClass* c = g_bfclasses; c->name && g_nvp < 64; ++c) {
        void* sym = dlsym(RTLD_DEFAULT, c->vtsym); if (!sym) continue;
        uint32_t vp = (uint32_t)(uintptr_t)sym + 8; g_vp[g_nvp++] = { vp, c }; g_vp_lo = std::min(g_vp_lo, vp); g_vp_hi = std::max(g_vp_hi, vp);
    }
    g_bfs_noffs = 0;
    for (const BfClass* c = g_bfclasses; c->name; ++c) for (int j = 0; j < c->nunits; j++) { bool have = false; for (int i = 0; i < g_bfs_noffs; i++) if (g_bfs_offs[i] == c->u[j].off) have = true; if (!have && g_bfs_noffs < 32) g_bfs_offs[g_bfs_noffs++] = c->u[j].off; }
    if (!g_nvp) return;
    for (uintptr_t pg : pages) {
        const uint32_t* w = (const uint32_t*)pg;
        for (int i = 0; i < 1024; i++) {
            uint32_t v = w[i]; if (v - g_vp_lo > g_vp_hi - g_vp_lo) continue;
            for (int k = 0; k < g_nvp; k++) if (g_vp[k].vptr == v) { bf_object((uint32_t)(pg + 4 * i), g_vp[k].c); break; }
        }
    }
    g_bfs->on = true;
    if (getenv("NP_DTL_BFDEBUG")) { say("[dtl] bf registry: %d classes with vtables, %u unit instances\n", g_nvp, g_bfn); for (int k = 0; k < g_nvp; k++) say("[dtl]   %s vptr %08x\n", g_vp[k].c->name, g_vp[k].vptr); }
}
uint32_t conv_store(uint32_t v, const uint8_t* dst) {
    if (v - kStatLo >= kStatLen) return v;
    uintptr_t d = (uintptr_t)dst;
    if (d - (uintptr_t)g_gstack < g_gstack_len) return v;                      // retail private storage (stack frames)
    if (d - (uintptr_t)g_shadow < kStatLen) return v;                          // shadow image
    return map_ptr_native(v);
}

void psq_unsupported(uint32_t gqr) { die("quantized paired-single load with GQR %08x", gqr); }

// ------------------------------------------------------------------------------------------------------------------------
// calling native code with a computed argument list (i386 cdecl)
extern "C" void dtl_callN(void* fn, const uint32_t* words, uint32_t* out, double* fout, uint32_t* hasf);
asm(R"(
.text
.globl dtl_callN
.type dtl_callN,@function
dtl_callN:
    pushl %ebp
    movl  %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl  12(%ebp), %esi
    movl  8(%ebp), %eax
    subl  $64, %esp
    andl  $-16, %esp
    movl  %esp, %edi
    movl  $16, %ecx
    cld
    rep movsl
    call  *%eax
    movl  16(%ebp), %ecx
    movl  %eax, 0(%ecx)
    movl  %edx, 4(%ecx)
    fxam
    fnstsw %ax
    andl  $0x4500, %eax
    cmpl  $0x4100, %eax
    je    1f
    movl  20(%ebp), %ecx
    fstpl 0(%ecx)
    movl  24(%ebp), %ecx
    movl  $1, 0(%ecx)
    jmp   2f
1:  movl  24(%ebp), %ecx
    movl  $0, 0(%ecx)
2:  leal  -12(%ebp), %esp
    popl  %edi
    popl  %esi
    popl  %ebx
    popl  %ebp
    ret
.size dtl_callN,.-dtl_callN
)");

static void call_native(const DtlFn* f, void* fn, CpuContext* c) {
    uint32_t w[16]; int nw = 0; int gi = 3, fi = 1; int sk = 0;
    auto geti = [&]() -> uint32_t { if (gi <= 10) return c->gpr[gi++]; uint32_t v = MemoryInline::FlatRead32(c->gpr[1] + 8 + 4 * (sk++)); return v; };
    if (f->hasThis) w[nw++] = map_ptr_native(geti());
    for (const char* k = f->kinds; *k; ++k) {
        if (nw > 13) die("too many words for native call %s", f->name);
        switch (*k) {
        case 'p': w[nw++] = map_ptr_native(geti()); break;
        case 'i': w[nw++] = geti(); break;
        case 'f': { float x = (float)c->fpr[fi++].d; memcpy(&w[nw++], &x, 4); break; }
        case 'd': { double x = c->fpr[fi++].d; memcpy(&w[nw], &x, 8); nw += 2; break; }
        case 'L': { if ((gi & 1) == 0) gi++; uint32_t hi = c->gpr[gi], lo = c->gpr[gi + 1]; gi += 2; w[nw++] = lo; w[nw++] = hi; break; }
        case '.': { while (gi <= 10 && nw < 12) w[nw++] = c->gpr[gi++]; break; }
        default: die("unsupported parameter kind '%c' calling %s", *k, f->name);
        }
    }
    uint32_t out[2] = { 0, 0 }; double fr = 0; uint32_t hasf = 0;
    uint32_t this0 = c->gpr[3];
    dtl_callN(fn, w, out, &fr, &hasf);
    if (g_in_child && g_which == 1) { const bool tr = g_calls_trace; if (tr && strstr(f->name, "GetPressureDerivative")) { uint32_t vp = *(uint32_t*)(uintptr_t)w[0]; Dl_info d0; const char* vn = (dladdr((void*)(uintptr_t)(vp - 8), &d0) && d0.dli_sname) ? d0.dli_sname : "?"; char b[1200]; int l = snprintf(b, sizeof b, "[dtl-vt] object %x vptr %x (%s):", w[0], vp, vn); for (int k = 0; k < 10; k++) { uint32_t fa = *(uint32_t*)(uintptr_t)(vp + 4 * k); Dl_info d1; const char* nn = (dladdr((void*)(uintptr_t)fa, &d1) && d1.dli_sname) ? d1.dli_sname : "?"; l += snprintf(b + l, sizeof b - l, " [%d]%s", k, nn); } b[l++] = '\n'; ssize_t wr = write(2, b, l); (void)wr; } if (tr) { char b[400]; int l = snprintf(b, sizeof b, "[dtl-call] %s args:", f->name); for (int i = 0; i < nw && i < 8; i++) l += snprintf(b + l, sizeof b - l, " %x", w[i]); l += snprintf(b + l, sizeof b - l, " -> eax %x%s\n", out[0], hasf ? " (+x87)" : ""); ssize_t wr = write(2, b, l); (void)wr; } }
    if (f->hasThis && (strncmp(f->name, "__ct", 4) == 0 || strncmp(f->name, "__dt", 4) == 0 || strncmp(f->name, "__as", 4) == 0)) {   // retail constructors, destructors and operator= return this in r3; the Itanium ones return void
        if (*g_bf_onp && strncmp(f->name, "__ct", 4) == 0) { uint32_t vp = *(uint32_t*)(uintptr_t)w[0]; bf_note_store((uintptr_t)w[0], vp); }       // a native constructor made an object of a class with bit-fields
        c->gpr[3] = this0; if (hasf) c->fpr[1].d = (f->ret == 'f') ? (double)(float)fr : fr; return;
    }
    switch (f->ret) {
    case 'v': break;
    case 'b': case 'u': c->gpr[3] = out[0] & 0xFF; break;
    case 'c': c->gpr[3] = (uint32_t)(int32_t)(int8_t)out[0]; break;
    case 's': c->gpr[3] = (uint32_t)(int32_t)(int16_t)out[0]; break;
    case 'w': c->gpr[3] = out[0] & 0xFFFF; break;
    case 'L': c->gpr[3] = out[1]; c->gpr[4] = out[0]; break;
    case 'f': case 'd': break;
    default: c->gpr[3] = out[0]; break;
    }
    if (hasf) { c->fpr[1].d = (f->ret == 'f') ? (double)(float)fr : fr; if (f->pairedret) c->fpr[1].d = PpcPackPairedInline((float)fr, (float)fr); }   // nlQuatDot: the translated callers convert with PPC_PsToScalarInline
}

static const char* g_native_ex = nullptr;     // NP_DTL_NATIVE, read once in init() (a lazily initialised static would dirty a page in the retail child only)
static inline void trace(uint32_t a) { if (g_sh && g_in_child) { uint32_t n = g_sh->trace_n; g_sh->trace[n & 511] = a; g_sh->trace_n = n + 1; } }
static bool use_translation(const DtlFn* f, bool* have_tr) {
    auto it = g_tr.find(f->addr);
    bool tr = it != g_tr.end(); *have_tr = tr;
    void* nat = g_natfn.count(f->addr) ? g_natfn[f->addr] : nullptr;
    if (!tr) return false;
    if (!nat) return true;
    {   // NP_DTL_NATIVE=<substr,substr,...>: functions whose (MWCC) name contains one of these stay native (bit-field heavy byte-exact TUs: cPoseAccumulator,cPN_,cSAnim ...)
        const char* ex = g_native_ex;
        if (ex && *ex) { const char* p = ex; while (*p) { const char* e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p); if (l && l < 64) { char k[64]; memcpy(k, p, l); k[l] = 0; if (strstr(f->name, k)) return false; } if (!e) break; p = e + 1; } }
    }
    if (g_mode == 1) return f->root == 1;       // the whole retail closure: the goalie (and PeachPhoto / PoseAccumulator) TUs; the numeric sweep roots (root 2) stay native
    return f->addr == g_tested;
}
} // namespace dtl

using namespace dtl;

// Indirect calls (virtual functions, callbacks) that land in native code without a retail counterpart in the tables: the signature comes from the demangled native symbol (parameters as ABI kinds, `this` for
// members; the return value is taken from eax / x87 whatever the type). Cached in the excluded state page (nothing here may dirty the compared memory).
static const DtlFn* synth_native(uintptr_t target) {
    for (int i = 0; i < g_bfs->nsynth; i++) if ((uintptr_t)g_bfs->synth[i].f.nat == target) return &g_bfs->synth[i].f;
    Dl_info di; if (!dladdr((void*)target, &di) || !di.dli_sname || g_bfs->nsynth >= 128) return nullptr;
    int st = 0; char* dem = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st); if (!dem || st) { free(dem); return nullptr; }
    SynthFn& sf = g_bfs->synth[g_bfs->nsynth]; memset(&sf, 0, sizeof sf);       // no std::string / new here: this runs in the retail child with the game heaps on
    const char* open = strchr(dem, '('); if (!open) { free(dem); return nullptr; }
    int depth = 0, nk = 0; const char* p = open + 1; const char* tok = p; bool bad = false;
    auto classify = [&](const char* b, const char* e) {
        while (b < e && *b == ' ') b++; while (e > b && e[-1] == ' ') e--;
        size_t n = e - b; if (n == 0 || (n == 4 && !strncmp(b, "void", 4))) return;
        char k = 'i';
        bool ptr = e[-1] == '*' || e[-1] == '&'; for (const char* q = b; q + 2 < e + 1 && q < e; q++) if ((q[0] == '(' && q + 1 < e && q[1] == '*') || q[0] == '[') ptr = true;
        if (ptr) k = 'p';
        else if (n == 5 && !strncmp(b, "float", 5)) k = 'f'; else if (n == 6 && !strncmp(b, "double", 6)) k = 'd';
        else if ((n == 9 && !strncmp(b, "long long", 9)) || (n == 18 && !strncmp(b, "unsigned long long", 18))) k = 'L';
        else if (n == 3 && !strncmp(b, "...", 3)) k = '.';
        if (nk < 14) sf.kinds[nk++] = k; else bad = true;
    };
    for (; *p; p++) {
        if (*p == '(' || *p == '<') depth++;
        else if (*p == '>') depth--;
        else if (*p == ')') { if (depth == 0) { classify(tok, p); break; } depth--; }
        else if (*p == ',' && depth == 0) { classify(tok, p); tok = p + 1; }
    }
    size_t hl = open - dem; bool member = false; for (size_t i = 0; i + 1 < hl; i++) if (dem[i] == ':' && dem[i + 1] == ':') member = true;
    snprintf(sf.name, sizeof sf.name, "%s%.*s", (uintptr_t)di.dli_saddr == target ? "" : "~", (int)std::min<size_t>(hl, 100), dem);   // '~': dladdr found an earlier symbol, the signature is a guess
    free(dem);
    if (bad) return nullptr;
    sf.f = { 0, sf.name, (const char*)target, sf.kinds, member ? 1 : 0, '?', 0, nullptr, 0 };
    g_bfs->nsynth++;
    return &sf.f;
}
void dtl_dispatch(uint32_t target, CpuContext* ctx) {
    const DtlFn* f = nullptr;
    if (target - 0x80004000u < 0x004D74CCu) {
        auto it = g_byaddr.find(target);
        if (it == g_byaddr.end()) die("unknown retail call target %08x", target);
        f = it->second;
    } else {
        auto it = g_bynat.find((uintptr_t)target);
        if (it == g_bynat.end()) {
            if (const DtlFn* sf = synth_native((uintptr_t)target)) { trace(0u | 1u); call_native(sf, (void*)(uintptr_t)target, ctx); return; }
            Dl_info di; const char* nm = dladdr((void*)(uintptr_t)target, &di) && di.dli_sname ? di.dli_sname : "?";
            die("indirect call to a native function without a retail counterpart (signature not derivable): %08x %s", target, nm);
        }
        f = it->second;
    }
    bool have_tr;
    if (use_translation(f, &have_tr)) { trace(f->addr); g_tr[f->addr](ctx); return; }
    trace(f->addr | 1u);
    auto nf = g_natfn.find(f->addr);
    if (nf == g_natfn.end() || !nf->second) die("no native and no translation for %08x %s", f->addr, f->name);
    if (f->kinds[0] == '?') die("signature unknown for native call %08x %s", f->addr, f->name);
    call_native(f, nf->second, ctx);
}

// ------------------------------------------------------------------------------------------------------------------------
namespace dtl {
static void load_dol() {
    const char* path = getenv("NP_DTL_DOL"); if (!path) path = "/home/claude/msc/game/sys/main.dol";
    FILE* fp = fopen(path, "rb"); if (!fp) { say("[dtl] cannot open %s\n", path); abort(); }
    uint32_t hdr[0x100 / 4]; if (fread(hdr, 4, 0x100 / 4, fp) != 0x100 / 4) abort();
    for (auto& h : hdr) h = __builtin_bswap32(h);
    g_shadow = (uint8_t*)mmap(nullptr, kStatLen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_shadow_base = (uintptr_t)g_shadow;
    for (int i = 0; i < 18; i++) {
        uint32_t off = hdr[i], addr = hdr[0x48 / 4 + i], size = hdr[0x90 / 4 + i];
        if (!size || addr - kStatLo >= kStatLen) continue;
        uint32_t n = std::min(size, kStatLen - (addr - kStatLo));
        fseek(fp, off, SEEK_SET); if (fread(g_shadow + (addr - kStatLo), 1, n, fp) != n) abort();
    }
    fclose(fp);
}

// file-static data of the native build: the symbol table of the running executable (position independent: bias from an exported reference symbol)
static void map_file_statics() {
    FILE* fp = fopen("/proc/self/exe", "rb"); if (!fp) return;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> img(n); if (fread(img.data(), 1, n, fp) != (size_t)n) { fclose(fp); return; } fclose(fp);
    struct E32H { uint8_t id[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
    struct E32S { uint32_t name, type, flags, addr, off, size, link, info, align, entsize; };
    struct E32Y { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; };
    const E32H* h = (const E32H*)img.data(); const E32S* sh = (const E32S*)(img.data() + h->shoff);
    const E32S* symtab = nullptr; for (int i = 0; i < h->shnum; i++) if (sh[i].type == 2) symtab = &sh[i];
    if (!symtab) return;
    const char* str = (const char*)(img.data() + sh[symtab->link].off); const E32Y* sy = (const E32Y*)(img.data() + symtab->off); size_t ns = symtab->size / sizeof(E32Y);
    std::unordered_multimap<std::string, std::pair<uint32_t, uint32_t>> locals; uint32_t refval = 0;
    for (size_t i = 0; i < ns; i++) {
        const char* nm = str + sy[i].name; if (!*nm) continue;
        if (strcmp(nm, g_localref) == 0) refval = sy[i].value;
        if ((sy[i].info >> 4) != 0 || (sy[i].info & 15) != 1) continue;            // STB_LOCAL, STT_OBJECT
        std::string name = nm; int len;
        if (name.compare(0, 3, "_ZL") == 0 && sscanf(name.c_str() + 3, "%d", &len) == 1) { size_t p = 3; while (isdigit((unsigned char)name[p])) p++; name = name.substr(p); }
        else if (name.compare(0, 3, "_ZZ") == 0) {                                      // function-local static: the last <len><identifier> after an 'E'
            std::string id; for (size_t q = 3; q + 1 < name.size(); q++) {
                if (name[q] != 'E' || !isdigit((unsigned char)name[q + 1])) continue;
                size_t j = q + 1; int l = 0; while (j < name.size() && isdigit((unsigned char)name[j])) l = l * 10 + (name[j++] - '0');
                if (l <= 0 || j + l > name.size()) continue;
                size_t e = j + l; bool tail = (e == name.size());
                if (!tail && name[e] == '_') { size_t k = e + 1; while (k < name.size() && isdigit((unsigned char)name[k])) k++; tail = (k == name.size() && k > e + 1); }
                if (tail) id = name.substr(j, l);
            }
            if (id.empty()) continue; name = id;
        }
        else if (name[0] == '_' && name[1] == 'Z') continue;
        locals.emplace(name, std::make_pair(sy[i].value, sy[i].size));
    }
    void* ref = dlsym(RTLD_DEFAULT, g_localref); if (!ref || !refval) return;
    uintptr_t bias = (uintptr_t)ref - refval; int nloc = 0;
    for (size_t i = 0; i < ns; i++) { const char* nm = str + sy[i].name; if (*nm && ((sy[i].info & 15) == 1 || (sy[i].info & 15) == 2) && sy[i].size) g_objsyms.push_back({ bias + sy[i].value, sy[i].size, nm }); }
    std::sort(g_objsyms.begin(), g_objsyms.end(), [](const ObjSym& a, const ObjSym& b) { return a.addr < b.addr; });
    for (const DtlLocal* l = g_locals; l->addr; ++l) {
        std::string key = l->name; { size_t d = key.find('$'); if (d != std::string::npos) key.resize(d); }        // retail function-local statics: name$<number>
        auto r = locals.equal_range(key); int cnt = 0; std::pair<uint32_t, uint32_t> best{ 0, 0 }; bool sizes_equal = true;
        for (auto it = r.first; it != r.second; ++it) { if (cnt == 0) best = it->second; else if (it->second.second != best.second) sizes_equal = false; cnt++; }
        if (cnt > 1 && key != l->name) {          // the same function-local name in several native functions: take the one holding the retail initial contents
            int found = 0; std::pair<uint32_t, uint32_t> pick{ 0, 0 };
            for (auto it = r.first; it != r.second; ++it) {
                if (it->second.second != (uint32_t)l->size || (l->size & 3)) continue;
                const uint32_t* nv = (const uint32_t*)(bias + it->second.first); const uint32_t* rv = (const uint32_t*)(g_shadow + (l->addr - kStatLo)); bool eq = true;
                for (uint32_t w = 0; w < l->size / 4 && eq; w++) eq = (nv[w] == __builtin_bswap32(rv[w]));
                if (eq) { found++; pick = it->second; }
            }
            if (found < 1) continue;               // not identifiable: stays unmapped (reads the retail image)
            best = pick; sizes_equal = true;
        }
        if (!cnt || !sizes_equal || (best.second && (uint32_t)l->size != best.second)) continue;
        g_segs.push_back({ l->addr, l->addr + (uint32_t)l->size, (uint8_t*)(bias + best.first), 0, l->name }); nloc++;
    }
    say("[dtl] %d file-static data symbols mapped (bias %lx)\n", nloc, (unsigned long)bias);
}

// ---- virtual slot maps ----------------------------------------------------------------------------------------------------
static const char* rfunc_name(uint32_t a) {
    size_t n = sizeof(g_rfuncs) / sizeof(g_rfuncs[0]) - 1, lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_rfuncs[m].addr < a) lo = m + 1; else hi = m; }
    return lo < n && g_rfuncs[lo].addr == a ? g_rfuncs[lo].name : nullptr;
}
static std::string method_retail(const char* nm) {
    if (!nm) return "?";
    if (strncmp(nm, "__dt__", 6) == 0) return "~";
    const char* e = strstr(nm, "__"); if (!e || e == nm) return nm;
    return std::string(nm, e - nm);
}
static std::string method_native(uintptr_t fn) {
    if (!fn) return "?";
    Dl_info di; if (!dladdr((void*)fn, &di) || !di.dli_sname || (uintptr_t)di.dli_saddr != fn) return "?";
    int st = 0; char* dem = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st); if (!dem || st) { free(dem); return "?"; }
    std::string d = dem; free(dem);
    size_t par = d.find('('); if (par == std::string::npos) return "?";
    std::string head = d.substr(0, par);
    size_t c = head.rfind("::"); std::string m = c == std::string::npos ? head : head.substr(c + 2);
    if (!m.empty() && m[0] == '~') return "~";
    return m;
}
static void build_vtmaps() {
    int nmap = 0, nid = 0;
    for (const DtlData* d = g_data; d->nat; ++d) {
        if (!d->isVt || d->size <= 12) continue;
        void* na = dlsym(RTLD_DEFAULT, d->nat); if (!na) continue;
        uint32_t nbytes = 0; for (const DtlVt* v = g_vts; v->nat; ++v) if (strcmp(v->nat, d->nat) == 0) { nbytes = v->size; break; }
        int R = (int)(d->size - 8) / 4, N = nbytes > 8 ? (int)(nbytes - 8) / 4 : 0;
        if (R <= 0 || N <= 0 || R > 120 || N > 120) continue;
        std::vector<std::string> rm(R), nm(N);
        for (int j = 0; j < R; j++) { uint32_t fa; memcpy(&fa, g_shadow + (d->addr - kStatLo) + 8 + 4 * j, 4); fa = __builtin_bswap32(fa); rm[j] = fa ? method_retail(rfunc_name(fa)) : "?"; }
        for (int i = 0; i < N; i++) nm[i] = method_native(*(uintptr_t*)((uintptr_t)na + 8 + 4 * i));
        VtMapT v; memset(&v, 0, sizeof v); v.nat = (uintptr_t)na; v.nbytes = nbytes + 8; v.nslots = R; bool identity = true;
        int i = 0;
        for (int j = 0; j < R; j++) {
            int t = -1;
            if (rm[j] == "?") t = std::min(i, N - 1);
            else { for (int k = i; k < N; k++) if (nm[k] == rm[j]) { t = k; break; } if (t < 0) t = std::min(i, N - 1); }
            v.map[j] = (uint8_t)t; i = t + 1;
            if (rm[j] == "~" && i < N && nm[i] == "~") i++;                 // the deleting destructor entry that follows the complete one
            if (t != j) identity = false;
        }
        if (identity) { nid++; continue; }
        g_vtmaps.push_back(v); nmap++;
    }
    std::sort(g_vtmaps.begin(), g_vtmaps.end(), [](const VtMapT& a, const VtMapT& b) { return a.nat < b.nat; });
    say("[dtl] virtual slot maps: %d classes remapped, %d identical\n", nmap, nid);
}

static void init() {
    g_native_ex = getenv("NP_DTL_NATIVE"); g_calls_trace = getenv("NP_DTL_CALLS") != nullptr;
    g_bfs = (BfState*)mmap(nullptr, sizeof(BfState), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); g_bf_onp = &g_bfs->on;
    load_dol();
    g_gstack = (uint8_t*)mmap(nullptr, g_gstack_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_gstack_base = (uintptr_t)g_gstack; g_gstack_size = g_gstack_len;
    g_sh = (Shared*)mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    for (const DtlVt* v = g_vts; v->nat; ++v) {
        void* a = dlsym(RTLD_DEFAULT, v->nat); if (!a) continue;
        g_vtv.push_back({ (uintptr_t)a, (uintptr_t)a + v->size + 8 });
    }
    std::sort(g_vtv.begin(), g_vtv.end());
    if (!g_vtv.empty()) { g_vt_lo = g_vtv.front().first; g_vt_hi = g_vtv.back().second; }
    build_vtmaps();
    int nseg = 0, nmiss = 0;
    for (const DtlData* d = g_data; d->nat; ++d) {
        void* a = dlsym(RTLD_DEFAULT, d->nat);
        if (!a) { nmiss++; continue; }
        g_segs.push_back({ d->addr, d->addr + (uint32_t)std::max(d->size, 1), (uint8_t*)a, d->isVt, d->nat }); nseg++;
    }
    map_file_statics();
    std::sort(g_segs.begin(), g_segs.end(), [](const Seg& a, const Seg& b) { return a.lo < b.lo; });
    // overlapping segments (zero-sized or aliased): keep the first of each start, clip to the next start
    for (size_t i = 0; i + 1 < g_segs.size(); i++) if (g_segs[i].hi > g_segs[i + 1].lo) g_segs[i].hi = g_segs[i + 1].lo;
    int nf = 0, nn = 0;
    for (const DtlFn* f = g_fns; f->name; ++f) {
        g_byaddr[f->addr] = f; nf++;
        if (f->nat) { void* a = dlsym(RTLD_DEFAULT, f->nat); if (a) { g_natfn[f->addr] = a; g_bynat[(uintptr_t)a] = f; nn++; } }
    }
    for (auto* t = g_translated; t->a; ++t) g_tr[t->a] = t->f;
    const char* m = getenv("NP_DTL_MODE"); g_mode = m ? atoi(m) : 1;
    say("[dtl] init: %d functions (%d native), %zu translated, %d mapped statics (%d without native symbol), %zu vtables, shadow %p\n", nf, nn, g_tr.size(), nseg, nmiss, g_vtv.size(), g_shadow);
}

// ---- running the retail function --------------------------------------------------------------------------------------
static void setup_ctx(CpuContext* c) {
    memset(c, 0, sizeof *c);
    c->gpr[1] = (uint32_t)((uintptr_t)g_gstack + g_gstack_len - 256) & ~15u;
    c->gpr[2] = 0x806EB060u; c->gpr[13] = 0x806E33C0u;
}

struct Rng {
    uint64_t s; explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { for (int i = 0; i < 4; i++) next(); }
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
    float uf() { return (next() & 0xFFFFFF) / 16777216.0f; }
    float rng(float a, float b) { return a + (b - a) * uf(); }
    bool chance(float p) { return uf() < p; }
};
alignas(16) static uint8_t g_scratch[4096];
struct Step { const DtlFn* f; CpuContext ctx; };
struct Test { void* goalie; const DtlFn* f; CpuContext ctx; int fuzz; uint32_t seed; int state; char desc[200]; int nextra; Step extra[14]; };   // f/ctx = the first step

static void fuzz_goalie(const Test* t) {
    if (!t->fuzz || !t->goalie) return;
    Rng r(t->seed ^ 0xABCDEF); char* g = (char*)t->goalie;
    if (t->state >= 0) *(int*)(g + 0x328) = t->state;
    *(int*)(g + 0x334) = (int)(r.next() % 8);                // mnSubstate
    *(float*)(g + 0x350) = r.rng(-0.5f, 3.0f);                // mfWaitTime
    *(float*)(g + 0x3C4) = r.rng(0.0f, 3.0f);                 // mfTargetTime
    *(uint8_t*)(g + 0x368) = r.chance(0.5f);                  // mbDoNavigate
    *(uint8_t*)(g + 0x36D) = r.chance(0.2f);                  // mbIsDown
    // scratch area: random plausible vectors / words (the out parameters of the callee are compared too: it lives in .bss)
    for (int i = 0; i < 16; i++) { float* v = (float*)(g_scratch + 16 * i); v[0] = r.rng(-35, 35); v[1] = r.rng(-20, 20); v[2] = r.rng(0, 2); v[3] = r.rng(-1, 1); }
}

static void run_native_step(const DtlFn* f, const CpuContext& c0) {
    CpuContext c = c0;
    auto it = g_natfn.find(f->addr); if (it == g_natfn.end() || !it->second) die("no native function for %s", f->name);
    call_native(f, it->second, &c);
}
static void run_retail_step(const DtlFn* f, const CpuContext& c0) {
    CpuContext& c = g_priv.ctx; c = c0; c.gpr[1] = (uint32_t)((uintptr_t)g_gstack + g_gstack_len - 256) & ~15u; c.gpr[2] = 0x806EB060u; c.gpr[13] = 0x806E33C0u;
    auto it = g_tr.find(f->addr); if (it == g_tr.end()) die("no translation for %08x", f->addr);
    g_tested = f->addr;
    it->second(&c);
}
static void run_native(const Test* t) {
    fuzz_goalie(t);
    run_native_step(t->f, t->ctx);
    for (int i = 0; i < t->nextra; i++) run_native_step(t->extra[i].f, t->extra[i].ctx);
}
static void run_retail(const Test* t) {
    fuzz_goalie(t);
    run_retail_step(t->f, t->ctx);
    for (int i = 0; i < t->nextra; i++) run_retail_step(t->extra[i].f, t->extra[i].ctx);
}

// ---- page hashing / dirty page reports ------------------------------------------------------------------------------------
struct Region { uintptr_t lo, hi; };
static std::vector<Region> scan_regions(uintptr_t ex0, uintptr_t ex1, uintptr_t st0, uintptr_t st1) {
    std::vector<Region> out;
    FILE* fp = fopen("/proc/self/maps", "r"); if (!fp) return out;
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        unsigned long lo, hi; char perm[8]; char path[400] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]", &lo, &hi, perm, path) < 3) continue;
        if (perm[0] != 'r' || perm[1] != 'w' || perm[3] != 'p') continue;
        const char* p = path; while (*p == ' ') ++p;
        if (strstr(p, "[stack") || strstr(p, "[vvar") || strstr(p, "[vdso") || strstr(p, "[heap]") || strstr(p, ".so")) continue;
        uintptr_t a = lo, b = hi;
        auto cut = [&](uintptr_t x0, uintptr_t x1) {          // remove [x0,x1) from [a,b) (only the simple cases: it is page aligned and whole)
            if (x0 <= a && x1 >= b) { a = b; return; }
        };
        (void)cut;
        out.push_back({ a, b });
    }
    fclose(fp);
    // exclude: guest stack, shared page, thread stack, shadow, scratch
    std::vector<Region> ex = { { (uintptr_t)g_gstack, (uintptr_t)g_gstack + g_gstack_len }, { (uintptr_t)g_sh, (uintptr_t)g_sh + sizeof(Shared) },
                               { (uintptr_t)g_shadow, (uintptr_t)g_shadow + kStatLen }, { st0, st1 }, { ex0, ex1 },
                               { (uintptr_t)&g_priv, (uintptr_t)&g_priv + sizeof g_priv }, { (uintptr_t)g_bfs, (uintptr_t)g_bfs + sizeof(BfState) } };
    for (const Region& r : out) if (st0 >= r.lo && st0 < r.hi) ex.push_back(r);      // the whole mapping of the game thread's stack: glibc puts the thread control block and the TLS variables above the usable stack
    for (auto& e : ex) { e.lo &= ~(uintptr_t)4095; e.hi = (e.hi + 4095) & ~(uintptr_t)4095; }      // whole pages (a partial page would start a compared range mid-page)
    std::vector<Region> res;
    for (Region r : out) {
        std::vector<Region> cur = { r };
        for (auto& e : ex) {
            std::vector<Region> nxt;
            for (auto& c : cur) {
                if (e.hi <= c.lo || e.lo >= c.hi) { nxt.push_back(c); continue; }
                if (e.lo > c.lo) nxt.push_back({ c.lo, e.lo });
                if (e.hi < c.hi) nxt.push_back({ e.hi, c.hi });
            }
            cur.swap(nxt);
        }
        for (auto& c : cur) res.push_back(c);
    }
    return res;
}

static inline uint64_t hash_page(const uint8_t* p) {
    const uint64_t* w = (const uint64_t*)p; uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 512; i++) { h ^= w[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return h;
}

struct PageRec { uint32_t addr; uint8_t data[4096]; };

static void child_fault(int sig, siginfo_t* si, void* uc_) {
    ucontext_t* uc = (ucontext_t*)uc_;
    g_sh->fault_pc[g_which] = (uint64_t)uc->uc_mcontext.gregs[REG_EIP];
    { static const int rr[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI }; for (int i = 0; i < 8; i++) g_sh->regs[i] = (uint32_t)uc->uc_mcontext.gregs[rr[i]]; }
    g_sh->sig[g_which] = sig; g_sh->fault_addr[g_which] = (uint64_t)(uintptr_t)si->si_addr; g_sh->status[g_which] = 2;
    _exit(98);
}
static void child(int which, const Test* t, int fd, bool retail, uintptr_t st0, uintptr_t st1) {
    g_in_child = true; g_which = which; alarm(40);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = child_fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr); sigaction(SIGFPE, &sa, nullptr); sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr); sigaction(SIGALRM, &sa, nullptr);
    size_t cap = 1u << 20;      // pages: bookkeeping lives in its own mapping, excluded from the scan
    size_t ex_len = 64u << 20;
    uint8_t* book = (uint8_t*)mmap(nullptr, ex_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    std::vector<Region> regs = scan_regions((uintptr_t)book, (uintptr_t)book + ex_len, st0, st1);
    uint64_t* h1 = (uint64_t*)book; size_t npg = 0;
    std::vector<uintptr_t> pages;
    for (auto& r : regs) for (uintptr_t a = r.lo; a < r.hi; a += 4096) { pages.push_back(a); }
    // skip pages that are not resident (untouched anonymous memory): ask pagemap
    int pm = open("/proc/self/pagemap", O_RDONLY);
    std::vector<uintptr_t> res;
    if (pm >= 0) {
        for (uintptr_t a : pages) { uint64_t e = 0; if (pread(pm, &e, 8, (off_t)(a >> 12) * 8) == 8 && (e >> 63)) res.push_back(a); }
        close(pm);
    } else res = pages;
    if (res.size() * 8 > ex_len / 2) die("too many pages (%zu)", res.size());
    npg = res.size();
    for (size_t i = 0; i < npg; i++) h1[i] = hash_page((const uint8_t*)res[i]);
    if (retail && !getenv("NP_DTL_NOBF")) bf_build(res);
    if (NpMemSetGameThread) NpMemSetGameThread(1);          // F allocates from the game heaps like in the real game
    if (retail) run_retail(t); else run_native(t);
    if (NpMemSetGameThread) NpMemSetGameThread(0);
    uint32_t eax = 0;
    uint32_t nd = 0;
    for (size_t i = 0; i < npg; i++) if (hash_page((const uint8_t*)res[i]) != h1[i]) nd++;
    // report: header, then the dirty pages
    uint32_t hdr[4] = { 0xD71D71D7u, nd, (uint32_t)npg, eax };
    if (write(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) _exit(96);
    for (size_t i = 0; i < npg; i++) {
        if (hash_page((const uint8_t*)res[i]) == h1[i]) continue;
        uint32_t a = (uint32_t)res[i];
        if (write(fd, &a, 4) != 4) _exit(96);
        const uint8_t* p = (const uint8_t*)res[i]; size_t left = 4096;
        while (left) { ssize_t k = write(fd, p, left); if (k <= 0) _exit(96); p += k; left -= k; }
    }
    g_sh->status[which] = 1;
    _exit(0);
}

struct Report { bool ok = false; uint32_t npages = 0, nscan = 0; std::map<uint32_t, std::vector<uint8_t>> pages; };
static Report read_report(int fd) {
    Report r; lseek(fd, 0, SEEK_SET);
    uint32_t hdr[4]; if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr || hdr[0] != 0xD71D71D7u) return r;
    r.npages = hdr[1]; r.nscan = hdr[2];
    for (uint32_t i = 0; i < hdr[1]; i++) {
        uint32_t a; if (read(fd, &a, 4) != 4) return r;
        std::vector<uint8_t> d(4096); size_t got = 0;
        while (got < 4096) { ssize_t k = read(fd, d.data() + got, 4096 - got); if (k <= 0) return r; got += k; }
        r.pages[a] = std::move(d);
    }
    r.ok = true; return r;
}

static bool word_equal(uint32_t a, uint32_t b) {
    if (a == b) return true;
    float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    if (std::isfinite(fa) && std::isfinite(fb)) {
        float ma = fabsf(fa), mb = fabsf(fb);
        if ((ma > 1e-6f && ma < 1e9f) || (mb > 1e-6f && mb < 1e9f) || (ma == 0 && mb == 0)) {
            float m = std::max(1.0f, std::max(ma, mb));
            if (fabsf(fa - fb) <= 2e-4f * m) return true;
        }
    }
    return false;
}

static std::string maps_of_desc(uint32_t addr) {
    if (addr - 0x80000000u < 0x20000000u) return "";                      // the game arenas: not a host mapping of interest
    FILE* fp = fopen("/proc/self/maps", "r"); std::string out; if (!fp) return out; char line[512];
    while (fgets(line, sizeof line, fp)) { unsigned long lo, hi; char perm[8] = "", path[400] = ""; if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]", &lo, &hi, perm, path) >= 3 && addr >= lo && addr < hi) { char b[480]; const char* pp = path; while (*pp == ' ') pp++; snprintf(b, sizeof b, "%lx-%lx %s %s", lo, hi, perm, pp); out = b; break; } }
    fclose(fp); return out;
}
static std::string describe(uint32_t addr, void* goalie) {
    char b[256];
    uintptr_t g = (uintptr_t)goalie;
    if (addr >= g && addr < g + 0x1100) {
        uint32_t off = addr - g, best = 0; const char* nm = nullptr;
        for (const DtlField* f = g_fields; f->name; ++f) if (f->off <= off && f->off >= best) { best = f->off; nm = f->name; }
        snprintf(b, sizeof b, "goalie+0x%x (%s+%u)", off, nm ? nm : "?", off - best); return b;
    }
    Dl_info di;
    if (dladdr((void*)(uintptr_t)addr, &di) && di.dli_sname) { snprintf(b, sizeof b, "%s+0x%lx", di.dli_sname, (unsigned long)(addr - (uintptr_t)di.dli_saddr)); return b; }
    { size_t lo = 0, hi = g_objsyms.size(); while (lo < hi) { size_t m = (lo + hi) / 2; if (g_objsyms[m].addr <= addr) lo = m + 1; else hi = m; }
      if (lo && addr < g_objsyms[lo - 1].addr + g_objsyms[lo - 1].size) { snprintf(b, sizeof b, "%s+0x%lx", g_objsyms[lo - 1].name.c_str(), (unsigned long)(addr - g_objsyms[lo - 1].addr)); return b; } }
    { std::string m = maps_of_desc(addr); if (!m.empty()) { snprintf(b, sizeof b, "[%s]", m.c_str()); return b; } }
    snprintf(b, sizeof b, "heap %08x", addr); return b;
}

static int g_ntests = 0, g_nbad = 0, g_nflag = 0, g_nnondet = 0, g_ncrashB = 0, g_ncrashA = 0;

// child C: a copy of the process at fork time that answers page requests, so that pages dirtied by only one of A and B are compared with the
// exact start state (the parent keeps running other threads that may change its own pages meanwhile)
static void ref_server(int rfd, int wfd) {
    alarm(120);
    for (;;) {
        uint32_t a; ssize_t k = read(rfd, &a, 4); if (k != 4 || a == 0) _exit(0);
        const uint8_t* p = (const uint8_t*)(uintptr_t)a; size_t left = 4096;
        while (left) { ssize_t w = write(wfd, p, left); if (w <= 0) _exit(0); p += w; left -= w; }
    }
}

static std::string pc_name(uint64_t pc) {
    Dl_info di; char b[200];
    if (dladdr((void*)(uintptr_t)pc, &di) && di.dli_sname) { snprintf(b, sizeof b, "%s+0x%lx", di.dli_sname, (unsigned long)(pc - (uintptr_t)di.dli_saddr)); return b; }
    { size_t lo = 0, hi = g_objsyms.size(); while (lo < hi) { size_t m = (lo + hi) / 2; if (g_objsyms[m].addr <= pc) lo = m + 1; else hi = m; }
      if (lo && pc < g_objsyms[lo - 1].addr + g_objsyms[lo - 1].size) { snprintf(b, sizeof b, "%s+0x%lx", g_objsyms[lo - 1].name.c_str(), (unsigned long)(pc - g_objsyms[lo - 1].addr)); return b; } }
    snprintf(b, sizeof b, "?"); return b;
}
static std::string seg_of(uint64_t fa) {
    char b[200]; b[0] = 0;
    for (const Seg& s : g_segs) { uintptr_t n = (uintptr_t)s.nat; if (fa + 64 >= n && fa < n + (s.hi - s.lo) + 64) { snprintf(b, sizeof b, "in/near mapped static %s (retail %08x+%u, native %lx)", s.name, s.lo, s.hi - s.lo, (unsigned long)n); break; } }
    return b;
}
static std::string maps_of(uint64_t fa) {
    FILE* fp = fopen("/proc/self/maps", "r"); std::string out; if (!fp) return out; char line[512];
    while (fgets(line, sizeof line, fp)) { unsigned long lo, hi; if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && fa >= lo && fa < hi) { out = line; while (!out.empty() && (out.back() == '\n')) out.pop_back(); break; } }
    fclose(fp); return out;
}
static int one_test(const Test* t, int seq) {
    char tmp1[] = "/tmp/dtlA.XXXXXX", tmp2[] = "/tmp/dtlB.XXXXXX";
    int fa = mkstemp(tmp1), fb = mkstemp(tmp2); unlink(tmp1); unlink(tmp2);
    g_sh->status[0] = g_sh->status[1] = 0; g_sh->trace_n = 0; g_sh->xl_n = 0; g_sh->msg[0][0] = g_sh->msg[1][0] = 0;
    pthread_attr_t at; void* sa = nullptr; size_t ss = 0;
    pthread_getattr_np(pthread_self(), &at); pthread_attr_getstack(&at, &sa, &ss); pthread_attr_destroy(&at);
    uintptr_t st0 = (uintptr_t)sa, st1 = (uintptr_t)sa + ss;
    int cq[2], cr[2]; if (pipe(cq) || pipe(cr)) { say("[dtl] pipe failed\n"); return -3; }
    pid_t pc = fork();
    if (pc == 0) { close(cq[1]); close(cr[0]); ref_server(cq[0], cr[1]); }
    close(cq[0]); close(cr[1]);
    pid_t pa = fork();
    if (pa == 0) { close(cq[1]); close(cr[0]); child(0, t, fa, false, st0, st1); }
    int stA = 0, stB = 0;
    waitpid(pa, &stA, 0);
    pid_t pb = fork();
    if (pb == 0) { close(cq[1]); close(cr[0]); child(1, t, fb, !getenv("NP_DTL_CONTROL"), st0, st1); }
    waitpid(pb, &stB, 0);
    Report A = read_report(fa), B = read_report(fb);
    close(fa); close(fb);
    bool cclosed = false;
    auto closeC = [&]() { if (cclosed) return; cclosed = true; uint32_t z = 0; ssize_t w = write(cq[1], &z, 4); (void)w; close(cq[1]); close(cr[0]); int stc; waitpid(pc, &stc, 0); };
    g_ntests++;
    uint32_t stt = t->goalie ? *(volatile uint32_t*)((char*)t->goalie + 0x328) : 0;
    if (!A.ok) { g_ncrashA++; say("[dtl] test %d %s: native child failed (status %d, sh %u sig %d fault %llx pc %llx (%s)) %s [maps: %s]\n", seq, t->desc, stA, g_sh->status[0], g_sh->sig[0], (unsigned long long)g_sh->fault_addr[0], (unsigned long long)g_sh->fault_pc[0], pc_name(g_sh->fault_pc[0]).c_str(), g_sh->msg[0], maps_of(g_sh->fault_addr[0]).c_str()); closeC(); return -1; }
    if (!B.ok) {
        g_ncrashB++;
        say("[dtl] test %d %s (goalie state %u): RETAIL child failed: status %d sh %u sig %d fault %llx pc %llx (%s) %s %s\n  last calls:", seq, t->desc, stt, stB, g_sh->status[1], g_sh->sig[1], (unsigned long long)g_sh->fault_addr[1], (unsigned long long)g_sh->fault_pc[1], pc_name(g_sh->fault_pc[1]).c_str(), seg_of(g_sh->fault_addr[1]).c_str(), g_sh->msg[1]);
        say("  regs eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n", g_sh->regs[0], g_sh->regs[1], g_sh->regs[2], g_sh->regs[3], g_sh->regs[4], g_sh->regs[5], g_sh->regs[6], g_sh->regs[7]);
        { uint32_t n = g_sh->xl_n; say("  last static accesses (retail -> native):"); for (uint32_t i = n > 6 ? n - 6 : 0; i < n; i++) { uint32_t a = g_sh->xl[i & 15][0]; uint32_t off = 0; const char* nm = retail_name(a, &off); say(" %08x(%s+0x%x)->%08x", a, nm ? nm : "?", off, g_sh->xl[i & 15][1]); } say("\n"); }
        uint32_t n = g_sh->trace_n; for (uint32_t i = n > 12 ? n - 12 : 0; i < n; i++) { uint32_t a = g_sh->trace[i & 511] & ~1u; auto it = g_byaddr.find(a); say(" %08x(%s)", a, it != g_byaddr.end() ? it->second->name : "?"); }
        say("\n"); closeC(); return -2;
    }
    std::vector<uint32_t> addrs; for (auto& kv : A.pages) addrs.push_back(kv.first); for (auto& kv : B.pages) if (!A.pages.count(kv.first)) addrs.push_back(kv.first);
    std::sort(addrs.begin(), addrs.end());
    int ndiff = 0, nmirror = 0, nflag = 0; std::string out, outf;
    for (uint32_t pg : addrs) {
        std::vector<uint8_t> refpage(4096);
        if (!A.pages.count(pg) || !B.pages.count(pg)) {                                   // exact start state from child C
            ssize_t w = write(cq[1], &pg, 4); size_t got = 0; (void)w;
            while (got < 4096) { ssize_t k = read(cr[0], refpage.data() + got, 4096 - got); if (k <= 0) break; got += k; }
            if (got < 4096) memcpy(refpage.data(), (const void*)(uintptr_t)pg, 4096);
        }
        const uint8_t* pa_ = A.pages.count(pg) ? A.pages[pg].data() : refpage.data();
        const uint8_t* pb_ = B.pages.count(pg) ? B.pages[pg].data() : refpage.data();
        for (int i = 0; i < 1024; i++) {
            uint32_t wa, wb; memcpy(&wa, pa_ + 4 * i, 4); memcpy(&wb, pb_ + 4 * i, 4);
            if (word_equal(wa, wb)) continue;
            uint32_t wo; if (A.pages.count(pg) && B.pages.count(pg)) memcpy(&wo, (const uint8_t*)(uintptr_t)(pg + 4 * i), 4); else memcpy(&wo, refpage.data() + 4 * i, 4);       // (approximately) the state both children started from
            { uint32_t da = wa ^ wo, db = wb ^ wo;
              auto revw = [](uint32_t x, int W) { uint32_t r = 0; for (int b = 0; b < 32; b++) if (x & (1u << b)) r |= 1u << ((b & ~(W - 1)) | (W - 1 - (b & (W - 1)))); return r; };
              if (da && (revw(da, 32) == db || revw(da, 16) == db || revw(da, 8) == db)) { nmirror++; continue; }   // MWCC allocates bit-fields MSB first, clang LSB first: the same flag, the mirrored bit (in the 32/16/8 bit unit)
              {   // mixed word: bits both children changed alike (a native callee set them) are common; the rest must be mirror images, a retail write onto a bit that already had the value is a no-op
                  uint32_t com = da & db, na = da & ~com, nb = db & ~com;
                  bool mixok = false;
                  if (na | nb) for (int W = 32; W >= 8; W >>= 1) {
                      uint32_t ra = revw(na, W), rb = revw(nb, W);
                      bool ok = (rb & ~na) == 0 || false;                    // every retail-only change is the mirror of a native-only change
                      ok = (revw(nb, W) & ~na) == 0;
                      uint32_t missing = ra & ~nb;                           // mirror bits the retail did not change: fine if the start value already equals what native wrote at the original position
                      for (int b = 0; b < 32 && ok; b++) if (missing & (1u << b)) { int ob = -1; for (int c = 0; c < 32; c++) if ((na & (1u << c)) && (revw(1u << c, W) == (1u << b))) ob = c; if (ob < 0 || (((wo >> b) & 1) != ((wa >> ob) & 1))) ok = false; }
                      if (ok) { mixok = true; break; }
                  }
                  if (mixok) { nmirror++; continue; }
              }
              if (__builtin_popcount(da) + __builtin_popcount(db) == 1 && __builtin_popcount(wo | wa | wb) >= 3) {                                          // one side flips a single bit, the other does not touch the word: a flag the translation reads/writes in the other bit order
                  nflag++;
                  if (getenv("NP_DTL_BFDEBUG") && pg + 4 * i >= 0x90000000u) { uint32_t o = pg + 4 * i - 44, vp = *(volatile uint32_t*)(uintptr_t)o; Dl_info di2; const char* nm2 = (dladdr((void*)(uintptr_t)(vp - 8), &di2) && di2.dli_sname) ? di2.dli_sname : "?"; say("[dtl]   bfdebug: word %08x, 44 bytes earlier %08x holds %08x (%s)\n", pg + 4 * i, o, vp, nm2); }
                  if (nflag <= 6) { char b[200]; snprintf(b, sizeof b, "    %08x %-44s start %08x native %08x retail %08x  (single-bit flag word: bit-field order artefact?)\n", pg + 4 * i, describe(pg + 4 * i, t->goalie).c_str(), wo, wa, wb); outf += b; }
                  continue; } }
            ndiff++;
            if (ndiff <= 24) {
                float fa2, fb2; memcpy(&fa2, &wa, 4); memcpy(&fb2, &wb, 4);
                if (getenv("NP_DTL_BFDEBUG") && pg + 4 * i >= 0x90000000u) { uint32_t o = pg + 4 * i - 44, vp = *(volatile uint32_t*)(uintptr_t)o; Dl_info di2; const char* nm2 = (dladdr((void*)(uintptr_t)(vp - 8), &di2) && di2.dli_sname) ? di2.dli_sname : "?"; say("[dtl]   bfdebug: word %08x, 44 bytes earlier %08x holds %08x (%s)\n", pg + 4 * i, o, vp, nm2); }
                char b[300]; snprintf(b, sizeof b, "    %08x %-44s start %08x native %08x (%g)  retail %08x (%g)\n", pg + 4 * i, describe(pg + 4 * i, t->goalie).c_str(), wo, wa, fa2, wb, fb2);
                out += b;
            }
        }
    }
    closeC();
    if (ndiff && getenv("NP_DTL_TRACE")) {
        std::map<uint32_t, int> cnt; std::vector<uint32_t> order; uint32_t n = g_sh->trace_n; if (n > 512) n = 512;
        for (uint32_t i = 0; i < n; i++) { uint32_t a = g_sh->trace[i]; if (!cnt[a]++) order.push_back(a); }
        std::string tr = "    retail calls (NATIVE marked *):"; char b[200];
        for (uint32_t a : order) { auto it = g_byaddr.find(a & ~1u); snprintf(b, sizeof b, " %s%s x%d;", (a & 1) ? "*" : "", it != g_byaddr.end() ? it->second->name : "?", cnt[a]); tr += b; }
        tr += "\n"; ssize_t w = write(2, tr.data(), tr.size()); (void)w;
    }
    if (getenv("NP_DTL_PROBE") && t->goalie && strstr(t->desc, "fn_8008EF58")) {
        void** sess = (void**)dlsym(RTLD_DEFAULT, "g_pNetworkSessionBase"); uint32_t team = *(uint32_t*)((char*)t->goalie + 788);
        say("[dtl]   probe: session %p machines %d; team %08x players:", sess ? *sess : nullptr, sess && *sess ? *(int*)*sess : -1, team);
        for (int i = 0; i < 5; i++) { uint32_t pl = *(uint32_t*)(uintptr_t)(team + 164 + 4 * i); say(" %08x(ctl %08x)", pl, pl ? *(uint32_t*)(uintptr_t)(pl + 780) : 0); } say("\n");
    }
    if (getenv("NP_DTL_BFDEBUG") && t->goalie) { uint32_t c = *(volatile uint32_t*)((char*)t->goalie + 200); uint32_t* w = (uint32_t*)(uintptr_t)c; say("[dtl]   bfdebug: goalie %p controller %08x: %08x %08x %08x %08x %08x %08x ... flags@44 %08x\n", t->goalie, c, w[0], w[1], w[2], w[3], w[4], w[5], w[11]); }
    say("[dtl] test %d %s goalie state %u sub %d: %s (native %u pages, retail %u pages, %d differing words, %d bit-field mirror words, %d single-bit flag words)\n", seq, t->desc, stt, t->goalie ? *(volatile int*)((char*)t->goalie + 0x334) : -1, ndiff ? "DIFF" : nflag ? "same-except-flag-words" : "same", A.npages, B.npages, ndiff, nmirror, nflag);
    if (nflag) g_nflag++;
    out += outf;
    if (ndiff) g_nbad++;
    if (ndiff || nflag) { size_t off = 0; while (off < out.size()) { ssize_t w = write(2, out.data() + off, out.size() - off); if (w <= 0) break; off += w; } }
    return ndiff ? 1 : 0;
}

} // namespace dtl

namespace dtl {
struct FnStat { int tests = 0, diff = 0, crashA = 0, crashB = 0; };
static std::map<uint32_t, FnStat> g_fnstat;
static std::vector<const DtlFn*> g_testfns;
static size_t g_cursor = 0;

static std::vector<std::string> split(const char* s, char sep) {
    std::vector<std::string> out; std::string cur;
    for (; s && *s; ++s) { if (*s == sep) { out.push_back(cur); cur.clear(); } else cur += *s; }
    if (s) out.push_back(cur);
    return out;
}

static uint32_t player_ptr(Rng& r, bool fielder) {
    void* teams = dlsym(RTLD_DEFAULT, "g_pTeams"); if (!teams) return 0;
    uint32_t tp = ((uint32_t*)teams)[r.next() & 1]; if (!tp) return 0;
    return *(uint32_t*)(uintptr_t)(tp + 0xA4 + 4 * (r.next() % (fielder ? 4 : 5)));
}

// builds the argument registers of a test for function f (PowerPC EABI: this + integer class in r3.., floats in f1..); false: unsupported parameter
static bool build_args(Test* t, Rng& r, int variant) {
    const DtlFn* f = t->f; CpuContext* c = &t->ctx; int gi = 3, fi = 1;
    std::string d = f->name; d = d.substr(0, d.find("__")) ;
    if (f->hasThis) c->gpr[gi++] = (uint32_t)(uintptr_t)t->goalie;
    auto types = split(f->ptypes, ';');
    char tmp[64]; std::string desc = d + "(";
    size_t idx = 0;
    for (const char* k = f->kinds; *k; ++k, ++idx) {
        std::string ty = idx < types.size() ? types[idx] : "";
        if (idx) desc += ",";
        switch (*k) {
        case 'f': { float v = (std::string(f->name).compare(0, 6, "Action") == 0 || variant == 0) ? 0.02f : (float[]){ 0.0f, 0.25f, 1.0f, 2.0f }[r.next() & 3];
                    c->fpr[fi++].d = v; snprintf(tmp, sizeof tmp, "%g", v); desc += tmp; break; }
        case 'i': { uint32_t v;
                    if (ty == "b") v = r.next() & 1;
                    else if (ty == "Us") v = r.next() & 0xFFFF;
                    else if (ty.size() && (isdigit(ty[0]) || ty[0] == 'Q')) v = r.next() % 3;       // enum
                    else v = r.next() % 4;
                    c->gpr[gi++] = v; snprintf(tmp, sizeof tmp, "%u", v); desc += tmp; break; }
        case 'p': { uint32_t v = 0; std::string pt = ty.substr(0, 1); size_t j = 1; while (j < ty.size() && (ty[j] == 'C' || ty[j] == 'V')) j++;
                    std::string cls = ty.size() > j ? ty.substr(j) : "";
                    if (cls == "7cPlayer") v = player_ptr(r, false);
                    else if (cls == "8cFielder") v = player_ptr(r, true);
                    else if (cls == "6Goalie") v = (uint32_t)(uintptr_t)t->goalie;
                    else if (cls == "5cBall") { void* b = dlsym(RTLD_DEFAULT, "g_pBall"); v = b ? *(uint32_t*)b : 0; }
                    else if (cls == "9nlVector3" || cls == "9nlVector4" || cls == "f" || cls == "i" || cls == "b" || cls == "Us" || cls == "Ui") v = (uint32_t)(uintptr_t)(g_scratch + 16 * (r.next() % 8) + 256 * (cls == "9nlVector4" ? 1 : 0));
                    else if (pt == "P" && (cls == "")) v = 0;
                    else return false;                       // unknown pointer type: do not test this function
                    c->gpr[gi++] = v; snprintf(tmp, sizeof tmp, "@%x", v); desc += tmp; break; }
        default: return false;
        }
    }
    desc += ")";
    snprintf(t->desc, sizeof t->desc, "%s", desc.c_str());
    return true;
}

static void collect_testfns() {
    for (const DtlFn* f = g_fns; f->name; ++f) {
        if (!f->root || !f->hasThis || !f->nat) continue;
        if (!strstr(f->name, "__6GoalieF") && !strstr(f->name, "__6GoalieC")) continue;
        if (!g_natfn.count(f->addr) || !g_tr.count(f->addr)) continue;
        if (strncmp(f->name, "__ct", 4) == 0 || strncmp(f->name, "__dt", 4) == 0) continue;
        if (strchr(f->kinds, '?') || strchr(f->kinds, 'S') || strchr(f->kinds, 'L') || strchr(f->kinds, 'd')) continue;
        g_testfns.push_back(f);
    }
    say("[dtl] %zu goalie functions selected for per-function tests\n", g_testfns.size());
}

static Test g_tbuf;      // the live hook reuses one static Test (no game-heap allocations by the harness around the forks)
static const DtlFn* fn_by_name_prefix(const char* p) { for (const DtlFn* f = g_fns; f->name; ++f) if (strncmp(f->name, p, strlen(p)) == 0 && f->name[strlen(p)] == '_' && f->name[strlen(p) + 1] == '_') return f; return nullptr; }

// PeachPhoto: Start, a few Updates, End on the static gPeachPhotoState; compares the whole state (StartPeachPhoto/UpdatePeachPhoto/EndPeachPhoto)
static Test* build_peach(uint32_t seed) {
    const DtlFn* st = fn_by_name_prefix("StartPeachPhoto"); const DtlFn* up = fn_by_name_prefix("UpdatePeachPhoto"); const DtlFn* en = fn_by_name_prefix("EndPeachPhoto");
    void* state = dlsym(RTLD_DEFAULT, "gPeachPhotoState");
    if (!st || !up || !en || !state || !g_natfn.count(st->addr) || !g_tr.count(st->addr)) return nullptr;
    Rng r(seed * 7919u + 13); Test* t = &g_tbuf; memset(t, 0, sizeof *t); t->state = -1;
    float* v = (float*)g_scratch; v[0] = r.rng(-30, 30); v[1] = r.rng(-18, 18); v[2] = r.rng(0, 3); v[3] = 0;
    t->f = st; t->ctx.gpr[3] = (uint32_t)(uintptr_t)state; t->ctx.gpr[4] = (uint32_t)(uintptr_t)g_scratch;
    float delay = r.rng(0, 1), hw = r.rng(1, 6), hh = r.rng(1, 6);
    t->ctx.fpr[1].d = delay; t->ctx.fpr[2].d = hw; t->ctx.fpr[3].d = hh;
    int nup = 3 + (r.next() % 6);
    t->nextra = 0;
    for (int i = 0; i < nup; i++) { Step& s = t->extra[t->nextra++]; s.f = up; s.ctx.gpr[3] = (uint32_t)(uintptr_t)state; s.ctx.fpr[1].d = 0.02; s.ctx.gpr[4] = 100 + i * 3; }
    const DtlFn* rd = fn_by_name_prefix("RenderPeachPhoto"); const DtlFn* sb = fn_by_name_prefix("SetPeachPhotoTextureBorder");
    if (sb && getenv("NP_DTL_BORDER") && g_natfn.count(sb->addr) && g_tr.count(sb->addr)) { Step& s = t->extra[t->nextra++]; s.f = sb; s.ctx.gpr[3] = (uint16_t)r.next(); s.ctx.gpr[4] = r.next(); }
    if (rd && g_natfn.count(rd->addr) && g_tr.count(rd->addr) && getenv("NP_DTL_RENDER")) { Step& s = t->extra[t->nextra++]; s.f = rd; s.ctx.gpr[3] = (uint32_t)(uintptr_t)state; }
    { Step& s = t->extra[t->nextra++]; s.f = en; s.ctx.gpr[3] = (uint32_t)(uintptr_t)state; s.ctx.gpr[4] = r.next() & 1; }
    snprintf(t->desc, sizeof t->desc, "PeachPhoto(Start c=(%.1f,%.1f,%.1f) d=%.2f w=%.1f h=%.1f; %d Updates; End)", v[0], v[1], v[2], delay, hw, hh, nup);
    return t;
}

static void print_fnstats() {
    say("[dtl] per-function: tests diff crashNative crashRetail\n");
    for (auto& kv : g_fnstat) { auto it = g_byaddr.find(kv.first); say("[dtl]   %-52s %4d %4d %4d %4d\n", it->second->name, kv.second.tests, kv.second.diff, kv.second.crashA, kv.second.crashB); }
}
} // namespace dtl

extern "C" void NpDtlGoalieHook(void* goalie, float dt) {
    static int every = 60, maxtests = 100, inited = 0, nfn = 6, kcombo = 2, fuzz = 1; static int cnt[2] = { 0, 0 };
    if (g_in_child) return;
    if (!inited) {
        inited = 1; const char* e = getenv("NP_DTL"); if (!e || !*e || *e == '0') return;
        int was = NpMemGetGameThread ? NpMemGetGameThread() : 0; if (NpMemSetGameThread) NpMemSetGameThread(0);
        g_enabled = true; init();
        if (NpMemSetGameThread) NpMemSetGameThread(was);
        if (const char* v = getenv("NP_DTL_EVERY")) every = atoi(v);
        if (const char* v = getenv("NP_DTL_MAX")) maxtests = atoi(v);
        if (const char* v = getenv("NP_DTL_FNS")) nfn = atoi(v);
        if (const char* v = getenv("NP_DTL_K")) kcombo = atoi(v);
        if (const char* v = getenv("NP_DTL_FUZZ")) fuzz = atoi(v);
        if (g_mode == 3) collect_testfns();
    }
    if (!g_enabled) return;
    struct HeapSwitch { int was; HeapSwitch() { was = NpMemGetGameThread ? NpMemGetGameThread() : 0; if (NpMemSetGameThread) NpMemSetGameThread(0); } ~HeapSwitch() { if (NpMemSetGameThread) NpMemSetGameThread(was); } } heapSwitch;
    uint32_t team = *(uint32_t*)((char*)goalie + 0x314);
    int side = team ? *(int*)(uintptr_t)team : 0; side &= 1;
    static int nonidle = -1; if (nonidle < 0) { const char* e = getenv("NP_DTL_NONIDLE"); nonidle = (e && *e && *e != '0') ? 1 : 0; }
    if (nonidle && *(int*)((char*)goalie + 0x328) == 0) return;                 // only the goalie's non-idle actions (saves, dives, pounces, passes, loose balls ...)
    if (++cnt[side] % every != 0 || g_ntests >= maxtests) return;
    static uint32_t seedctr = 1;
    if (g_mode == 4) {
        for (int k = 0; k < kcombo && g_ntests < maxtests; k++) { Test* t = build_peach(seedctr++); if (!t) { say("[dtl] PeachPhoto functions unavailable\n"); g_ntests = maxtests; break; } one_test(t, g_ntests + 1); }
    } else if (g_mode != 3) {
        static const DtlFn* upd = nullptr; if (!upd) upd = g_byaddr[0x800776BCu];
        Test* t = &g_tbuf; memset(t, 0, sizeof *t); t->goalie = goalie; t->f = upd; t->fuzz = 0; t->state = -1;
        t->ctx.gpr[3] = (uint32_t)(uintptr_t)goalie; t->ctx.fpr[1].d = (double)dt; snprintf(t->desc, sizeof t->desc, "Update(%g)", (double)dt);
        int r = one_test(t, g_ntests + 1); (void)r;
    } else {
        for (int n = 0; n < nfn && g_ntests < maxtests && !g_testfns.empty(); n++) {
            const DtlFn* f = g_testfns[g_cursor++ % g_testfns.size()];
            for (int k = 0; k < kcombo && g_ntests < maxtests; k++) {
                Test* t = &g_tbuf; memset(t, 0, sizeof *t); t->goalie = goalie; t->f = f; t->fuzz = fuzz; t->seed = seedctr++; t->state = -1;
                for (const auto& e : g_fnstate) { if (!e.fn) break; if (strncmp(f->name, e.fn, strlen(e.fn)) == 0 && f->name[strlen(e.fn)] == '_' && f->name[strlen(e.fn) + 1] == '_') t->state = e.state; }
                Rng r(t->seed);
                if (!build_args(t, r, k)) break;
                int res = one_test(t, g_ntests + 1);
                FnStat& st = g_fnstat[f->addr]; st.tests++; if (res == 1) st.diff++; if (res == -1) st.crashA++; if (res == -2) st.crashB++;
            }
        }
        if (g_ntests % 20 < nfn * kcombo) print_fnstats();
    }
    say("[dtl] SUMMARY tests=%d diff=%d flag_only=%d retail_failed=%d native_failed=%d\n", g_ntests, g_nbad, g_nflag, g_ncrashB, g_ncrashA);
}

// ------------------------------------------------------------------------------------------------------------------------
// NP_DTL_SWEEP=1: offline sweep of the numeric functions (g_sweep): random arguments / POD buffers, native against the translation, no game state needed
namespace dtl {
static bool pod_type(const std::string& t) {
    static const char* ok[] = { "f", "9nlVector3", "9nlVector2", "9nlVector4", "9nlMatrix4", "12nlQuaternion", "8nlColour", "f", "i", "Ui", "Us", "s", "c", "Uc", "b", "l", "Ul", "d", nullptr };
    for (const char** o = ok; *o; ++o) if (t == *o) return true;
    return false;
}
static bool sweep_eligible(const DtlFn* f) {
    if (f->ret == 'S') return false;                       // struct returned by value: hidden result pointer (not supported by the shim)
    if (!f->nat || !g_natfn.count(f->addr) || !g_tr.count(f->addr)) return false;
    if (strchr(f->kinds, '?') || strchr(f->kinds, 'S') || strchr(f->kinds, 'L') || strchr(f->kinds, '.')) return false;
    if (strncmp(f->name, "__ct", 4) == 0 || strncmp(f->name, "__dt", 4) == 0 || strncmp(f->name, "__sinit", 7) == 0) return false;
    if (strstr(f->name, "Random") || strstr(f->name, "random")) return false;          // shared RNG state: the second run sees another state
    if (f->hasThis) {                    // POD classes only
        const char* p = strstr(f->name, "__"); if (!p) return false;
        std::string cls; for (p = p + 2; *p && *p != 'F' && *p != 'C'; ++p) cls += *p;
        if (cls == "16cPoseAccumulator") { /* synthetic accumulator, see sweep_child */ }
        else if (!pod_type(cls) || cls == "f" || cls == "i") return false;
    }
    auto types = split(f->ptypes, ';'); size_t i = 0;
    for (const char* k = f->kinds; *k; ++k, ++i) {
        std::string t = i < types.size() ? types[i] : "";
        if (*k == 'p') {
            size_t j = 0; while (j < t.size() && (t[j] == 'P' || t[j] == 'R' || t[j] == 'C' || t[j] == 'V')) j++;
            if (j == 0 || !pod_type(t.substr(j))) return false;
        }
    }
    return true;
}

struct SweepResult { uint32_t trials, bad, firstbad; char msg[400]; };
static SweepResult* g_swres = nullptr;

static void sweep_child(const DtlFn* f, int trials, uint32_t seedbase) {
    alarm(30);
    g_in_child = true; g_which = 0;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = child_fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr); sigaction(SIGBUS, &sa, nullptr); sigaction(SIGFPE, &sa, nullptr); sigaction(SIGILL, &sa, nullptr); sigaction(SIGALRM, &sa, nullptr);
    auto types = split(f->ptypes, ';');
    int bad = 0; g_swres->trials = 0; g_swres->bad = 0; g_swres->firstbad = 0; g_swres->msg[0] = 0;
    for (int tr = 0; tr < trials; tr++) {
        Rng r(seedbase * 100003u + tr * 7919u + 1);
        alignas(16) static uint8_t bufA[6][256], bufB[6][256]; int np = 0;
        CpuContext ca; setup_ctx(&ca); int gi = 3, fi = 1; std::string desc;
        auto fillbuf = [&](int k) { for (int i = 0; i < 64; i++) { uint32_t w; if (r.chance(0.8f)) { float v = r.rng(-5, 5); memcpy(&w, &v, 4); } else w = r.next() & 7; memcpy(&bufA[k][4 * i], &w, 4); } memcpy(bufB[k], bufA[k], 256); };
        char tmp[48];
        bool acc = f->hasThis && strstr(f->name, "__16cPoseAccumulator") != nullptr;
        struct Acc { alignas(16) uint8_t obj[64]; alignas(16) uint8_t hier[64]; alignas(16) uint8_t rot[8 * 32]; alignas(16) uint8_t scale[8 * 20]; alignas(16) uint8_t trans[8 * 20]; };
        alignas(16) static Acc accA, accB;
        if (acc) {
            memset(&accA, 0, sizeof accA); *(int*)(accA.hier + 0x24) = (int)(r.next() % 4); *(int*)(accA.hier + 0x28) = 4 + (int)(r.next() % 3);
            for (int n = 0; n < 8; n++) {
                float* q = (float*)(accA.rot + 32 * n); for (int j = 0; j < 4; j++) q[j] = r.rng(-1, 1); q[4] = r.rng(0.1f, 2); accA.rot[32 * n + 28] = r.chance(0.5f);
                float* sc = (float*)(accA.scale + 20 * n); for (int j = 0; j < 3; j++) sc[j] = r.rng(0.5f, 2); sc[3] = r.rng(0.1f, 2); accA.scale[20 * n + 16] = r.chance(0.5f);
                float* tr = (float*)(accA.trans + 20 * n); for (int j = 0; j < 3; j++) tr[j] = r.rng(-5, 5); tr[3] = r.rng(0.1f, 2); accA.trans[20 * n + 16] = r.chance(0.5f);
            }
            accB = accA;
            *(uint32_t*)(accA.obj + 0) = (uint32_t)(uintptr_t)accA.hier; *(uint32_t*)(accA.obj + 0x10) = (uint32_t)(uintptr_t)accA.rot; *(uint32_t*)(accA.obj + 0x14) = (uint32_t)(uintptr_t)accA.scale; *(uint32_t*)(accA.obj + 0x18) = (uint32_t)(uintptr_t)accA.trans;
            *(uint32_t*)(accB.obj + 0) = (uint32_t)(uintptr_t)accB.hier; *(uint32_t*)(accB.obj + 0x10) = (uint32_t)(uintptr_t)accB.rot; *(uint32_t*)(accB.obj + 0x14) = (uint32_t)(uintptr_t)accB.scale; *(uint32_t*)(accB.obj + 0x18) = (uint32_t)(uintptr_t)accB.trans;
            ca.gpr[gi++] = (uint32_t)(uintptr_t)accA.obj;
        } else if (f->hasThis) { fillbuf(np); ca.gpr[gi++] = (uint32_t)(uintptr_t)bufA[np]; np++; }
        size_t i = 0;
        for (const char* k = f->kinds; *k; ++k, ++i) {
            std::string t = i < types.size() ? types[i] : "";
            if (*k == 'f') { float v = r.chance(0.1f) ? (float[]){ 0.0f, 1.0f, -1.0f }[r.next() % 3] : r.rng(-10, 10); ca.fpr[fi++].d = v; snprintf(tmp, sizeof tmp, " f=%g", v); desc += tmp; }
            else if (*k == 'd') { double v = r.rng(-10, 10); ca.fpr[fi++].d = v; snprintf(tmp, sizeof tmp, " d=%g", v); desc += tmp; }
            else if (*k == 'i') { uint32_t v; if (t == "Us") v = r.next() & 0xFFFF; else if (t == "b") v = r.next() & 1; else v = r.next() % 4; ca.gpr[gi++] = v; snprintf(tmp, sizeof tmp, " i=%u", v); desc += tmp; }
            else if (*k == 'p') { if (np >= 6) { g_swres->bad = 0; return; } fillbuf(np); ca.gpr[gi++] = (uint32_t)(uintptr_t)bufA[np]; np++; }
        }
        CpuContext cb = ca;
        // the second set of buffers for the translation: rebase the pointer arguments
        { int g2 = 3, n2 = 0; if (acc) { cb.gpr[g2++] = (uint32_t)(uintptr_t)accB.obj; } else if (f->hasThis) { cb.gpr[g2++] = (uint32_t)(uintptr_t)bufB[n2++]; }
          i = 0; for (const char* k = f->kinds; *k; ++k, ++i) { if (*k == 'p') cb.gpr[g2++] = (uint32_t)(uintptr_t)bufB[n2++]; else if (*k == 'i') g2++; } }
        run_native_step(f, ca);                // runs on a copy of the context; results are in the buffers
        // the return value: run_native_step discards it, so call again through a context we keep
        CpuContext ra = ca, rb = cb;
        // buffers were modified by the first call: re-fill the native side from the retail copy (still pristine) and call with result capture
        for (int k = 0; k < np; k++) memcpy(bufA[k], bufB[k], 256);
        if (acc) { Acc keep = accB; uint32_t o = *(uint32_t*)(accA.obj + 0); (void)o; memcpy(accA.hier, keep.hier, sizeof keep.hier); memcpy(accA.rot, keep.rot, sizeof keep.rot); memcpy(accA.scale, keep.scale, sizeof keep.scale); memcpy(accA.trans, keep.trans, sizeof keep.trans); }
        { auto it = g_natfn.find(f->addr); call_native(f, it->second, &ra); }
        g_tested = f->addr;
        { CpuContext& c = g_priv.ctx; c = rb; c.gpr[1] = (uint32_t)((uintptr_t)g_gstack + g_gstack_len - 256) & ~15u; c.gpr[2] = 0x806EB060u; c.gpr[13] = 0x806E33C0u; g_tr[f->addr](&c); rb = c; }
        bool same = true; std::string why;
        for (int k = 0; k < np && same; k++) for (int w = 0; w < 64; w++) { uint32_t x, y; memcpy(&x, &bufA[k][4 * w], 4); memcpy(&y, &bufB[k][4 * w], 4); if (!word_equal(x, y)) { same = false; char b[96]; snprintf(b, sizeof b, " buf%d[%d] native %08x retail %08x", k, w, x, y); why = b; break; } }
        if (acc && same) {
            const uint8_t* pa[3] = { accA.rot, accA.scale, accA.trans }; const uint8_t* pb[3] = { accB.rot, accB.scale, accB.trans }; const int len[3] = { 8 * 32, 8 * 20, 8 * 20 };
            for (int k = 0; k < 3 && same; k++) for (int w = 0; w < len[k] / 4; w++) { uint32_t x, y; memcpy(&x, pa[k] + 4 * w, 4); memcpy(&y, pb[k] + 4 * w, 4); if (!word_equal(x, y)) { same = false; char b[96]; snprintf(b, sizeof b, " acc%s[%d] native %08x retail %08x", k == 0 ? "Rot" : k == 1 ? "Scale" : "Trans", w, x, y); why = b; break; } }
        }
        if (same && f->ret != 'v') {
            if (f->ret == 'f' || f->ret == 'd') { float x = f->pairedret ? PpcGetPs0Inline(ra.fpr[1].d) : (float)ra.fpr[1].d, y = f->pairedret ? PpcGetPs0Inline(rb.fpr[1].d) : (float)rb.fpr[1].d; if (!(x == y || fabsf(x - y) <= 2e-4f * std::max(1.0f, fabsf(x)) || (std::isnan(x) && std::isnan(y)))) { same = false; char b[64]; snprintf(b, sizeof b, " ret native %g retail %g", x, y); why = b; } }
            else if (f->ret == 'p') { }
            else { uint32_t x = ra.gpr[3], y = rb.gpr[3]; if (f->ret == 'b' || f->ret == 'u') { x &= 0xFF; y &= 0xFF; }
                   bool angleish = (f->ret == 'w' || f->ret == 's' || strstr(f->name, "ACos") || strstr(f->name, "ASin") || strstr(f->name, "ATan"));   // 16-bit angles: fused multiply-add rounding moves the last bit or two
                   if (angleish && ((int)(x - y) >= -4 && (int)(x - y) <= 4)) y = x;
                   if (x != y) { same = false; char b[64]; snprintf(b, sizeof b, " ret native %08x retail %08x", x, y); why = b; } }
        }
        g_swres->trials++;
        if (!same) { bad++; if (!g_swres->bad) { g_swres->firstbad = tr; snprintf(g_swres->msg, sizeof g_swres->msg, "%s |%s", desc.c_str(), why.c_str()); } g_swres->bad++; }
    }
    _exit(0);
}

static void sweep_all() {
    init();
    g_swres = (SweepResult*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int trials = getenv("NP_DTL_TRIALS") ? atoi(getenv("NP_DTL_TRIALS")) : 40;
    std::vector<const DtlFn*> fns;
    const char* only = getenv("NP_DTL_ONLY"); uint32_t onlya = only ? (uint32_t)strtoul(only, nullptr, 16) : 0;
    for (const uint32_t* a = g_sweep; *a; ++a) { if (onlya && *a != onlya) continue; auto it = g_byaddr.find(*a); if (it != g_byaddr.end() && sweep_eligible(it->second)) fns.push_back(it->second); }
    say("[dtl-sweep] %zu eligible functions of %zu, %d trials each\n", fns.size(), (size_t)(sizeof(g_sweep) / sizeof(g_sweep[0]) - 1), trials);
    int nbad = 0, ncrash = 0, nok = 0;
    for (const DtlFn* f : fns) {
        g_swres->trials = g_swres->bad = 0; g_swres->msg[0] = 0; g_sh->status[0] = 0;
        pid_t p = fork();
        if (p == 0) sweep_child(f, trials, f->addr);
        int st = 0; waitpid(p, &st, 0);
        if (g_swres->bad) { nbad++; say("[dtl-sweep] DIFF  %08x %-60s %u of %u trials differ; first (trial %u):%s\n", f->addr, f->name, g_swres->bad, g_swres->trials, g_swres->firstbad, g_swres->msg); }
        else if (!WIFEXITED(st) || g_swres->trials < (uint32_t)trials) { ncrash++; say("[dtl-sweep] CRASH %08x %-60s after %u trials (status %d, sig %d fault %llx %s)\n", f->addr, f->name, g_swres->trials, st, g_sh->sig[0], (unsigned long long)g_sh->fault_addr[0], g_sh->msg[0]); }
        else nok++;
    }
    say("[dtl-sweep] done: %d functions identical, %d differ, %d crashed or aborted\n", nok, nbad, ncrash);
    _exit(nbad ? 1 : 0);
}
__attribute__((constructor(65535))) static void sweep_ctor() { const char* e = getenv("NP_DTL_SWEEP"); if (e && *e == '1') sweep_all(); }
} // namespace dtl

// ------------------------------------------------------------------------------------------------------------------------
// NP_DTL_SELFTEST=1: a few checks of the machinery before the game starts (no game state needed), then exit
namespace dtl {
static int g_selftest_fail = 0;
static void expect(bool ok, const char* what) { say("[dtl-selftest] %-60s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) g_selftest_fail++; }
static void selftest() {
    init();
    CpuContext c; setup_ctx(&c);
    // 1. static mapping: lbl_806DBD44 (GoalieActions.cpp, 1.0f) read through the translation's memory model
    expect(MemoryInline::FlatReadFloat32(0x806DBD44u) == 1.0f, "retail static lbl_806DBD44 -> native symbol (1.0f)");
    expect(MemoryInline::FlatReadFloat32(0x806DBD40u) == 2.5f, "retail static lbl_806DBD40 -> native symbol (2.5f)");
    // an immutable constant from the shadow image: .sdata2 0x806E39A4 = 1.0f
    expect(MemoryInline::FlatReadFloat32(0x806E39A4u) == 1.0f, "shadow .sdata2 0x806E39A4 (1.0f)");
    for (uint32_t a = 0x806E39A0u; a < 0x806E39B0u; a += 4) say("   %08x: %08x\n", a, MemoryInline::FlatRead32(a));
    for (uint32_t a = 0x806DBBE0u; a < 0x806DBBF0u; a += 4) say("   %08x: %08x (shadow %08x)\n", a, MemoryInline::FlatRead32(a), *(uint32_t*)(g_shadow + (a - kStatLo)));
    { const Seg* sg = find_seg(0x806E39A4u); say("   value %g, seg %s [%08x,%08x)\n", MemoryInline::FlatReadFloat32(0x806E39A4u), sg ? sg->name : "none", sg ? sg->lo : 0, sg ? sg->hi : 0); }
    // 2. a translated root function: GetSavePlaneOffset (0x800776B4) vs the native one
    { typedef float (*F)(); F nf = (F)dlsym(RTLD_DEFAULT, "GetSavePlaneOffset"); setup_ctx(&c); g_tr[0x800776B4u](&c);
      expect(nf && (float)c.fpr[1].d == nf() , "translated GetSavePlaneOffset == native"); say("   = %g\n", (double)c.fpr[1].d); }
    // 3. ABI shim retail->native: nlATan2f(1, 2), nlSqrt(9, 0)
    { setup_ctx(&c); c.fpr[1].d = 1.0; c.fpr[2].d = 2.0; dtl_dispatch(0x802AA254u, &c);
      typedef float (*F)(float, float); F nf = (F)dlsym(RTLD_DEFAULT, "_Z8nlATan2fff");
      expect(nf && (float)c.fpr[1].d == nf(1.0f, 2.0f), "native call nlATan2f(1,2) through the shim"); say("   = %g\n", (double)c.fpr[1].d); }
    { setup_ctx(&c); c.fpr[1].d = 9.0; c.gpr[3] = 0; dtl_dispatch(0x802A9B98u, &c);
      expect((float)c.fpr[1].d == 3.0f, "native call nlSqrt(9) through the shim"); say("   = %g\n", (double)c.fpr[1].d); }
    // 4. integer / bool returns: IsPointInCone-like: use nlRandom (state in native statics) twice, same value from both paths is not required, just a call
    { setup_ctx(&c); c.gpr[3] = 100; dtl_dispatch(0x80332ECCu, &c); expect(c.gpr[3] < 100, "native call nlRandom(100) returns an int < 100"); say("   = %u\n", c.gpr[3]); }
    // 4b. member calls with this + float argument/return: Timer::SetSeconds / GetSeconds (retail 802B75BC / 802B760C), result compared with the native methods
    { alignas(16) uint8_t tbuf[64]; memset(tbuf, 0, sizeof tbuf);
      setup_ctx(&c); c.gpr[3] = (uint32_t)(uintptr_t)tbuf; c.fpr[1].d = 2.5; dtl_dispatch(0x802B75BCu, &c);
      setup_ctx(&c); c.gpr[3] = (uint32_t)(uintptr_t)tbuf; dtl_dispatch(0x802B760Cu, &c);
      typedef float (*GF)(void*); GF gs = (GF)dlsym(RTLD_DEFAULT, "_ZNK5Timer10GetSecondsEv");
      expect(gs && (float)c.fpr[1].d == gs(tbuf) && gs(tbuf) > 2.4f && gs(tbuf) < 2.6f, "member calls Timer::SetSeconds(2.5)/GetSeconds() through the shim"); say("   = %g\n", (double)c.fpr[1].d); }
    // 4c. positive control for undefined behaviour: cPoseAccumulator::BlendTrans (retail 8030BF88) on a synthetic accumulator, native against the translation.
    //     With the block-scope `vtemp` of the decomp (fixed in main 391cabb) clang drops the stores to vtemp.z and the native function reads stack garbage for mirrored blends.
    { auto it = g_byaddr.find(0x8030BF88u); auto nf = g_natfn.find(0x8030BF88u);
      if (it == g_byaddr.end() || nf == g_natfn.end() || !g_tr.count(0x8030BF88u)) { expect(false, "BlendTrans available (native + translation)"); }
      else {
        struct TA { float t[3]; float w; uint8_t ident; uint8_t pad[3]; };
        Rng r(4242); int bad = 0, badmirror = 0, nmirror = 0, total = 400;
        for (int k = 0; k < total; k++) {
            alignas(16) uint8_t hier[64]; memset(hier, 0, sizeof hier); *(int*)(hier + 0x24) = (int)(r.next() % 4); *(int*)(hier + 0x28) = 4 + (int)(r.next() % 3);
            TA ta[8], tb[8];
            for (int i = 0; i < 8; i++) { ta[i].t[0] = r.rng(-5, 5); ta[i].t[1] = r.rng(-5, 5); ta[i].t[2] = r.rng(-5, 5); ta[i].w = r.rng(0.1f, 2); ta[i].ident = 1; memset(ta[i].pad, 0, 3); }
            memcpy(tb, ta, sizeof ta);
            uint8_t oa[64], ob[64]; memset(oa, 0, sizeof oa); memset(ob, 0, sizeof ob);
            *(uint32_t*)(oa + 0) = *(uint32_t*)(ob + 0) = (uint32_t)(uintptr_t)hier; *(uint32_t*)(oa + 0x18) = (uint32_t)(uintptr_t)ta; *(uint32_t*)(ob + 0x18) = (uint32_t)(uintptr_t)tb;
            alignas(16) float v[4] = { r.rng(-3, 3), r.rng(-3, 3), r.rng(-3, 3), 0 };
            int node = (int)(r.next() % 7); float w = r.rng(0.05f, 1.5f); int mirror = (int)(r.next() & 1); nmirror += mirror;
            CpuContext ca; setup_ctx(&ca); ca.gpr[3] = (uint32_t)(uintptr_t)oa; ca.gpr[4] = node; ca.gpr[5] = (uint32_t)(uintptr_t)v; ca.fpr[1].d = w; ca.gpr[6] = mirror;
            CpuContext cb = ca; cb.gpr[3] = (uint32_t)(uintptr_t)ob;
            call_native(it->second, nf->second, &ca);
            g_tr[0x8030BF88u](&cb);
            bool same = true; for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) { float x = ((float*)&ta[i])[j], y = ((float*)&tb[i])[j]; if (!(fabsf(x - y) <= 1e-4f * std::max(1.0f, fabsf(x)))) same = false; }
            if (!same) { bad++; badmirror += mirror; }
        }
        say("   BlendTrans native vs translation: %d of %d random blends differ (%d of them mirrored, %d mirrored in total)\n", bad, total, badmirror, nmirror);
        expect(bad == 0, "cPoseAccumulator::BlendTrans native == translation (400 random blends)");
      } }
    { for (uint32_t ang : { 0x1000u, 0x4000u, 0xC000u }) { setup_ctx(&c); c.gpr[3] = ang; g_tested = 0x802A9CC8u; g_tr[0x802A9CC8u](&c); say("   retail nlSin(%04x) = %g\n", ang, (double)c.fpr[1].d); }
      say("   shadow doubles: %g %g\n", MemoryInline::FlatReadFloat64(0x806E6018u), MemoryInline::FlatReadFloat64(0x806E6018u - 8)); }
    { alignas(16) float q1[4] = { 1, 2, 3, 4 }, q2[4] = { 5, 6, 7, 8 };
      for (int pass = 0; pass < 2; pass++) { g_mode = pass ? 3 : 1; setup_ctx(&c); c.gpr[3] = (uint32_t)(uintptr_t)q1; c.gpr[4] = (uint32_t)(uintptr_t)q2; g_tested = 0x80368770u; dtl_dispatch(0x80368770u, &c);
        say("   nlQuatDot((1,2,3,4),(5,6,7,8)) via dispatch mode %d = %g (expect 70; paired form converted)\n", g_mode, (double)PpcGetPs0Inline(c.fpr[1].d)); }
      auto it = g_byaddr.find(0x803B2804u); say("   PSQUATDotProduct known: %d native %d translated %d\n", it != g_byaddr.end(), (int)g_natfn.count(0x803B2804u), (int)g_tr.count(0x803B2804u)); g_mode = 1; }
    // 4d. the fork driver on the BlendTrans accumulator: both children start from the same memory, native against translation, page diff
    { const DtlFn* f = g_byaddr[0x8030BF88u];
      uint8_t* mem = (uint8_t*)mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      uint8_t* hier = mem + 0x400; uint8_t* obj = mem + 0x800; uint8_t* tarr = mem + 0xC00; float* vec = (float*)(mem + 0x1400);
      *(int*)(hier + 0x24) = 1; *(int*)(hier + 0x28) = 3;
      *(uint32_t*)(obj + 0) = (uint32_t)(uintptr_t)hier; *(uint32_t*)(obj + 0x18) = (uint32_t)(uintptr_t)tarr;
      for (int i = 0; i < 8; i++) { float* t = (float*)(tarr + 20 * i); t[0] = 0.5f * i; t[1] = 1.0f + i; t[2] = 2.0f - i; t[3] = 1.0f; tarr[20 * i + 16] = 1; }
      vec[0] = 0.25f; vec[1] = -0.5f; vec[2] = 0.75f;
      for (int mirror = 0; mirror < 2; mirror++) {
          Test* t = new Test; memset(t, 0, sizeof *t); t->goalie = obj; t->f = f; t->state = -1;
          t->ctx.gpr[3] = (uint32_t)(uintptr_t)obj; t->ctx.gpr[4] = 5; t->ctx.gpr[5] = (uint32_t)(uintptr_t)vec; t->ctx.fpr[1].d = 0.5; t->ctx.gpr[6] = mirror;
          snprintf(t->desc, sizeof t->desc, "BlendTrans(node 5, mirror %d)", mirror);
          int r = one_test(t, 100 + mirror); say("   fork driver result for mirror=%d: %d (0 same, 1 diff)\n", mirror, r);
          delete t;
      } }
    { const Seg* sg = find_seg(0x804E88C0u); say("   nlATanLow seg %s native %p vt?%d lo %lx hi %lx\n", sg ? sg->name : "none", sg ? (void*)sg->nat : nullptr, sg ? (int)in_vtable(sg->nat) : -1, (unsigned long)g_vt_lo, (unsigned long)g_vt_hi);
      for (uint32_t a = 0x804E88C0u; a < 0x804E88D0u; a += 4) say("   %08x -> %08x\n", a, MemoryInline::FlatRead32(a)); }
    // 5. vtables and function tables
    expect(!g_vtv.empty(), "native vtables found");
    expect(g_natfn.count(0x800776BCu) == 1, "Goalie::Update has a native counterpart");
    expect(g_segs.size() > 1000, "mapped statics");
    say("[dtl-selftest] %s\n", g_selftest_fail ? "FAILED" : "all ok");
    _exit(g_selftest_fail ? 1 : 0);
}
__attribute__((constructor(65535))) static void selftest_ctor() { const char* e = getenv("NP_DTL_SELFTEST"); if (e && *e == '1') selftest(); }
} // namespace dtl

extern "C" void NpDtlAllocHook(unsigned long size, unsigned int alignment) {
    if (!g_in_child || !getenv("NP_DTL_ALLOCTRACE")) return;
    void* bt[24]; int n = backtrace(bt, 24); char b[96]; int l = snprintf(b, sizeof b, "[dtl] child %d alloc size %lu align %u\n", g_which, size, alignment); ssize_t w = write(2, b, l); (void)w;
    backtrace_symbols_fd(bt, n, 2);
}
