// The front-end / back-end seam.
//
// The front end (vgx_api.cpp, vgx_state.cpp, vgx_draw.cpp, vgx_texcache.cpp,
// vgx_shadergen.cpp) turns GX API calls and command streams into a CmdBuf: a
// list of self-contained commands plus the arenas they point into. Nothing in
// a CmdBuf refers to guest memory or to mutable front-end state, so a CmdBuf
// can be handed to another thread and executed later. The back end
// (vgx_backend_gl.cpp) only ever sees CmdBufs.
#pragma once
#include "vgx_common.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace vgx {

// ---------------------------------------------------------------- vertices
// Decoded vertices are interleaved, tightly packed in this order:
//   pos    4 x f32  (x, y, z, position-matrix row index)      always
//   nrm    3 x f32                                            VL_NRM
//   col0   4 x u8 (normalised)                                VL_COL0
//   col1   4 x u8                                             VL_COL1
//   texN   2 x f32 each                                       VL_TEX0 << N
//   tmi    8 x u8 (texture-matrix row index per texcoord)     VL_TMI
//   skin   4 x u8 (bone row index: 3 x bone number) + 4 x f32 (weights)   VL_SKIN (W5 SKIN, vgx_ext.h: GPU skinning)
enum : uint32_t {
    VL_NRM = 1u << 0,
    VL_COL0 = 1u << 1,
    VL_COL1 = 1u << 2,
    VL_TEX0 = 1u << 3, // .. VL_TEX0 << 7
    VL_TMI = 1u << 11,
    VL_SKIN = 1u << 12,
    VL_ALLTEX = 0xFFu << 3,
};
struct VtxLayout {
    uint32_t mask = 0;
    uint16_t stride = 16;
    uint8_t offNrm = 0, offCol0 = 0, offCol1 = 0, offTmi = 0;
    uint8_t offTex[8] = {};
    uint8_t offSkin = 0;
};
inline VtxLayout MakeLayout(uint32_t mask) {
    VtxLayout l;
    l.mask = mask;
    uint32_t o = 16;
    if (mask & VL_NRM) { l.offNrm = static_cast<uint8_t>(o); o += 12; }
    if (mask & VL_COL0) { l.offCol0 = static_cast<uint8_t>(o); o += 4; }
    if (mask & VL_COL1) { l.offCol1 = static_cast<uint8_t>(o); o += 4; }
    for (int i = 0; i < 8; ++i)
        if (mask & (VL_TEX0 << i)) { l.offTex[i] = static_cast<uint8_t>(o); o += 8; }
    if (mask & VL_TMI) { l.offTmi = static_cast<uint8_t>(o); o += 8; }
    if (mask & VL_SKIN) { l.offSkin = static_cast<uint8_t>(o); o += 20; }
    l.stride = static_cast<uint16_t>(o);
    return l;
}
// Fixed attribute locations (bound by name before linking).
enum : int { AT_POS = 0, AT_NRM = 1, AT_COL0 = 2, AT_COL1 = 3, AT_TEX0 = 4, AT_TMI0 = 12, AT_TMI1 = 13, AT_BIX = 14, AT_BW = 15, AT_COUNT = 16 };

// ---------------------------------------------------------- uniform blocks
// Uniform values travel as blocks of floats in CmdBuf::uni. A draw names one
// offset per block; the back end skips blocks whose offset it already applied
// to the program. Sizes are in floats.
enum : int {
    UB_PROJ = 0, // mat4 (column-major, Y flip and GX->GL depth remap folded in)
    UB_MV,       // posMtx 3 x vec4, nrmMtx 3 x vec4(xyz used)
    UB_TEXM,     // texMtx 8 x 3 x vec4, postMtx 8 x 3 x vec4
    UB_PAL,      // posMtx memory 64 x vec4, nrmMtx memory 32 x vec4(xyz used)
    UB_LIGHT,    // amb 2 x vec4, mat 2 x vec4, lights 8 x {pos, dir, col, cosatt, distatt}
    UB_TEV,      // see TevBlock in vgx_draw.cpp
    UB_COUNT
};
constexpr uint32_t kUbSize[UB_COUNT] = {16, 24, 192, 384, 176, 112};
constexpr uint32_t kNoBlock = 0xFFFFFFFFu;
// W5 SKIN: the bone matrices of a GPU-skinned draw (vgx_ext.h): 32 bones x 3 rows (GX row layout), a block of their own outside the
// six above (DrawRec::skinBlock; draws without skinning name none, so their records and hashes are as before).
constexpr uint32_t kSkinBlockFloats = 32 * 12;
// MSC_VGX_SKINPAL: the bone palette of a skinned draw comes in a few sizes. VsKey::Chan::skin names it: 0 not skinned, 1 the whole 32-bone palette (the
// value of every key and shader list written before the buckets), 2.. a smaller one (SkinBones). A draw gets the smallest that holds the skin state's
// bones (no vertex names a bone at or past the count, see SkinExpand), so its uniform block and the shader's u_bone array are that small.
constexpr uint8_t kSkinBucketMax = 4;
inline uint32_t SkinBones(uint8_t skin) {
    static const uint8_t bones[kSkinBucketMax + 1] = {0, 32, 4, 10, 16}; // (host census: skin states of 1..9 bones, none above 9)
    return skin <= kSkinBucketMax ? bones[skin] : 32u;
}
// Layout of UB_TEV (float indices)
enum : int {
    TEV_REG = 0,      // 4 x vec4: PREV, REG0, REG1, REG2 (0..1 scale, may be outside for S10)
    TEV_KONST = 16,   // 4 x vec4
    TEV_MISC = 32,    // alphaRef0, alphaRef1 (0..255), dstAlpha (0..1), zTexBias
    TEV_FOG = 36,     // A', B', C, unused
    TEV_FOGCOL = 40,  // rgb
    TEV_INDMTX = 44,  // 3 x {vec4 (m00,m01,m02,scale), vec4 (m10,m11,m12,0)}
    TEV_INDSCALE = 68, // 4 x vec4: (coordScaleS, coordScaleT, 1/texW, 1/texH) per indirect stage
    TEV_TEXSIZE = 84,  // 4 x vec4: (w, h) of texmap 2n in xy, of texmap 2n+1 in zw
};

