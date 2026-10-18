/* np/skingpu.h: GPU skinning of the CPU-skinned characters (W5 SKIN; np/docs/wave5-SKIN.md).
 *
 * In menus, cut-scenes and replays (task states 0x8 / 0x10 / 0x20000) the high-detail characters are skinned on the CPU
 * (ShaderSkinMesh::SoftwareSkinModel): four weights per vertex, at most 32 bones per packet, new position and normal arrays in the
 * frame arena every frame, so the renderer can neither cache the display lists nor keep the vertices. With MSC_SKIN_GPU=1 a packet
 * that qualifies (at most 32 bones, the weight and bone-number streams present, no morph delta this frame) is not skinned: its
 * streams stay the bind pose (static model memory) and its `skinnedVertices` points at an NpSkinGpuRec in the frame arena that holds
 * the bone matrices exactly as the CPU skinning reads them (copied at PrepareToRender, where the CPU path reads them). The frame send
 * (glx_SendFrame_cb) hands the record to the renderer right before the packet's Draw and switches it off right after: the state
 * travels in command order with the packet (a copy of the packet made by the two-stage pipeline carries the same pointer), never keyed
 * by array address (the same bind-pose arrays are drawn several times a frame with other matrices: identical sidekicks, shadows,
 * impostors).
 *
 * `skinnedVertices` is read by nothing but the material programs' "== 0" test (hardware palette vs CPU-skinned) and by PrepareToRender
 * (the stream address); see the report for the audit of CPU readers.
 */
#ifndef NP_SKINGPU_H
#define NP_SKINGPU_H

#ifdef NP_NATIVE
#include "NL/gl/glModel.h"

#define NP_SKIN_GPU_MAGIC 0x7FA56B11u   /* a NaN pattern: never the first float of a CPU-skinned vertex array */

struct NpSkinGpuRec
{
    unsigned long magic;            /* NP_SKIN_GPU_MAGIC */
    const NpSkinGpuRec* self;       /* this record (a second check against a vertex array that starts with the magic) */
    unsigned long nBones;           /* 1..32 */
    unsigned long nVerts;           /* unique vertices of the packet (entries of the weight / bone streams) */
    const void* weights;            /* stream 5: 4 big-endian floats per vertex */
    const void* indices;            /* stream 7: 4 bytes per vertex */
    float mats[1][12];              /* nBones x (3 x 4), GX rows: the SkinMatrices parameter as SoftwareSkinModel reads it */
};

static inline const NpSkinGpuRec* NpSkinGpuOf(const glModelPacket* p)
{
    const NpSkinGpuRec* r = (const NpSkinGpuRec*)p->skinnedVertices;
    if (r == 0 || r->magic != NP_SKIN_GPU_MAGIC || r->self != r)
        return 0;
    return r;
}

#ifdef __cplusplus
extern "C" {
#endif
int NpSkinGpuOn(void);     /* MSC_SKIN_GPU (default 0) */
int NpSkinDiagOn(void);    /* MSC_SKIN_STATS: the tag census of the draws (needs MSC_VGX_TAGCENSUS=1 in the renderer) and the per-frame skin counters */
#ifdef __cplusplus
}
#endif

#endif /* NP_NATIVE */
#endif /* NP_SKINGPU_H */
