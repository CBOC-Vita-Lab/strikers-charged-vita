// 1.3 MSC_VGX_UNI_SHADOW (vitaGL patch 0024): run_vitagl_uniform_shadow_test.sh takes vgl_fill_uniform_data, vglSetUniformData, upload_uniforms (with the uni_reuse
// cache of 0018), vglGetUniformShadow and vglUniformShadowWritten from the source. Two runs of one random stream over a model of the program tables: (A) every
// write through glUniform4fv's path, (B) every write the renderer's way with the shadow on: a memcpy to the addresses vglGetUniformShadow gave, then one
// vglUniformShadowWritten per draw; uniforms it refuses keep the glUniform4fv path. At every draw the bytes the GPU reads from the two default uniform buffers
// must match; the shadow run must refuse exactly the uniforms that are not plain F32 vec4s of the program. BREAK=1: Written leaves the changed flag alone (the
// uni_reuse copy is named again: stale values); BREAK=2: no dirty flag (no copy at all); BREAK=3: a fragment write flagged as a vertex one.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char GLboolean;
typedef unsigned int GLuint;
typedef int GLint;
#define GL_FALSE 0
#define GL_TRUE 1
#define vgl_fast_memcpy memcpy
#define vgl_fast_uniform_copy memcpy
#define __fp16 uint16_t
#define decode_unif_component_simple(...) abort();
typedef enum { SCE_GXM_PARAMETER_TYPE_F32, SCE_GXM_PARAMETER_TYPE_F16, SCE_GXM_PARAMETER_TYPE_U32, SCE_GXM_PARAMETER_TYPE_S32, SCE_GXM_PARAMETER_TYPE_U16,
               SCE_GXM_PARAMETER_TYPE_S16, SCE_GXM_PARAMETER_TYPE_U8, SCE_GXM_PARAMETER_TYPE_S8 } SceGxmParameterType;
typedef struct { SceGxmParameterType type; unsigned comps, arr; } SceGxmProgramParameter;
static SceGxmParameterType sceGxmProgramParameterGetType(const SceGxmProgramParameter *p) { return p->type; }
static unsigned sceGxmProgramParameterGetComponentCount(const SceGxmProgramParameter *p) { return p->comps; }
static unsigned sceGxmProgramParameterGetArraySize(const SceGxmProgramParameter *p) { return p->arr; }
typedef struct { int unused; } SceGxmContext;
typedef enum { UNIFORM_DATA, UNIFORM_SAMPLER } uniform_type;
typedef struct { const SceGxmProgramParameter *ptr; void *fptr, *vptr; uint8_t *changed; uniform_type type; } uniform;
typedef enum { PROG_INVALID, PROG_UNLINKED, PROG_LINKED } prog_status;
typedef struct { void *chain, *alias; int idx, bind; } ubo;
typedef struct { unsigned unif_buf_size; } shader;
typedef struct {
    prog_status status;
    shader *vshader, *fshader;
    void *vert_uniforms, *frag_uniforms;
    ubo *vert_ubos, *frag_ubos;
    void *unif_vbuffer, *unif_fbuffer;
    void *unif_last[2];
    uint32_t unif_gen[2];
    uint8_t unif_changed[2];
} program;
#define MAX_CUSTOM_PROGRAMS 8
#define NUNI 6
static program progs[MAX_CUSTOM_PROGRAMS];
static uniform unis[MAX_CUSTOM_PROGRAMS * NUNI + 1];
#define get_uniform_from_ptr(ptr, offs) (&unis[(ptr)])
static GLboolean dirty_shader_vert_unifs, dirty_shader_frag_unifs, vgl_uni_reuse = GL_TRUE;
static uint32_t vgl_unif_gen = 1, vgl_framecount, ubo_offset[1];
static uint32_t vgl_uni_stat[8];   // (be26 counters: not read here)
#define vgl_wr_copy(d, s, n) memcpy((d), (s), (n))   // (0045: the write-combined copy; the same bytes)
static inline int uni_hist_eq(const void *a, const void *b, uint32_t n) { return 1; }                          // (be26 history, uni_reuse 2 and 3: the model runs uni_reuse 1, the last copy only, so these are never reached)
static inline void *uni_hist_find(void *p, int st, const void *buf, uint32_t n) { return NULL; }
static SceGxmContext ctx;
#define gxm_context (&ctx)
static void *ubo_buf_dummy[1];
#define ubo_buf ((struct { void *ptr; uint32_t last_frame; } **)ubo_buf_dummy)
static uint8_t ring[1 << 15];
static uint32_t ring_at;
static void *vgl_def_vert_buf, *vgl_def_frag_buf, *gpu_v, *gpu_f;
static void *reserve(uint32_t n) {
    if (ring_at + n >= sizeof ring) { ring_at = 0; ++vgl_unif_gen; }
    void *r = ring + ring_at;
    ring_at += n;
    return r;
}
static void *vglReserveVertexUniformBuffer(uint32_t n) { return gpu_v = vgl_def_vert_buf = reserve(n); }
static void *vglReserveFragmentUniformBuffer(uint32_t n) { return gpu_f = vgl_def_frag_buf = reserve(n); }
static void sceGxmSetVertexDefaultUniformBuffer(SceGxmContext *c, void *b) { (void)c; gpu_v = b; }
static void sceGxmSetFragmentDefaultUniformBuffer(SceGxmContext *c, void *b) { (void)c; gpu_f = b; }
static void sceGxmSetVertexUniformBuffer(SceGxmContext *c, int i, void *b) { (void)c; (void)i; (void)b; }
static void sceGxmSetFragmentUniformBuffer(SceGxmContext *c, int i, void *b) { (void)c; (void)i; (void)b; }
#include "fixture.inc"
#if defined(BREAK) && BREAK == 1
#define WRITTEN(prog, st) do { dirty_shader_vert_unifs |= ((st) & 1) != 0; dirty_shader_frag_unifs |= ((st) & 2) != 0; } while (0)
#elif defined(BREAK) && BREAK == 2
#define WRITTEN(prog, st) do { progs[(prog) - 1].unif_changed[0] |= ((st) & 1) != 0; progs[(prog) - 1].unif_changed[1] |= ((st) & 2) != 0; } while (0)
#else
#define WRITTEN(prog, st) vglUniformShadowWritten((prog), (st))
#endif

