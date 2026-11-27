// 1.3 vglDrawElementsDirect (dependencies/vitaGL/source/custom_shaders.c, patch 0017). direct_vertex_setup, vglDrawElementsDirect and vgl_collapse_streams, extracted
// from the source by run_vitagl_direct_draw_test.sh (not copies), over a model of sceGxm and of the vertex program cache. The checks: the attributes and streams a
// direct draw patches equal the ones vgl_collapse_streams (MSC_VGX_ONESTREAM) makes for the same draw, so both paths share one patched program; constants come from stride-0
// streams in the program's attribute order; a program + key patches once and a relink forgets it; the stream pointer is the buffer's plus `base`; a failed patch or a
// bad argument draws nothing and says so.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char GLboolean;
typedef unsigned GLenum, GLuint;
typedef int GLsizei;
typedef void GLvoid;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 4
#define GL_LINES 1
#define GL_UNSIGNED_SHORT 0x1403
#define DRAW_SPEEDHACK
#define VERTEX_ATTRIBS_NUM 16
#define VGL_DIRECT_VPROGS 8
typedef uint32_t SceGxmIndexSource, SceGxmShaderPatcherId;
#define SCE_GXM_INDEX_SOURCE_INDEX_16BIT 0
enum { SCE_GXM_ATTRIBUTE_FORMAT_U8, SCE_GXM_ATTRIBUTE_FORMAT_U8N, SCE_GXM_ATTRIBUTE_FORMAT_F32 };
typedef struct { uint16_t streamIndex, offset; uint8_t format, componentCount; uint16_t regIndex; } SceGxmVertexAttribute;
typedef struct { uint16_t stride, indexSource; } SceGxmVertexStream;
typedef struct { int id; } SceGxmVertexProgram;
typedef struct { int unused; } SceGxmContext;
enum { VGL_DIRECT_F32 = 0, VGL_DIRECT_U8N = 1, VGL_DIRECT_U8 = 2 };
typedef struct { uint32_t key, enabled; uint16_t stride; uint8_t size[16], type[16]; uint16_t offset[16]; } vglDirectLayout;
typedef struct { void *ptr; uint32_t last_frame; } vbo;
typedef struct {
    uint32_t vertex_attrib_state;
    uint32_t vertex_attrib_vbo[VERTEX_ATTRIBS_NUM];
    uint32_t vertex_attrib_offsets[VERTEX_ATTRIBS_NUM];
    uint8_t vertex_attrib_size[VERTEX_ATTRIBS_NUM];
    float *vertex_attrib_value[VERTEX_ATTRIBS_NUM];
} vao;
typedef struct { int id; } shader_model;
typedef struct {
    shader_model *vshader;
    SceGxmVertexAttribute attr[VERTEX_ATTRIBS_NUM];
    uint8_t attr_map[VERTEX_ATTRIBS_NUM];
    SceGxmVertexProgram *vprog;
    unsigned attr_num;
    uint32_t direct_key[VGL_DIRECT_VPROGS];
    SceGxmVertexProgram *direct_vprog[VGL_DIRECT_VPROGS];
    uint8_t direct_next;
    uint8_t fresh_draw;   // 0040: the first draw since the link
} program;

static SceGxmContext ctx;
#define gxm_context (&ctx)
static vao vao0;
static vao *cur_vao = &vao0;
static uint32_t vgl_framecount = 7;
static uint64_t model_uploads, model_gxm_draws, model_impl;
static int patch_fail;
static unsigned patches;
static SceGxmVertexAttribute last_attr[16];
static SceGxmVertexStream last_stream[16];
static unsigned last_attr_num, last_stream_num;
static SceGxmVertexProgram programs_made[64];
static int patch_vertex_program(SceGxmShaderPatcherId id, const SceGxmVertexAttribute *a, uint32_t an, const SceGxmVertexStream *s, uint32_t sn, SceGxmVertexProgram **prog) {
    ++patches;
    last_attr_num = an; last_stream_num = sn;
    memcpy(last_attr, a, an * sizeof *a);
    memcpy(last_stream, s, sn * sizeof *s);
    if (patch_fail) return -1;
    *prog = &programs_made[patches % 64];
    return 0;
}
static const SceGxmVertexProgram *set_vprog;
static const void *set_stream[16];
static void sceGxmSetVertexProgram(SceGxmContext *c, const SceGxmVertexProgram *p) { set_vprog = p; }
static void sceGxmSetVertexStream(SceGxmContext *c, unsigned i, const void *p) { assert(i < 16); set_stream[i] = p; }
#define upload_uniforms() (++model_uploads)
#define sceKernelGetProcessTimeWide() 0ull   // (0040 first-draw timing: not modelled)
#define vgl_bt_note(k, t) ((void)(k), (void)(t))

