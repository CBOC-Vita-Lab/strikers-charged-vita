// 1.3 vglSetDrawCaches (dependencies/vitaGL/source, patch 0018): the macros and helpers are taken from the source by run_vitagl_draw_caches_test.sh (not copies), over a model
// of sceGxm: a random stream of program switches, blend changes, uniform writes (to any program), scene ends and ring wraps is played with the caches off and on. At every
// draw the program the GPU would use and the bytes it would read from both default uniform buffers must be the same; the on run must also make fewer patcher calls and copies.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char GLboolean;
#define GL_FALSE 0
#define GL_TRUE 1
#define VGL_FRAG_MEMO 8
#define vgl_fast_memcpy memcpy
#define vgl_memset memset
#define FP(i) ((SceGxmFragmentProgram *)(uintptr_t)(0x1000u + (i)))
typedef struct { int unused; } SceGxmFragmentProgram;
typedef struct { int unused; } SceGxmContext;
typedef int SceGxmMultisampleMode, SceGxmParameterType;
typedef union { uint32_t raw; } blend_config;
enum { SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 };
#define UNIF_N 16
#define VGL_UNI_HIST 4
#define VGL_UNI_HIST_BUDGET (1536u * 1024u)
static uint32_t vgl_uni_stat[7], vgl_uni_hist_bytes, vgl_uni_hist_cap;
static void *vglMalloc(uint32_t n) { return malloc(n); }
#define vgl_free free
#define vgl_wr_copy(d, s, n) memcpy(d, s, n)
typedef struct {
    struct { int id; unsigned unif_buf_size; const void *prog; } *vshader, *fshader;
    void *vert_uniforms, *frag_uniforms, *vert_ubos, *frag_ubos;
    blend_config blend_info;
    GLboolean is_fbo_float;
    SceGxmMultisampleMode msaa;
    SceGxmFragmentProgram *fprog;
    void *unif_vbuffer, *unif_fbuffer;
    uint32_t memo_blend[VGL_FRAG_MEMO];
    SceGxmFragmentProgram *memo_fprog[VGL_FRAG_MEMO];
    uint8_t memo_mode[VGL_FRAG_MEMO];
    uint8_t memo_next;
    void *unif_last[2];
    uint32_t unif_gen[2];
    uint8_t unif_changed[2];
    void *unif_hptr[2][VGL_UNI_HIST];
    uint32_t unif_hgen[2][VGL_UNI_HIST];
    uint8_t *unif_hdata[2];
    uint32_t unif_hsize[2];
    uint8_t unif_hnext[2];
} program;
typedef struct { void *fptr, *vptr; uint8_t *changed; const void *ptr; } uniform;
typedef struct { void *chain, *alias; int idx, bind; } ubo;
static blend_config blend_info;
static GLboolean is_fbo_float, vgl_frag_memo, vgl_uni_reuse, vgl_robust_scenes = 1;
static SceGxmMultisampleMode cur_msaa;
static GLboolean dirty_shader_vert_unifs, dirty_shader_frag_unifs;
static uint32_t vgl_unif_gen = 1;
static SceGxmContext ctx;
#define gxm_context (&ctx)
static uint8_t ring[1 << 16];
static uint32_t ring_at, copies, patcher_calls;
static void *vgl_def_vert_buf, *vgl_def_frag_buf, *gpu_v, *gpu_f;
static SceGxmFragmentProgram *gpu_fp;
static void *reserve(uint32_t n) {
    if (ring_at + n >= sizeof ring) { ring_at = 0; ++vgl_unif_gen; }
    void *r = ring + ring_at;
    ring_at += n;
    ++copies;
    return r;
}
static void *vglReserveVertexUniformBuffer(uint32_t n) { return gpu_v = vgl_def_vert_buf = reserve(n); }
static void *vglReserveFragmentUniformBuffer(uint32_t n) { return gpu_f = vgl_def_frag_buf = reserve(n); }
static void sceGxmSetVertexDefaultUniformBuffer(SceGxmContext *c, void *b) { (void)c; gpu_v = b; }
static void sceGxmSetFragmentDefaultUniformBuffer(SceGxmContext *c, void *b) { (void)c; gpu_f = b; }
static void sceGxmSetFragmentProgram(SceGxmContext *c, SceGxmFragmentProgram *f) { (void)c; gpu_fp = f; }
static void sceGxmSetVertexUniformBuffer(SceGxmContext *c, int i, void *b) { (void)c; (void)i; (void)b; }
static void sceGxmSetFragmentUniformBuffer(SceGxmContext *c, int i, void *b) { (void)c; (void)i; (void)b; }
static void *ubo_buf_dummy[1];
#define ubo_buf ((struct { void *ptr; uint32_t last_frame; } **)ubo_buf_dummy)
static uint32_t ubo_offset[1], vgl_framecount;
// the patcher: one object per combination
static int patch_fragment_program_model(int id, int fmt, SceGxmFragmentProgram **out) {
    ++patcher_calls;
    *out = FP(((uint32_t)id * 4099u + blend_info.raw * 8u + (uint32_t)fmt * 2u + (uint32_t)cur_msaa) & 0xFFFFu);
    return 0;
}
#define rebuild_frag_shader(x, y, z, w) patch_fragment_program_model(x, w, y)
#include "fixture.inc"