enum { VB = 160, FB = 96 };
static uint8_t vbuf[2][MAX_CUSTOM_PROGRAMS][VB], fbuf[2][MAX_CUSTOM_PROGRAMS][FB];
static shader vsh = {VB}, fsh = {FB};
// per program: uniform 0 mat4 (vertex), 1 vec4[3] (vertex), 2 vec4[2] (both stages), 3 vec4[2] (fragment), 4 an F16 vec4 (fragment: refused),
// 5 a vec2 (vertex: refused). Locations 1 + program * NUNI + k (0 and -1 mean "none" to the API).
static const SceGxmProgramParameter kMat4 = {SCE_GXM_PARAMETER_TYPE_F32, 4, 4}, kVec4x3 = {SCE_GXM_PARAMETER_TYPE_F32, 4, 3}, kVec4x2 = {SCE_GXM_PARAMETER_TYPE_F32, 4, 2},
                                    kHalf = {SCE_GXM_PARAMETER_TYPE_F16, 4, 1}, kVec2 = {SCE_GXM_PARAMETER_TYPE_F32, 2, 1};
static const unsigned kVec4s[NUNI] = {4, 3, 2, 2, 1, 1};
static void Setup(int run) {
    memset(progs, 0, sizeof progs);
    memset(vbuf[run], 0, sizeof vbuf[run]);
    memset(fbuf[run], 0, sizeof fbuf[run]);
    for (int i = 0; i < MAX_CUSTOM_PROGRAMS; ++i) {
        program *p = &progs[i];
        p->status = PROG_LINKED;
        p->vshader = &vsh; p->fshader = &fsh;
        p->vert_uniforms = p->frag_uniforms = p;
        p->unif_vbuffer = vbuf[run][i]; p->unif_fbuffer = fbuf[run][i];
        p->unif_changed[0] = p->unif_changed[1] = 1;
        uniform *u = &unis[1 + i * NUNI];
        u[0] = (uniform){&kMat4, NULL, vbuf[run][i] + 0, p->unif_changed, UNIFORM_DATA};
        u[1] = (uniform){&kVec4x3, NULL, vbuf[run][i] + 64, p->unif_changed, UNIFORM_DATA};
        u[2] = (uniform){&kVec4x2, fbuf[run][i] + 0, vbuf[run][i] + 112, p->unif_changed, UNIFORM_DATA};
        u[3] = (uniform){&kVec4x2, fbuf[run][i] + 32, NULL, p->unif_changed, UNIFORM_DATA};
        u[4] = (uniform){&kHalf, fbuf[run][i] + 64, NULL, p->unif_changed, UNIFORM_DATA};
        u[5] = (uniform){&kVec2, NULL, vbuf[run][i] + 144, p->unif_changed, UNIFORM_DATA};
    }
}
// glUniform4fv(location, count, value) as vitaGL has it (custom_shaders.c), for the F32 four-component case; the F16 / vec2 uniforms are written with their own types
static void Uniform4fv(GLint location, int count, const float *value) {
    int offs = 0;
    uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);
    vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 4, count)
}
static void UniformRaw(GLint location, const void *bytes, int size) {   // (the F16 vec4 and the vec2: plain bytes into their place, both runs alike)
    uniform *u = &unis[location];
    if (u->vptr) { memcpy(u->vptr, bytes, size); u->changed[0] = 1; dirty_shader_vert_unifs = GL_TRUE; }
    if (u->fptr) { memcpy(u->fptr, bytes, size); u->changed[1] = 1; dirty_shader_frag_unifs = GL_TRUE; }
}
static uint64_t Mix(uint64_t h, const void *d, size_t n) {
    const uint8_t *b = d;
    while (n--) h = (h ^ *b++) * 1099511628211ull;
    return h;
}
static void Draw(program *p) { upload_uniforms(); }