static program prog0;
static GLuint cur_program = 1;
#define progs (&prog0 - 0)
static const vglDirectLayout *direct_layout;
static vbo *direct_vbo;
static uint32_t direct_base;
static GLboolean direct_failed;
static void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx);
static vbo buf;
#define model_vbo(name) ((name) ? &buf : NULL)
#include "direct_fixture.inc"
// The test the real _glDrawElements_CustomShadersIMPL makes, as extracted, and what glDrawElements does with its result.
static GLboolean impl(program *p) {
    ++model_impl;
    SceGxmIndexSource index_type = 0;
#include "direct_impl.inc"
    return GL_TRUE;
}
static void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx) {
    if (impl(&prog0)) ++model_gxm_draws;
}

static float c_nrm[4] = {0, 0, 1, 0}, c_col[4] = {1, 1, 1, 1}, c_tmi[4] = {60, 60, 60, 60};
static void use_program(const uint8_t *locs, unsigned n) {
    memset(&prog0, 0, sizeof prog0);
    static shader_model vs = {42};
    prog0.vshader = &vs;
    prog0.attr_num = n;
    for (unsigned i = 0; i < n; ++i) { prog0.attr_map[i] = locs[i]; prog0.attr[locs[i]].regIndex = (uint16_t)(0x10 + 4 * locs[i]); }
}
static void direct_links_again(void) {
    memset(prog0.direct_key, 0, sizeof prog0.direct_key);
    memset(prog0.direct_vprog, 0, sizeof prog0.direct_vprog);
    prog0.direct_next = 0;
}