// ------------------------------------------------------------ render state
enum : uint8_t { BF_ZERO, BF_ONE, BF_SRC_COLOR, BF_INV_SRC_COLOR, BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_DST_ALPHA, BF_INV_DST_ALPHA, BF_DST_COLOR, BF_INV_DST_COLOR };
enum : uint8_t { BE_ADD, BE_REVSUB };
enum : uint8_t { CULL_NONE, CULL_BACK, CULL_FRONT, CULL_ALL }; // GX "front" = clockwise on screen
enum : uint8_t { CMP_NEVER, CMP_LESS, CMP_EQUAL, CMP_LEQUAL, CMP_GREATER, CMP_NEQUAL, CMP_GEQUAL, CMP_ALWAYS };
struct RenderState {
    uint8_t blend = 0, blendSrc = BF_ONE, blendDst = BF_ZERO, blendEq = BE_ADD;
    uint8_t depthTest = 0, depthFunc = CMP_LEQUAL, depthWrite = 0, cull = CULL_NONE;
    uint8_t colorWrite = 1, alphaWrite = 1;
    uint8_t dstAlphaPass = 0; // second, alpha-only pass with constant alpha (blend + GXSetDstAlpha)
    uint8_t lineWidth6 = 6;   // GX line width / point size in 1/6 pixel
    // Scissor and viewport in EFB pixels, origin top-left. The back end scales
    // them to the render target.
    int16_t scX = 0, scY = 0, scW = 640, scH = 528;
    float vpX = 0, vpY = 0, vpW = 640, vpH = 528, zNear = 0, zFar = 1;
};

// What the target GPU's shader patcher keys a fragment program on besides the shaders:
// the blend function (or none) and the colour mask. 12 bits. (vitaGL: blend_info in
// blending.c, compared per draw in setup_frag_program, custom_shaders.c.)
inline uint32_t BlendKey(const RenderState& rs) {
    return (rs.blend ? 1u | (static_cast<uint32_t>(rs.blendEq & 1u) << 1) | (static_cast<uint32_t>(rs.blendSrc & 15u) << 2) |
                           (static_cast<uint32_t>(rs.blendDst & 15u) << 6)
                     : 0u) |
           (rs.colorWrite ? 1u << 10 : 0u) | (rs.alphaWrite ? 1u << 11 : 0u);
}
// CMD_VARIANT: this program will be drawn with this vertex layout, blend function and
// colour mask. On the console a patched vertex program exists per (vertex shader, layout)
// and a patched fragment program per (program, blend function, colour mask), and each is
// built at the first draw that needs it (milliseconds). The back end draws one empty
// triangle now so that it is built here instead (shader warm-up list).
struct VariantCmd {
    uint16_t vsId = 0, fsId = 0;
    uint16_t layoutMask = 0, blendKey = 0;
};

enum : uint8_t { WRAP_CLAMP, WRAP_REPEAT, WRAP_MIRROR };
enum : uint8_t { FILT_NEAR, FILT_LINEAR, FILT_NEAR_MIP_NEAR, FILT_LIN_MIP_NEAR, FILT_NEAR_MIP_LIN, FILT_LIN_MIP_LIN };
struct TexBind {
    uint32_t texId = 0; // 0 = none (back end binds a 1x1 white texture)
    uint8_t wrapS = WRAP_CLAMP, wrapT = WRAP_CLAMP, minFilter = FILT_LINEAR, magFilter = FILT_LINEAR;
};

enum : uint8_t { PRIM_TRIANGLES, PRIM_LINES, PRIM_POINTS };
// Draw classes (DrawRec::cls; the game's NPFX_* in np/fx.h): below kClsBgFirst foreground, from it background.
enum : uint8_t { CLS_UNKNOWN = 0, CLS_PLAYER = 1, CLS_ITEM = 2, CLS_FX = 3, CLS_HUD = 4, CLS_NPC = 5, CLS_BG_FIRST = 8, CLS_STADIUM = 8, CLS_STADIUM_A = 9, CLS_CROWD = 10,
                 CLS_SKY = 11, CLS_FAR = 12, CLS_CROWD_CAP = 13, CLS_COUNT = 14 };