#define NPROG 6
static program progs[2][NPROG];
static uint8_t vb[2][NPROG][64], fb[2][NPROG][48];
static uniform uni[2][NPROG][4]; // 0, 1 vertex (1 also fragment), 2, 3 fragment
static uint32_t rv[2][NPROG], rf[2];
static struct { int id; unsigned unif_buf_size; const void *prog; } vs[NPROG], fs[NPROG];

static void Reset(int run) {
    for (int i = 0; i < NPROG; ++i) { free(progs[run][i].unif_hdata[0]); free(progs[run][i].unif_hdata[1]); }
    vgl_uni_hist_bytes = 0;
    memset(progs[run], 0, sizeof progs[run]);
    memset(vb[run], 0, sizeof vb[run]);
    memset(fb[run], 0, sizeof fb[run]);
    for (int i = 0; i < NPROG; ++i) {
        program *p = &progs[run][i];
        vs[i].unif_buf_size = 64; fs[i].unif_buf_size = 48; vs[i].id = i; fs[i].id = 100 + i;
        p->vshader = (void *)&vs[i]; p->fshader = (void *)&fs[i];
        p->vert_uniforms = p->frag_uniforms = p;
        p->unif_vbuffer = vb[run][i]; p->unif_fbuffer = fb[run][i];
        p->blend_info.raw = ~0u;
        program_caches_reset(p);
        uni[run][i][0] = (uniform){NULL, vb[run][i], p->unif_changed, 0};
        uni[run][i][1] = (uniform){fb[run][i] + 16, vb[run][i] + 16, p->unif_changed, 0};
        uni[run][i][2] = (uniform){fb[run][i] + 32, NULL, p->unif_changed, 0};
        uni[run][i][3] = (uniform){fb[run][i] + 40, NULL, p->unif_changed, 0};
    }
}
static GLboolean Draw(program *p) {
    setup_frag_program();
    upload_uniforms();
    return GL_TRUE;
}
#define sceGxmProgramParameterGetType(x) 0
#define vglSetUniformData(...) abort()
#define SCE_GXM_PARAMETER_TYPE_F32 0
#define SCE_GXM_PARAMETER_TYPE_S32 1
static void Write(uniform *u, const uint32_t *data) {
    const int offs = 0, size = 2, count = 1, type = SCE_GXM_PARAMETER_TYPE_F32;
    (void)count;
    vgl_fill_uniform_data(data, type, size, count);
}

typedef struct { uint32_t copies, patcher; uint64_t hash; uint32_t bad; } Result;
static uint64_t Mix(uint64_t h, const void *d, size_t n) {
    const uint8_t *b = d;
    while (n--) h = (h ^ *b++) * 1099511628211ull;
    return h;
}
// one stream; uses `seed` so both runs see the same operations
static Result Play(int run, GLboolean memo, GLboolean reuse, uint32_t seed, uint8_t bug) {
    const uint32_t mod = seed > 20 ? 3u : 0u;   // seeds above 20: values from {0,1,2}, so states repeat and the history has something to find
    Reset(run);
    memset(vgl_uni_stat, 0, sizeof vgl_uni_stat);
    vgl_frag_memo = memo; vgl_uni_reuse = reuse;
    dirty_shader_vert_unifs = dirty_shader_frag_unifs = 1;
    ring_at = 0; copies = 0; patcher_calls = 0; vgl_unif_gen = 1; blend_info.raw = 0; is_fbo_float = 0; cur_msaa = 0;
    gpu_v = gpu_f = NULL; gpu_fp = NULL;
    srand(seed);
    int cur = 0;
    uint64_t h = 14695981039346656037ull;
    for (int i = 0; i < 40000; ++i) {
        const int r = rand() % 100;
        if (r < 30) {   // glUseProgram (same id: nothing)
            const int n = rand() % NPROG;
            if (n != cur) { cur = n; dirty_shader_vert_unifs = dirty_shader_frag_unifs = 1; }
        } else if (r < 55) {   // glUniform on any program's location
            const int pi = (rand() % 4) ? cur : rand() % NPROG;
            uint32_t d[2] = {(uint32_t)rand(), (uint32_t)rand()};
            if (mod) { d[0] %= mod; d[1] %= mod; }
            if (!(bug == 1 && pi != cur)) Write(&uni[run][pi][rand() % 4], d);
            else { uniform *u = &uni[run][pi][rand() % 4]; if (u->vptr) memcpy(u->vptr, d, 8); if (u->fptr) memcpy(u->fptr, d, 8); }
            dirty_shader_vert_unifs = dirty_shader_frag_unifs = 1;
        } else if (r < 62) { blend_info.raw = (uint32_t)rand() % 5;
        } else if (r < 64) { is_fbo_float = !is_fbo_float;
        } else if (r < 66) { cur_msaa = rand() % 3;
        } else if (r < 69) {   // scene end
            if (bug != 2) ++vgl_unif_gen;
            dirty_shader_vert_unifs = dirty_shader_frag_unifs = (rand() & 1) ? 1 : dirty_shader_vert_unifs;
        }
        if (Draw(&progs[run][cur]) != GL_TRUE) abort();
        h = Mix(h, &gpu_fp, sizeof gpu_fp);
        h = Mix(h, gpu_v, 64);
        h = Mix(h, gpu_f, 48);
    }
    Result res = {copies, patcher_calls, h, vgl_uni_stat[6]};
    return res;
}