int main(void) {
    static uint8_t mem[4096];
    buf.ptr = mem;
    const GLuint name = 1;
    // a position (0), colour (2), one texcoord (4) from the buffer; the program also reads the normal (1) and matrix index (12), which the layout lacks
    const uint8_t locs[5] = {0, 1, 2, 4, 12};
    use_program(locs, 5);
    vglDirectLayout L;
    memset(&L, 0, sizeof L);
    L.key = 0x80000015; L.enabled = (1u << 0) | (1u << 2) | (1u << 4); L.stride = 28;
    L.size[0] = 4; L.type[0] = VGL_DIRECT_F32; L.offset[0] = 0;
    L.size[2] = 4; L.type[2] = VGL_DIRECT_U8N; L.offset[2] = 16;
    L.size[4] = 2; L.type[4] = VGL_DIRECT_F32; L.offset[4] = 20;
    vao0.vertex_attrib_size[1] = 4; vao0.vertex_attrib_size[12] = 4;
    vao0.vertex_attrib_value[1] = c_nrm; vao0.vertex_attrib_value[12] = c_tmi;

    assert(vglDrawElementsDirect(GL_TRIANGLES, 6, 12, name, 280, &L) == GL_TRUE);
    assert(patches == 1 && model_gxm_draws == 1);
    assert(prog0.vprog == set_vprog && set_vprog);
    assert(last_attr_num == 5 && last_stream_num == 3);
    assert(last_stream[0].stride == 28 && last_stream[1].stride == 0 && last_stream[2].stride == 0);
    assert(last_attr[0].streamIndex == 0 && last_attr[0].offset == 0 && last_attr[0].format == SCE_GXM_ATTRIBUTE_FORMAT_F32 && last_attr[0].componentCount == 4 && last_attr[0].regIndex == 0x10);
    assert(last_attr[1].streamIndex == 1 && last_attr[1].format == SCE_GXM_ATTRIBUTE_FORMAT_F32 && last_attr[1].componentCount == 4 && last_attr[1].regIndex == 0x14);
    assert(last_attr[2].streamIndex == 0 && last_attr[2].offset == 16 && last_attr[2].format == SCE_GXM_ATTRIBUTE_FORMAT_U8N);
    assert(last_attr[3].streamIndex == 0 && last_attr[3].offset == 20 && last_attr[3].componentCount == 2);
    assert(last_attr[4].streamIndex == 2);
    assert(set_stream[0] == mem + 280 && set_stream[1] == c_nrm && set_stream[2] == c_tmi);
    assert(buf.last_frame == vgl_framecount);
    assert(!direct_layout && !direct_vbo);

    // the same draw as vgl_collapse_streams sees it (base 280): the same attributes and streams, so one patched program serves both paths
    {
        SceGxmVertexAttribute in[5], out[5];
        SceGxmVertexStream ins[5], outs[5];
        memset(in, 0, sizeof in); memset(ins, 0, sizeof ins);
        const uint8_t fmt[5] = {SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_U8N, SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_F32};
        const uint8_t cnt[5] = {4, 4, 4, 2, 4};
        const uint32_t off[5] = {280, 0, 296, 300, 0};
        const uint8_t on[5] = {1, 0, 1, 1, 0};
        vao0.vertex_attrib_state = (1u << 0) | (1u << 2) | (1u << 4);
        for (int i = 0; i < 5; ++i) {
            in[i].regIndex = (uint16_t)(0x10 + 4 * locs[i]);
            in[i].format = fmt[i];
            in[i].componentCount = cnt[i];
            ins[i].stride = on[i] ? 28 : 0;
            if (on[i]) { vao0.vertex_attrib_vbo[locs[i]] = name; vao0.vertex_attrib_offsets[locs[i]] = off[i]; }
        }
        void *ptr0 = NULL;
        const uint32_t n = vgl_collapse_streams(&prog0, in, ins, out, outs, &ptr0);
        assert(n == 3 && ptr0 == mem + 280);
        assert(!memcmp(out, last_attr, sizeof out));
        assert(!memcmp(outs, last_stream, n * sizeof outs[0]));
    }

    // same program and key at another base: no patch, only the pointer moves
    assert(vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 56, &L));
    assert(patches == 1 && set_stream[0] == mem + 56);
    // another key patches; the first comes back without a patch
    vglDirectLayout L2 = L;
    L2.key = 0x80000005; L2.enabled = 1u; L2.stride = 16;
    vao0.vertex_attrib_size[2] = 4; vao0.vertex_attrib_size[4] = 4;
    vao0.vertex_attrib_value[2] = c_col; vao0.vertex_attrib_value[4] = c_col;
    assert(vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &L2));
    assert(patches == 2 && last_stream_num == 5);
    assert(vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &L));
    assert(patches == 2);
    // nine keys in a program of eight entries: the oldest is patched again, nothing is overrun
    for (uint32_t k = 0; k < 9; ++k) { vglDirectLayout K = L; K.key = 0x90000000u + k; assert(vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &K)); }
    assert(prog0.direct_next < VGL_DIRECT_VPROGS);
    // a relink forgets the keys (glLinkProgram, glCreateProgram: checked in the source by the run script)
    const unsigned p0 = patches;
    direct_links_again();
    assert(vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &L));
    assert(patches == p0 + 1);

    // a patch that fails is not published, nothing is drawn, and the caller is told
    const uint64_t d0 = model_gxm_draws;
    direct_links_again();
    patch_fail = 1;
    assert(vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &L) == GL_FALSE);
    patch_fail = 0;
    assert(model_gxm_draws == d0 && !prog0.direct_vprog[0] && !direct_layout);
    // the next draw is a fresh attempt
    assert(vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &L) == GL_TRUE && model_gxm_draws == d0 + 1);

    // what is refused before anything happens
    const uint64_t i0 = model_impl;
    assert(!vglDrawElementsDirect(GL_LINES, 3, 0, name, 0, &L));
    assert(!vglDrawElementsDirect(GL_TRIANGLES, 0, 0, name, 0, &L));
    assert(!vglDrawElementsDirect(GL_TRIANGLES, 3, 0, 0, 0, &L));
    assert(!vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, NULL));
    buf.ptr = NULL;
    assert(!vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &L));
    buf.ptr = mem;
    cur_program = 0;
    assert(!vglDrawElementsDirect(GL_TRIANGLES, 3, 0, name, 0, &L));
    cur_program = 1;
    assert(model_impl == i0);
    puts("PASS: vglDrawElementsDirect: program shape, stream pointers, per-key patching, failure paths");
    return 0;
}