struct DrawRec {
    uint16_t vsId = 0, fsId = 0;
    uint8_t prim = PRIM_TRIANGLES;
    uint8_t texMask = 0; // which of tex[] the fragment shader samples
    uint32_t layoutMask = 0;
    uint32_t vbuf = 0;      // 0: vertices / indices are in the frame's arenas; else a static buffer (BufUpload)
    // Where index value 0 of this draw is: bytes into the static vertex buffer, or into the
    // draw's chunk of CmdBuf::vtx (chunk start = CmdBuf::vtxChunks[vchunk], or 0 when the
    // frame has no chunk list). With absolute indices (the default, see "vertex addressing"
    // in vgx_draw.cpp) this is always 0 and the draw's first vertex is number `vtxFirst`;
    // in the old scheme (MSC_VGX_ABSIDX=0) every draw has its own offset and vtxFirst is 0.
    uint32_t vtxOffset = 0;
    uint32_t vtxCount = 0;
    uint32_t idxOffset = 0; // elements into CmdBuf::idx (or the static index buffer)
    uint32_t idxCount = 0;
    uint32_t vtxFirst = 0;  // index value of the draw's first vertex (what was added to its 0-based indices)
    uint16_t vchunk = 0;    // chunk of CmdBuf::vtx (dynamic draws)
    uint32_t ublock[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
    RenderState rs;
    TexBind tex[8];
    uint32_t skinBlock = kNoBlock; // W5 SKIN: offset of the bone matrices in CmdBuf::uni (kSkinBlockFloats), kNoBlock: not skinned on the GPU
    uint8_t cls = 0;               // W12 FG: the draw class (EXT_LAYER, CLS_*): from CLS_BG_FIRST background, else foreground; 0 unknown
    uint8_t layer = 0;             // W12 FG: the game's layer view (eCLV) it was drawn in (diagnostics)
    uint8_t fx = 0;                // FX: an additive effect sprite draw (EXT_FX), which the back end may draw at a lower resolution
};

// ----------------------------------------------------------------- shaders
// Generated by the front end (vgx_shadergen.cpp), compiled by the back end.
// Immutable once created; ids are dense and never reused.
struct FsKey;
struct VsKey;
struct ShaderSource {
    bool fragment = false;
    uint16_t id = 0;
    uint64_t keyHash = 0; // stable hash of the structural key (for an on-disk binary cache)
    uint8_t texMask = 0;  // fragment: samplers used
    uint32_t attrMask = 0; // vertex: bit per AT_* location used
    bool usesPalette = false, usesLights = false;
    uint8_t numTexGens = 0;
    // Vertex: sizes (in vec4) of the uniform arrays the text declares; 0 = not declared.
    // The back end uploads at most this much (and at most what the GL reports as active).
    uint8_t nMv = 0, nTm = 0, nPtm = 0, nPal = 0, nLt = 0;
    uint8_t nBone = 0; // W5 SKIN: u_bone (vec4), 0 = not a GPU-skinning shader
    uint8_t nTev = 28; // fragment: size of u_tev
    uint8_t zfVec = 0; // fragment: not 0: index of the Z-freeze plane in u_tev (the back end scales it to the render target)
    // W12 SHADER census (MSC_VGX_SHCENSUS, diagnostics): facts about the key the text came from. Not part of the text.
    uint8_t cAlphaOn = 0, cAlphaTest = 0, cFog = 0, cStages = 0, cProjMask = 0, cNop = 0, cNopHalf = 0, cConstAlpha = 0, cConstVal = 0, cTexAlphaMap = 0xFF, cTexZeroMap = 0xFF;
    uint8_t cProjZ1 = 0;    // vertex: of those, the texgens whose source z is the constant 1 (a texture source, or no third input)
    std::shared_ptr<const VsKey> vkey; // vertex: the key the text came from (W12 FILL: the texgen and position forms of a draw are looked at per draw)
    std::shared_ptr<const FsKey> fkey; // fragment: the key the text came from (W12 OVERDRAW: AlphaZeroDyn looks at it per draw)
    uint8_t cZero = 0;      // W12 OVERDRAW: ZeroFacts (fragment) / VsZeroFacts (vertex): the output is zero where colour 0 is (vgx_shadergen.h)
    uint8_t cProjPlain = 0; // vertex: texgens that are projective with a plain texture matrix (no per-vertex row, no post matrix, a transformed source)
    std::string text;
};

// ---------------------------------------------------------------- commands
enum : uint8_t {
    TF_RGBA8 = 0, // 4 bytes per texel, R first, top row first
    TF_DXT1 = 1,  // S3TC DXT1 blocks (8 bytes each, row-major), as GL_COMPRESSED_RGBA_S3TC_DXT1_EXT takes them
    TF_LA88 = 2,  // 2 bytes per texel, luminance then alpha, sampling as (L,L,L,A); no mipmaps
    TF_DXT5 = 3,  // S3TC DXT5 blocks (16 bytes each, row-major): the HD replacements (vgx_hdtex.cpp), and CMPR with transparent texels (MSC_VGX_CMPRA)
    TF_RGB565 = 4,   // 1.3 (MSC_VGX_TEX16): 2 bytes per texel, little-endian 16-bit words as GL_UNSIGNED_SHORT_5_6_5 (red in the top bits)
    TF_RGBA5551 = 5, // the same, GL_UNSIGNED_SHORT_5_5_5_1; never with genMips (vitaGL has no transfer format for its mips)
    TF_RGBA4444 = 6, // the same, GL_UNSIGNED_SHORT_4_4_4_4
    TF_I8 = 7,       // 1.3 (MSC_VGX_LA88=2, console only): 1 byte per texel, sampling as (I,I,I,I): GX I4 and I8, whose luminance and alpha are the same number; no mipmaps
};
struct TexUpload {
    uint32_t texId = 0;
    uint16_t width = 0, height = 0;
    uint8_t format = TF_RGBA8;
    // Level 0 first. Unless backend::Caps::mipClamp, a mipmapped texture carries the
    // full chain down to 1x1 (GLES2 has no GL_TEXTURE_MAX_LEVEL); with mipClamp only
    // the levels the game authored. `authored` says how many of `levels` those are.
    // genMips: only level 0 is given and the back end is to generate the rest
    // (backend::Caps::mipGen; RGBA8 only).
    uint8_t authored = 1;
    bool genMips = false;
    bool hdArrive = false;   // MSC_HDTEX_FADE: this upload is the HD replacement of a texture that was first drawn with the original (the back end eases the mip bias in)
    std::vector<std::vector<uint8_t>> levels;
};
// Geometry that outlives the frame (a cached display list): created or
// replaced by CMD_BUF_UPLOAD, referenced by DrawRec::vbuf, released by
// CMD_BUF_FREE. Ids are small and reused.
struct BufUpload {
    uint32_t bufId = 0;
    std::vector<uint8_t> vtx;
    std::vector<uint16_t> idx;
    bool merged = false; // a combined buffer of palette / skin runs (vgx_draw.cpp PmClose): player geometry, never measured by the crowd trim (no CPU copy for its LRU)
};
struct EfbRect { int16_t x = 0, y = 0, w = 0, h = 0; };
struct ClearSpec {
    bool enable = false;
    bool color = true, alpha = true, depth = true;
    float rgba[4] = {0, 0, 0, 1};
    float z = 1.0f;
};
// How the EFB colour is converted when copied into a texture:
// out = M * vec4(rgb, a) + add, rows in mat[].
struct CopyTexCmd {
    uint32_t texId = 0;
    EfbRect src;
    uint16_t dstW = 0, dstH = 0; // in guest texels (the back end may scale)
    bool depth = false;          // copy the depth buffer instead of colour
    bool skip = false;           // only the clear is needed (and, with fromPass, the end of the pass): nothing samples this copy before the
                                 // texture is copied to again, or the copy itself is made later from an atlas cell
    bool fromPass = false;       // the source is the offscreen pass that this copy ends (see PassCmd), not the EFB
    uint8_t fmt = 0xFF;          // the GX texture format the game asked for (GXSetTexCopyDst): 0x04 is RGB565 (a census in MSC_VGX_PASSES; nothing reads it yet)
    bool fill = false;           // the source rectangle is known to hold the constant colour fillRgba: no read of the EFB is needed
    bool still = false;          // MSC_MENU_STILL, MSC_PAUSE_STILL: the game wants to hear what became of this copy (VgxStillArm)
    float fillRgba[4] = {};
    bool clearElided = false;    // clear.enable was switched off by the front end: the rectangle already held these values
    // Not 0: the source is rectangle `src` of this texture (srcTexW x srcTexH, same units as
    // `src`), not the EFB: a picture of a copy atlas (PassCmd::atlasTex). Such a copy ends
    // no pass and has no clear.
    uint32_t srcTex = 0;
    uint16_t srcTexW = 0, srcTexH = 0;
    // Not 0: the destination is the rectangle (dstX, dstY, dstW, dstH) of texture texId,
    // which is dstTexW x dstTexH (a cell of a copy atlas), not a texture of dstW x dstH.
    uint16_t dstTexW = 0, dstTexH = 0;
    int16_t dstX = 0, dstY = 0;
    float mat[4][4] = {};
    float add[4] = {};
    ClearSpec clear;
};
struct CopyDispCmd {
    EfbRect src;
    ClearSpec clear;
    bool clearElided = false; // as in CopyTexCmd
};
// An offscreen pass, found by the front end (IsolatePasses in vgx_draw.cpp).
// The draws between CMD_PASS_BEGIN and the next CMD_COPY_TEX (which has
// `fromPass` set and ends the pass) only touch `rect` of the EFB; every plane
// of that rectangle which they or the copy read held the constant value given
// here when they began; and nothing ever looks at what they leave in the EFB.
// A back end may therefore draw them into a separate target of the
// rectangle's size that starts cleared to these values, take the copy from
// that target, and leave the EFB alone (the copy's own clear still applies to
// the EFB rectangle). On a tile-based GPU that turns "store the EFB, copy,
// reload the EFB" into one small scene that needs no previous contents.
struct PassCmd {
    EfbRect rect;
    float rgba[4] = {0, 0, 0, 0};
    float z = 1.0f;
    uint8_t planes = 7; // 1 colour, 2 alpha, 4 depth: the planes the pass uses at all (the others may hold anything)
    uint8_t known = 7;  // of those: the planes whose value at the start matters (and is given above)
    // Not 0: the copy that ends the pass is 1:1 with no conversion into this
    // texture, and no draw of the pass samples it, so the pass may be drawn
    // straight into the texture and the copy needs no work at all.
    uint32_t directTex = 0;
    // MSC_VGX_PASSDOWN (with directTex, no atlas): the copy that ends the pass is a 2:1 reduction (src = this rect, destination half the size per side) and the pass is a
    // filter step (one quad per draw, samples only pictures of this frame's copies). The back end draws it at half the scale straight into the destination texture, which
    // then has the size the copy would have made: the copy and its scene are gone. The 2x2 mean of the copy is gone too: the pass's taps are read at the destination's pixel centres.
    uint8_t down = 0;   // 1: the copy has no conversion; 2: the copy broadcasts alpha into the colour planes (GX format A8: the glow chain): the back end then lets the pass write colour too, which its four-texture TEV makes equal to the alpha
    // Not 0 (only with directTex): the picture does not get a texture of its own. It is drawn
    // into a cell of this atlas texture (atlasW x atlasH; the cell's picture starts at
    // atlasX, atlasY and has the pass rectangle's size; all in EFB pixels), which is cleared
    // to the pass's values, together with a border of atlasGutter pixels around it. The front
    // end has pointed every draw that samples the picture at the atlas (AtlasPasses in
    // vgx_draw.cpp). Many small pictures then share one render target, and passes that follow
    // each other into the same atlas are one scene for the GPU instead of one each.
    // atlasGutter: the border to clear with the cell (0: it holds the clear colour already).
    // Without directTex the cell is only the pass's scratch: the copy that ends the pass has
    // `skip` set and a later copy (CopyTexCmd::srcTex) reads the cell.
    uint32_t atlasTex = 0;
    uint16_t atlasW = 0, atlasH = 0, atlasX = 0, atlasY = 0;
    uint8_t atlasGutter = 0;
};
struct SetTargetCmd {
    uint16_t efbW = 640, efbH = 528;
};

enum CmdType : uint8_t { CMD_DRAW, CMD_TEX_UPLOAD, CMD_TEX_FREE, CMD_SHADER, CMD_COPY_TEX, CMD_COPY_DISP, CMD_PRESENT, CMD_SET_TARGET,
                         CMD_BUF_UPLOAD, CMD_BUF_FREE, CMD_PASS_BEGIN,
                         CMD_PROGRAM, // arg = vsId << 16 | fsId: this pair will be drawn with; link it now (shader warm-up list)
                         CMD_VARIANT, // arg = index into variants (shader warm-up list)
                         CMD_TYPE_COUNT };
struct Cmd {
    CmdType type;
    uint32_t arg; // index into the matching vector below (texId for CMD_TEX_FREE, bufId for CMD_BUF_FREE)
};

// For the frame arenas: resize() leaves new elements uninitialised (a std::vector<uint8_t>
// zero-fills them, and the vertex arena is grown and then written by the decoder for every
// draw: half a megabyte of memset per frame for nothing).
// 1.3 (MSC_VGX_ARENA_SOFT, vgx_arena.h): a block the calling thread got beforehand (a request that may fail without ending the program) is
// what the next allocate() of exactly that size returns; the vector's reserve() then copies into it as always.
struct NoInitPending { void* block = nullptr; size_t bytes = 0; };
inline NoInitPending& NoInitPendingBlock() { static thread_local NoInitPending p; return p; }
// 1.3 (MSC_VGX_ARENA_SOFT): every other frame vector (indices, uniforms, mesh uploads) that grows when the C heap has no block is not ended at
// once: the player hands memory back frame by frame (the other buffer's slot, record chunks), so the request is asked again every 2 ms for up to
// ~1.5 s, with one line in the log, and only then ends the program through operator new (whose FATAL line names the size). Only where the platform
// offers NpHostTryNew (the console build); elsewhere, and with the switch at 0, it is ::operator new as before.
extern "C" void* NpHostTryNew(size_t) __attribute__((weak));
inline void* NoInitHeapAlloc(size_t bytes) {
    static const bool soft = EnvInt("MSC_VGX_ARENA_SOFT", 1) != 0;
    if (!soft || !NpHostTryNew) return ::operator new(bytes);
    for (int attempt = 0; attempt < 750; ++attempt) {
        if (void* p = NpHostTryNew(bytes)) return p;
        if (attempt == 0) std::fprintf(stderr, "[vgx] frame buffer growth of %lu KiB: the C heap has no room; asking again for up to 1.5 s\n", static_cast<unsigned long>(bytes >> 10));
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, nullptr);
    }
    return ::operator new(bytes);
}
// nxboot25 R2-9 (MSC_VGX_VTX_BLOCK=1, default 0): a vertex-arena block of 4 MiB or more is taken from the platform's own memory block (NpHostBlockNew: sceKernelAllocMemBlock USER_RW)
// instead of the C heap, and given back by NpHostBlockFree (true when the pointer was one of its blocks). Console memory is not made: the 2 x 16 MiB move from the 96 MiB C heap to the system's
// free main memory (33-38 MiB in the 6027/6033/6124 logs). The platform hooks are weak: without them (host, an older platform) nothing changes. A refused block falls back to the heap.
extern "C" void* NpHostBlockNew(size_t) __attribute__((weak));
extern "C" int NpHostBlockFree(void*) __attribute__((weak));
inline bool VtxBlockOn() {
    static int on = -1;   // (a plain word, as NoInitHeapAlloc: read once)
    if (on < 0) { const char* e = std::getenv("MSC_VGX_VTX_BLOCK"); on = (e && *e && *e != '0') ? 1 : 0; }
    return on != 0;
}
constexpr size_t kVtxBlockMin = 4u << 20;
template <typename T> struct NoInitAlloc {
    using value_type = T;
    NoInitAlloc() = default;
    template <typename U> NoInitAlloc(const NoInitAlloc<U>&) {}
    T* allocate(size_t n) {
        NoInitPending& p = NoInitPendingBlock();
        if (p.block && p.bytes == n * sizeof(T)) { T* r = static_cast<T*>(p.block); p.block = nullptr; p.bytes = 0; return r; }
        if (sizeof(T) == 1 && n >= kVtxBlockMin && NpHostBlockNew && VtxBlockOn())
            if (void* b = NpHostBlockNew(n)) return static_cast<T*>(b);
        return static_cast<T*>(NoInitHeapAlloc(n * sizeof(T)));
    }
    void deallocate(T* p, size_t n) {
        if (sizeof(T) == 1 && n >= kVtxBlockMin && NpHostBlockFree && NpHostBlockFree(p)) return;
        ::operator delete(p);
    }
    template <typename U> void construct(U* p) { ::new (static_cast<void*>(p)) U; }
    template <typename U, typename A0, typename... A> void construct(U* p, A0&& a0, A&&... a) {
        ::new (static_cast<void*>(p)) U(std::forward<A0>(a0), std::forward<A>(a)...);
    }
    template <typename U> bool operator==(const NoInitAlloc<U>&) const { return true; }
    template <typename U> bool operator!=(const NoInitAlloc<U>&) const { return false; }
};