static uint64_t Play(int run, uint32_t seed, unsigned *refused) {
    Setup(run);
    struct { void *v, *f; uint32_t room; GLboolean ok; } sh[MAX_CUSTOM_PROGRAMS][NUNI];
    *refused = 0;
    if (run == 1)
        for (int i = 0; i < MAX_CUSTOM_PROGRAMS; ++i)
            for (int k = 0; k < NUNI; ++k) {
                sh[i][k].ok = vglGetUniformShadow(i + 1, 1 + i * NUNI + k, &sh[i][k].v, &sh[i][k].f, &sh[i][k].room) && sh[i][k].room >= kVec4s[k] * 16u;
                if (!sh[i][k].ok) ++*refused;
                // (a location of another program is refused)
                void *v, *f; uint32_t r;
                if (vglGetUniformShadow(i + 1, 1 + ((i + 1) % MAX_CUSTOM_PROGRAMS) * NUNI + k, &v, &f, &r)) { puts("a location of another program was accepted"); exit(1); }
            }
    dirty_shader_vert_unifs = dirty_shader_frag_unifs = GL_TRUE;
    ring_at = 0; vgl_unif_gen = 1; gpu_v = gpu_f = NULL;
    srand(seed);
    int cur = 0;
    uint64_t h = 14695981039346656037ull;
    for (int i = 0; i < 30000; ++i) {
        uint32_t stages = 0;
        const int r = rand() % 100;
        if (r < 25) {
            const int n = rand() % MAX_CUSTOM_PROGRAMS;
            if (n != cur) { cur = n; dirty_shader_vert_unifs = dirty_shader_frag_unifs = GL_TRUE; }
        } else if (r < 31) {   // scene end
            ++vgl_unif_gen;
        }
        // the draw's blocks: each uniform of the current program given with probability 1/3 (the renderer gives only changed blocks)
        for (int k = 0; k < NUNI; ++k) {
            if (rand() % 3) continue;
            float v[16];
            for (unsigned e = 0; e < kVec4s[k] * 4; ++e) v[e] = (float)(rand() % 1000) * 0.25f;
            const GLint loc = 1 + cur * NUNI + k;
            if (k >= 4) { UniformRaw(loc, v, 8); continue; }
            if (run == 1 && sh[cur][k].ok) {
                if (sh[cur][k].v) { memcpy(sh[cur][k].v, v, kVec4s[k] * 16); stages |= 1; }
#if defined(BREAK) && BREAK == 3
                if (sh[cur][k].f) { memcpy(sh[cur][k].f, v, kVec4s[k] * 16); stages |= 1; }
#else
                if (sh[cur][k].f) { memcpy(sh[cur][k].f, v, kVec4s[k] * 16); stages |= 2; }
#endif
            } else {
                Uniform4fv(loc, (int)kVec4s[k], v);
            }
        }
        if (stages) WRITTEN(cur + 1, stages);
        Draw(&progs[cur]);
        h = Mix(h, gpu_v, VB);
        h = Mix(h, gpu_f, FB);
    }
    return h;
}

int main(void) {
    for (uint32_t seed = 1; seed <= 30; ++seed) {
        unsigned r0, r1;
        const uint64_t a = Play(0, seed, &r0), b = Play(1, seed, &r1);
        if (a != b) { printf("seed %u: the GPU read different uniform bytes\n", seed); return 1; }
        if (r1 != 2 * MAX_CUSTOM_PROGRAMS) { printf("seed %u: %u uniforms refused, expected %u (the F16 and the vec2 of each program)\n", seed, r1, 2 * MAX_CUSTOM_PROGRAMS); return 1; }
    }
    puts("vitaGL uniform shadows: the same uniform bytes at every draw; only plain F32 vec4 uniforms of the program accepted");
    return 0;
}
