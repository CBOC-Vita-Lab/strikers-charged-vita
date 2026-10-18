// Extension records (W5 SKIN): commands of the native port that GX itself does not have. They travel in the
// record stream like any GX call (rec::Ext, R_EXT), so they arrive at the player in command order, through the
// frame send (the job records them where the game made the call) and the two-stage pipeline, and they are seen
// by the display-list cache at the moment a list is called.
//
//   EXT_TAG       [tag, flags]   diagnostic (MSC_VGX_TAGCENSUS=1): the draws until the next tag are counted
//                                under this tag (the native port tags every packet draw with its material
//                                program and a "skinned" flag)
//   EXT_SKIN      [nBones | flags << 16, nVerts, weights (2 words), indices (2 words), nBones x 12 floats]
//                 GPU skinning of the draws that follow, until EXT_SKIN_OFF: every vertex's position and normal are
//                 the weighted sum of up to four bone matrices (3 x 4, GX row layout, as the game's CPU skinning
//                 reads them) applied to the vertex as it comes from the arrays. The weights (4 big-endian floats per
//                 vertex) and the bone indices (4 bytes per vertex) are arrays in guest memory indexed by the vertex's
//                 POSITION index. A slot after the first zero weight, and a bone index >= nBones, do not count (the
//                 game's CPU loop stops at the first zero weight and only visits bones below nBones).
//   EXT_SKIN_OFF  []             back to plain vertices
#pragma once
#include "vgx_common.h"

namespace vgx {

enum : uint32_t { EXT_TAG = 1, EXT_SKIN = 2, EXT_SKIN_OFF = 3 };
constexpr uint32_t kSkinMaxBones = 32;

// Player side (vgx_skin.cpp).
void PlayExt(uint32_t kind, const uint32_t* w, uint32_t n);

// ---- diagnostics: MSC_VGX_TAGCENSUS=1, one line per frame and tag (vgx_skin.cpp)
extern uint32_t g_diagTag, g_diagFlags;
bool TagCensusOn();
void TagCensusDraw(uint32_t verts, uint32_t vtxBytes, uint32_t idxCount, bool isStatic);
void TagCensusDecode(uint32_t verts);
void TagCensusFrame(uint64_t frame);

// ---- diagnostics: MSC_VGX_SOFTCENSUS=1 (vgx_skin.cpp): per frame, the CPU-transformed (soft) draws and how many of them repeat a draw of the
// frame before exactly (same display list and segment, same vertex count and layout, same position and normal matrices)
extern const void* g_replayList;   // the display list the cache is replaying into the stream (vgx_dlcache.cpp Emit), else null
extern uint32_t g_replaySeg;
extern uint64_t g_replayHash;      // XXH3 of the segment's decoded vertices (vgx_dlcache.cpp, made at the build): what a soft-instancing buffer holds is a function of it
bool SoftCensusOn();
void SoftCensusDraw(uint32_t count, uint32_t layoutMask, const float* mv12, const float* nrm9);
void SoftCensusFrame(uint64_t frame);

// ---- static buffers owned by others than the display-list cache (vgx_dlcache.cpp hands out the ids; W5 SKIN soft instancing)
uint32_t AllocStaticBufferId();
void FreeStaticBufferId(uint32_t id);   // queues CMD_BUF_FREE

// ---- GPU skinning state (EXT_SKIN): what the draws that follow are skinned with
struct SkinState {
    bool on = false;
    uint32_t nBones = 0;
    uint32_t nVerts = 0;               // vertices the weight / index arrays hold (positions indexed beyond do not count)
    const uint8_t* weights = nullptr;  // 4 big-endian f32 per vertex
    const uint8_t* indices = nullptr;  // 4 u8 per vertex
    uint32_t serial = 0;               // bumped by every EXT_SKIN (the uniform block is built once per state)
    float mats[kSkinMaxBones * 12] = {};
};
extern SkinState g_skin;
// MSC_VGX_SKIN=0: the player ignores EXT_SKIN (diagnostic: the bind pose is drawn).
bool SkinEnabled();

} // namespace vgx