// W12 FILL (MSC_VGX_SHQUAD): the box of the stored positions of a static draw that writes alpha (min xyz, max xyz), for the back end's alpha ledger.
struct StaticBox {
    uint32_t draw;
    float b[6];
};
struct CmdBuf {
    std::vector<Cmd> cmds;
    std::vector<DrawRec> draws;
    std::vector<TexUpload> uploads;
    std::vector<const ShaderSource*> shaders;
    std::vector<CopyTexCmd> copyTex;
    std::vector<CopyDispCmd> copyDisp;
    std::vector<SetTargetCmd> targets;
    std::vector<BufUpload> bufUploads;
    std::vector<PassCmd> passes;
    std::vector<uint8_t, NoInitAlloc<uint8_t>> vtx;
    std::vector<uint16_t, NoInitAlloc<uint16_t>> idx;
    std::vector<float, NoInitAlloc<float>> uni; // (GXD: new elements are not cleared, PushBlock writes all of them; cheaper than insert(): 500 per frame)
    // Start (byte offset in vtx) of each chunk of the vertex arena, ascending, first 0.
    // A chunk goes into a vertex buffer of its own, and the dynamic draws of a chunk
    // address their vertices from the chunk's start (DrawRec::vchunk / vtxFirst).
    // Empty: one chunk, draws carry byte offsets (the old scheme).
    std::vector<uint32_t> vtxChunks;
    std::vector<VariantCmd> variants;
    std::vector<StaticBox> staticBoxes; // ascending by draw
    uint64_t frame = 0; // presented-frame number this buffer belongs to (diagnostics)
    uint64_t latencyTag = 0; // immutable identity delivered before decoder/backend execution
    bool hidden = false;     // EXT_HIDDEN (MSC_DISCARD_REHEARSE): executed, never presented
    uint8_t scene = 0;       // W12 OVERDRAW: the kind of scene (EXT_SCENE): 0 other, 1 match play, 2 replay, 3 cut-scene
    // 1.3 MSC_BG30 (EXT_BG30, vgx_bg30.h): 0 an ordinary picture; 1 a full picture whose background is kept (captured); 2 a light picture (no
    // background draws: the kept background is re-projected to this camera); 3 a full picture that captures and re-uses its own background (proof).
    uint8_t bg30 = 0;
    float bg30View[16] = {}; // the game's view matrix of this picture (row vectors: eye = world . V), for kinds 1-3