int main(void) {
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        const Result off = Play(0, 0, 0, seed, 0);
        const Result m = Play(1, 1, 0, seed, 0), u = Play(1, 0, 1, seed, 0), both = Play(1, 1, 1, seed, 0), h = Play(1, 0, 2, seed, 0), hb = Play(1, 1, 3, seed, 0);
        if (off.hash != m.hash || off.hash != u.hash || off.hash != both.hash || off.hash != h.hash || off.hash != hb.hash) { printf("seed %u: different output\n", seed); return 1; }
        if (!(m.patcher < off.patcher)) { printf("seed %u: memo saved nothing (%u, %u)\n", seed, m.patcher, off.patcher); return 1; }
        if (!(u.copies < off.copies)) { printf("seed %u: reuse saved nothing\n", seed); return 1; }
        if (hb.bad) { printf("seed %u: verify mode saw %u reuses that differ from the buffer\n", seed, hb.bad); return 1; }
        if (h.copies > u.copies) { printf("seed %u: the history made more copies than the last-copy reuse (%u, %u)\n", seed, h.copies, u.copies); return 1; }
        if (seed > 20 && !(h.copies < u.copies)) { printf("seed %u: the history saved nothing (%u, %u)\n", seed, h.copies, u.copies); return 1; }
        if (seed == 21) printf("history: copies %u (reuse 1) -> %u (reuse 2)\n", u.copies, h.copies);
        if (seed == 1) printf("patcher calls %u -> %u, copies %u -> %u\n", off.patcher, m.patcher, off.copies, u.copies);
    }
    // the test can fail: a write that does not mark, and a scene end that does not move the generation (the second only shows as a buffer across scenes, so it is checked by name)
    { const Result off = Play(0, 0, 0, 7, 0), bad = Play(1, 0, 1, 7, 1); if (off.hash == bad.hash) { puts("a write that does not mark went unseen"); return 1; } }
    // gem2b vglSetUniHistCap: a cap below every buffer's size makes the history act as the last-copy reuse alone, a cap that holds one stage's buffers (fs 48 B, vs 64 B) keeps its hits; the picture never differs
    for (uint32_t seed = 21; seed <= 30; ++seed) {
        const Result u = Play(1, 0, 1, seed, 0), full = Play(1, 0, 2, seed, 0);
        vgl_uni_hist_cap = 32; const Result c0 = Play(1, 0, 2, seed, 0);
        vgl_uni_hist_cap = 48; const Result c48 = Play(1, 0, 2, seed, 0);
        vgl_uni_hist_cap = 64; const Result c64 = Play(1, 0, 3, seed, 0);
        vgl_uni_hist_cap = 0;
        if (c0.hash != full.hash || c48.hash != full.hash || c64.hash != full.hash) { printf("seed %u: the cap changed the picture\n", seed); return 1; }
        if (c0.copies != u.copies) { printf("seed %u: cap 32 should leave only the last-copy reuse (%u, %u)\n", seed, c0.copies, u.copies); return 1; }
        if (!(c48.copies > full.copies && c48.copies < u.copies)) { printf("seed %u: cap 48 should keep the fragment history only (%u, %u, %u)\n", seed, full.copies, c48.copies, u.copies); return 1; }
        if (c64.copies != full.copies || c64.bad) { printf("seed %u: cap 64 should keep everything (%u, %u)\n", seed, c64.copies, full.copies); return 1; }
    }
    puts("vitaGL draw caches: the history cap keeps the picture and cuts the history by size");
    puts("vitaGL draw caches: same program and same uniform bytes at every draw");
    return 0;
}