    bool empty() const { return cmds.empty(); }
    void clear() {
        cmds.clear(); draws.clear(); uploads.clear(); shaders.clear(); copyTex.clear();
        copyDisp.clear(); targets.clear(); bufUploads.clear(); passes.clear(); vtx.clear(); idx.clear(); uni.clear();
        vtxChunks.clear(); variants.clear(); staticBoxes.clear();
        latencyTag = 0;
        scene = 0;
        bg30 = 0;
        hidden = false;
    }
};

// 1.3 (wave 8) per-match switch line: what the front end's switches have done so far (cumulative, never reset by a diagnostic); filled by vgx_draw.cpp (GetDrawSwitchCounts), read by the back end
struct DrawSwitchCounts { bool cullOn = false, palMergeOn = false, warmOrderOn = false; uint64_t warmMarked = 0, warmFront = 0, culled = 0, culledTris = 0, palRuns = 0, palJoined = 0; };
// ---------------------------------------------------------------- back end
// Implemented by vgx_backend_gl.cpp; called on the thread that owns the GL
// context (see vgx_host_egl.cpp).
namespace backend {
bool Init(int renderWidth, int renderHeight); // 0,0 = follow the EFB size
void Execute(const CmdBuf& buf);
// Frame interpolation (vgx_interp.cpp) executes one command buffer twice: first as the
// in-between picture, with other uniform blocks for some draws, then as itself.
//   draws    not null: use these records (as many, in the same order, as buf.draws; their
//            uniform blocks are offsets into the same buf.uni) instead of buf.draws;
//   vtx      not null: these bytes (buf.vtx.size() of them, same chunks) are the frame's
//            vertex arena for this execution instead of buf.vtx;
//   replay   the buffer has been executed before in this state: nothing that creates,
//            replaces or frees a texture, buffer, shader or render target is done again,
//            and the frame's vertex and index arenas are not uploaded again ...
//   restream ... except the vertex arena when this is set (the execution before had other bytes).
// ExecuteEx(buf, {}) is Execute(buf).
struct ExecOpts {
    const DrawRec* draws = nullptr;
    const uint8_t* vtx = nullptr;
    bool replay = false;
    bool restream = false;
    bool hidden = false;   // executed, but the display copy is not presented (a frame the presenter's clock jumped over)
};
void ExecuteEx(const CmdBuf& buf, const ExecOpts& opts);
// Present hook: the host layer decides what "present" means (swap, PNG dump).
// Returns the last display copy as tightly packed RGBA8, top row first.
const std::vector<uint8_t>& LastDisplayCopy(int& width, int& height);
void RequestCapture(bool enable); // read back the next display copy
// Optional present hook for hosts that show the picture themselves (instead
// of asking for a capture and intercepting the read-back). Called on the back
// end's thread at every display copy, before the copy's clear, with the EFB
// framebuffer object still bound: its name, and the copy rectangle in
// render-target pixels. The EFB is stored top row first: GL row 0 is the top
// row of the picture.
using PresentHook = void (*)(unsigned efbFramebuffer, int x, int y, int width, int height);
void SetPresentHook(PresentHook hook);
// MSC_VGX_HUDNATIVE (vgx_hudplan.h): a present in two steps. The first does all of a present except the swap and leaves the display bound with its scene open; it gives the rectangle
// the picture took on the display (x, width, height) and returns false if it showed nothing (the frame then goes on through the ordinary hook). The back end draws on the display, then calls
// the second, which swaps.
using PresentSplitHook = bool (*)(unsigned efbFramebuffer, int x, int y, int width, int height, int* displayX, int* displayW, int* displayH);
using PresentFinishHook = void (*)();
void SetPresentSplit(PresentSplitHook split, PresentFinishHook finish);
// 1.3 MSC_VITA_SHAPE=1: the dynamic-resolution sizes keep their pixels but take the shape of the display rectangle dispW x dispH (0, 0: the game's own shape, the default).
// A fixed size (MSC_VITA_RENDER) is left alone. The host calls this before the first target is made.
void SetRenderShape(int dispW, int dispH);
// W5 RT: hints for the scene that the next draw or clear begins on `framebuffer` (MSC_VGX_RT=1; see "W5 RT" in vgx_backend_gl.cpp): the
// Vita host passes them to vitaGL (vglRtSceneHint). kHintNoLoad: nothing in the scene reads the depth that was there before it began;
// kHintNoStore: nothing reads its depth after it.
enum : unsigned { kHintNoLoad = 1u, kHintNoStore = 2u };
using SceneHintHook = void (*)(unsigned framebuffer, unsigned hints);
void SetSceneHintHook(SceneHintHook hook);
// DYNRES (vgx_backend_gl.cpp "dynamic resolution"): the host's GPU time of the newest finished frame. Returns false when there is none; seq grows
// by one per finished frame (a new value), ms is what the GPU was busy for it.
typedef bool (*GpuTimeFn)(uint32_t* seq, float* ms);
void SetGpuTimeSource(GpuTimeFn fn);
// heldgpu24: the same frame's first-submission-to-last-completion time (ms, the display scene's wait for a buffer taken out as GpuTimeFn's busy time has it), noted by the host just before GpuTimeFn returns it.
void NoteGpuSpanMs(float ms);
float RenderScale(); // the scene's render scale in force (1: the game's own size)
int LiveScene(); // 1.3: the scene class (EXT_SCENE) of the buffer last executed: 1 is live match play
// W5 RT: a cheap microsecond clock for the draw timers (MSC_VGX_RTTIME=1; default: std::chrono::steady_clock).
using MicroClock = uint32_t (*)();
void SetMicroClock(MicroClock clock);
uint64_t ExecutingFrame(); // W5 RT: CmdBuf::frame of the buffer being executed (the present hook's frame)
uint64_t ExecutingLatencyTag();
void GetDrawTimers(uint64_t out[4]); // MSC_VGX_RTTIME=1, cumulative: draws that began a scene, their microseconds, sampled other draws (every 32nd), theirs
// Picture scaling at present (LOOK, wave 5). A host that shows the picture itself can scale the
// display copy to the screen with a shader of the back end instead of a framebuffer blit:
//   PIC_LINEAR  plain bilinear (what the blit does, through a shader: for comparisons)
//   PIC_SHARP   "sharp bilinear": each source texel keeps a flat core, only the seam between two
//               texels is blended over about one screen pixel (the picture is not blurred by a
//               non-integer scale; one texture fetch per pixel)
//   PIC_UNSHARP sharp bilinear plus an unsharp mask (four more fetches; amount `sharpen`, 0.2 to 0.6)
//   PIC_SMOOTH  plain bilinear plus the same unsharp mask (five fetches): edges stay smooth, the
//               softness of the scale is taken back (the Vita host's default, amount 0.5)
//   PIC_COMBINED 1.3: SMOOTH's look from three fetches at coordinates the vertex shader computed, no dependent read, one gain for sharpening and edges (see PictureFragmentSource)
//   PIC_CAS     1.3: contrast-adaptive sharpening from five fetches at coordinates the vertex shader computed, no dependent read (see PictureFragmentSource)
//   PIC_FAST    1.3: bilinear plus an unsharp mask from two diagonal half-texel fetches (three fetches, all at coordinates the vertex shader
//               computed, no clamp and no edge smoothing). The rectangle is inset by one texel per side so that no fetch leaves the display copy.
// Hosts read MSC_VITA_UPSCALE=off|linear|sharp|sharp+|smooth|fast (PictureModeFromString; off = 0 = no shader:
// the Vita host's glBlitFramebuffer) and MSC_VITA_SHARPEN=<amount>.
enum PictureMode { PIC_BLIT = 0, PIC_LINEAR = 1, PIC_SHARP = 2, PIC_UNSHARP = 3, PIC_SMOOTH = 4, PIC_FAST = 6, PIC_CAS = 7, PIC_COMBINED = 8 /* (5 is the back end's SMOOTH without edge smoothing) */ };
// 1.3 MSC_VGX_LODBIAS_RUNG: the rectangle the picture takes on the display (960x544, or the 4:3 box's), told at each present; the texture bias follows the picture's size against it.
void SetPictureSize(int w, int h);
// 1.3 MSC_PIC_PREWARM: build the picture program(s) a present in `mode` can need (SMOOTH: with and without its edge smoothing) now, at start-up, not at the first present that uses them
// (a step to a smaller picture, which was a compile and a link in play). Returns the number of programs ready.
int PrewarmPicturePrograms(int mode);
int PictureModeFromString(const char* text);   // -1: not a mode name
// Draws the display copy rectangle (x, y, w, h in render-target pixels, as in PresentHook) of the EFB into
// the rectangle (dx, dy, dw, dh) of framebuffer object `fbo` (0: the display). flipY: the picture's top
// row goes to the top of an upright GL framebuffer (the display) instead of GL row 0 (a read-back).
// To be called from the present hook (the EFB's state is then the back end's own).
bool DrawPicture(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, int mode, float sharpen, bool flipY);   // false: the picture shader did not build (state untouched)
// Host harness (MSC_VGX_SCREEN=WxH): the captured display copy (RequestCapture) is the picture scaled to
// w x h by DrawPicture with `mode` (PIC_LINEAR when 0), as the console would show it. 0 x 0: off.
void SetScreenCapture(int w, int h, int mode, float sharpen);
// More pictures of the same display copy at the same screen size, one per mode (comparisons in one run): after the main
// capture, LastScreenVariant(i) is the picture made with modes[i] (tightly packed RGBA8, top row first).
void SetScreenCaptureExtra(const int* modes, int count);
const std::vector<uint8_t>& LastScreenVariant(int index, int& width, int& height);
// Host-harness fast-forward: while set, draw commands are dropped (uploads,
// shader compiles, copies and clears still run).
void SetSkipDraws(bool skip);
uint32_t DbgExecCmd(); // MSC_VGX_ASYNCLOG: (command type << 24 | index) of the command the back end is executing, 0xFFFFFFFF between buffers
bool CaptureRequested();   // the two flags as last set (the presenter keeps them with each frame)
bool SkipDrawsRequested();
// Progress display while shaders are being compiled. On the console a shader takes
// 0.3 to 1 s the first time (GLSL -> Cg -> the system's compiler); a frame that needs
// several (the first frame of a start with the warm-up list: all of them) would leave
// the last picture frozen for that long. The host names a framebuffer the back end may
// draw a bar into in the middle of a frame (0 = the display itself), its size, and a
// function that shows it (swaps). After 150 ms of shader work within one command buffer
// the back end draws the bar and calls `show` after every further shader / program /
// variant, at most every 40 ms. `show` may do anything with the context; the back end
// trusts none of its cached state afterwards. Without this call nothing is drawn.
//   MSC_VGX_PROGRESS=0            never draw the bar
//   MSC_VGX_SIM_COMPILE_MS=<n>    (testing, any GL) every shader compile is charged n ms on
//                                 top of what it took, as the console's compiler would cost;
//   MSC_VGX_SIM_CACHE=<dir>       ... unless <dir> holds a file named after the hash of the
//                                 shader text, which is created by the first compile: the
//                                 behaviour of vitaGL's on-disk shader cache;
//   MSC_VGX_SIM_COMPILE_SLEEP=1   ... and the thread really sleeps that long.
using ProgressShowHook = void (*)(uint32_t done, uint32_t total);
void SetProgressDisplay(unsigned framebuffer, int width, int height, ProgressShowHook show);
struct Stats {
    uint32_t programs = 0, vertexShaders = 0, fragmentShaders = 0;
    uint32_t programsUsedThisFrame = 0, drawCallsThisFrame = 0, textures = 0;
    uint64_t textureBytes = 0;
    // Shader work since start-up: compiles asked of the GL, of those answered from the simulated
    // cache (MSC_VGX_SIM_CACHE; the real one is inside vitaGL and cannot be seen from here),
    // programs linked, time spent in all of it (with the simulated charge), bars shown.
    uint32_t shaderCompiles = 0, shaderSimCacheHits = 0, programLinks = 0, progressFrames = 0;
    double shaderWorkMs = 0;
};
Stats GetStats(); // per-frame members describe the last executed buffer
// What the back end wants from the front end. Fixed at build time / by the
// environment (never asked of the GL), so any thread may read it at any time.
struct Caps {
    bool dxt1 = false;     // takes TF_DXT1 uploads (MSC_VGX_DXT1, default 1)
    bool mipGen = false;   // RGBA8 mip chains: send level 0 only, the back end generates the rest (MSC_VGX_MIPGEN)
    bool mipClamp = false; // send only the authored levels of a mip chain (MSC_VGX_MIPCLAMP)
    bool la88 = false;     // non-mipmapped I/IA uploads (MSC_VGX_LA88, default 0)
    bool laMips = false;   // tile24: mipmapped I/IA uploads too, as level 0 in LA88 / one byte with a generated chain (MSC_VGX_LA88=3; needs mipGen, the console)
    bool i8 = false;       // 1.3: non-mipmapped I4 and I8 as one byte a texel, TF_I8 (MSC_VGX_LA88=2; the console's back end only)
    bool i8churn = false;  // walk15 (MSC_VGX_I8CHURN): an I8 texture uploaded again and again (movie planes) as one byte a texel, TF_I8; every other I8 stays as it is
};
const Caps& GetCaps();
}

// The host layer's submission hook: runs `buf` on the render thread and
// returns when it has been executed (synchronous for now).
void SubmitToBackend(CmdBuf& buf);

namespace host {
// Makes whatever context the back end needs current on the calling thread
// (the render thread). vgx_host_egl.cpp for the GL back end; the null back
// end has nothing to create.
bool CreateContext();
}

} // namespace vgx
