#include "vgx_perf_diag.h"
// OpenGL ES 2.0 back end. Executes CmdBufs; knows nothing about GX registers
// or guest memory. Only ES 2.0 core calls are used (plus OES_depth24 /
// OES_depth_texture / EXT_texture_compression_s3tc when the driver offers
// them) so the same code sits on vitaGL on the console.
//
// The EFB is an FBO whose rows are stored top row first (the projection flips
// Y), so EFB copies come out in texture row order and no flip is needed when
// a copy is sampled or read back.
//
// What vitaGL (commit cdbba42, read 4-5 Oct) makes of GL calls decides how
// this file is written; the rules, each with the place in vitaGL it comes from:
//  - Every GL state call is issued only when the value changes (GlState below).
//    glEnable(GL_SCISSOR_TEST) and glScissor mark the scissor dirty even when
//    nothing changes (misc.c, tests.c glScissor), and a dirty scissor costs two
//    extra full-target draws before the next draw (tests.c update_scissor_test).
//    glUseProgram marks both uniform buffers dirty, and a dirty buffer is
//    copied whole into a 2 MiB ring before the next draw (custom_shaders.c
//    upload_uniforms; utils/gxm_utils.c).
//  - Vertex attribute pointers never carry a per-draw byte offset: sceGxm's
//    attribute offset has 16 bits and vitaGL stores the whole pointer in it
//    (custom_shaders.c handle_packed_vbo_attrib), and every distinct offset is
//    a newly patched vertex program (utils/gxm_utils.c vglCreateVertexProgram).
//    Draws index their vertices from the start of a buffer instead (see
//    DrawRec::vtxFirst and CmdBuf::vtxChunks in vgx_cmd.h).
//  - Every attribute location has had a pointer set with a buffer bound (Init),
//    and the copy quad comes from a buffer: an attribute without a buffer makes
//    vitaGL scan the index list of every draw for its highest index.
//  - The constant an attribute location supplies while its array is off
//    (glVertexAttrib4f) is given again at least once per frame: vitaGL keeps
//    the four floats in a 256 KiB ring (buffers.c DISABLED_ATTRIBS_POOL_SIZE,
//    custom_shaders.c reserve_attrib_pool) that later glVertexAttrib* calls
//    overwrite, so a constant set once and used for minutes would turn into
//    some other location's constant after about 16 000 calls.
//  - Uniform locations are compared with -1 only (a vitaGL location is the
//    negated address of its record and may have either sign), and array
//    uploads never exceed the size the GL reports for the array.
//  - glClear ignores glColorMask (misc.c glClear: the clear program is patched
//    once without a mask), so a clear of colour without alpha or the reverse is
//    drawn as a quad (MSC_VGX_QUADCLEAR, default 1 everywhere).
//  - A framebuffer object's render target is re-created when the colour
//    attachment's size changes and is sized for 8 scenes per frame (shared.h
//    _glFramebufferTexture2D, gxm.c): see the pool below.
//  - glTexImage2D with level > 0 ignores its data and regenerates the level
//    from level 0 (textures.c -> gpu_alloc_mipmaps); glCompressedTexImage2D
//    stores each level as given but cannot take levels smaller than 4x4
//    (utils/gpu_utils.c gpu_alloc_compressed_texture: a transfer of w/4 x h/4
//    blocks). See backend::Caps.
//
// MSC_VGX_GLSTATS=<csv> writes, per executed frame, how many GL calls of each
// kind were made (VGX_GL_COUNTERS below) plus what they cost on vitaGL as far
// as that can be told from here (scenes, scissor updates, bytes copied into
// the uniform ring).
#include <atomic>
#include "vgx_cmd.h"
namespace vgx::rec { double TakePlayerIdleMs(); bool TakePipeWaitMs(double* playerSlotWaitMs, double* backEndIdleMs); }
extern "C" int NpFpsDisplayInterval(void) __attribute__((weak));
extern "C" int vgx_vram_free_kib(void) __attribute__((weak)); // the host's free GPU memory in KiB (Vita: vitaGL's VRAM + RAM pools), -1 unknown
extern "C" int vgx_tex_has_storage(void) __attribute__((weak)); // the texture bound on unit 0 got its memory (Vita: vitaGL gives no error)
#ifdef __VGX_VITA__
extern "C" int vglStrikersBindFramebufferDepth(unsigned int fb) __attribute__((weak)); // patched vitaGL: a framebuffer's depth as the bound texture
#endif // the game's frame-rate policy (np/fps.h): vblanks per picture, 0 = none (DYNRES's budget)
#ifdef __VGX_VITA__
extern "C" void vgx_vita_log_glb_stats(void); // vita/vgx_host_vita.cpp (a no-op line when vitaGL is not the GLB build)
#endif
#include "vgx_shadergen.h"
#include "vgx_validate.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <ctime>
#include <unordered_set>
#include <unordered_map>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

namespace vgx::backend {

const Caps& GetCaps() {
    static const Caps caps = [] {
#ifdef __VGX_VITA__
        const long vita = 1;
#else
        const long vita = EnvInt("MSC_VGX_VITA", 0); // 1: behave towards the front end as on the console (for tests on the desktop)
#endif
        Caps c;
        c.dxt1 = EnvInt("MSC_VGX_DXT1", 1) != 0;
        c.mipGen = EnvInt("MSC_VGX_MIPGEN", vita) != 0;
        c.mipClamp = EnvInt("MSC_VGX_MIPCLAMP", vita) != 0;
        c.la88 = EnvInt("MSC_VGX_LA88", 0) != 0;
        return c;
    }();
    return caps;
}

namespace {

// ------------------------------------------------------------ call census
#define VGX_GL_COUNTERS(X)                                                                                                            \
    X(glCalls)        /* every GL call made for the frame */                                                                         \
    X(scenes)         /* render-target changes followed by drawing (sceGxm scenes), not counting the host's present */               \
    X(efbScenes)      /* of those: on the EFB */                                                                                     \
    X(fboBinds) X(fboAttach)                                                                                                         \
    X(clears)         /* glClear */                                                                                                  \
    X(quadClears)     /* clears drawn as a quad (partial colour mask) */                                                             \
    X(copyQuads)      /* EFB / pass -> texture copy draws */                                                                         \
    X(passesScaled)   /* DYNRES: large passes drawn at the reduced pass scale */                                                     \
    X(fxRuns) X(fxDraws) /* FX: runs of additive effect draws drawn at the lower resolution, and their draws */                        \
    X(draws) X(drawIndices)                                                                                                          \
    X(programUses)    /* glUseProgram */                                                                                             \
    X(uniformCalls) X(uniformBytes) /* glUniform*: all blocks */                                                                     \
    X(palCalls) X(palBytes)         /* of those: the matrix palette */                                                               \
    X(ringBytes)      /* vitaGL: bytes of whole uniform buffers copied to its ring (one per draw whose program or uniforms changed) */ \
    X(streamUploads) X(streamBytes) /* glBufferData of the frame's vertex chunks and indices */                                      \
    X(staticUploads) X(staticBytes) /* glBufferData of static buffers */                                                             \
    X(texUploads) X(texUploadBytes) X(texBinds) X(texParams) X(texDeletes) X(mipGens)                                                \
    X(vboBinds) X(attrPointers) X(attrToggles) X(attrConsts)                                                                         \
    X(layoutChanges)  /* draws that had to set attribute pointers (buffer or layout changed) */                                      \
    X(scissorCalls)   /* glScissor + glEnable/glDisable(GL_SCISSOR_TEST) */                                                          \
    X(scissorUpdates) /* vitaGL: scissor mask rebuilt (new scene or scissor call before a draw): 2 extra full-target draws each */   \
    X(viewportCalls) X(stateCalls) /* blend, depth, cull, colour mask, depth range, line width */                                    \
    X(shaderCompiles) X(programLinks) X(readPixels)                                                                                   \
    X(fragSwitches)   /* vitaGL: draws whose program was last drawn with another blend function / colour mask (a shader patcher call) */ \
    X(fragVariants)   /* of those: with a combination never used before (the patcher builds a fragment program: milliseconds) */      \
    X(vtxVariants)    /* draws with a (vertex shader, vertex layout) pair never used before (the patcher builds a vertex program) */   \
    X(warmDraws)      /* empty draws that make vitaGL build a program variant ahead of its first use (CMD_VARIANT) */               \
    X(sceneReuploads) /* draws that gave all their uniform blocks again because they open a scene with the previous draw's program */ \
    X(constAttrDraws) /* draws whose vertex shader reads an attribute the vertex layout does not have (a glVertexAttrib4f constant) */          \
    X(mergeable)      /* W5 GLB: draws that continue the previous draw command exactly (same shaders, uniform blocks, textures, state, buffer) with the next indices */ \
    X(sameStateGap)   /* of the other draws: same state as the previous draw command but not the next indices */                       \
    X(progSwitchOnly) /* draws that switched program but gave no uniform block (the ring copy is the cost) */                              \
    X(pairs) X(pairsSameProg) X(pairsDiffStatic) X(pairsDiffRs) X(pairsDiffTex) X(pairsDiffU0) X(pairsDiffU1) X(pairsDiffU2) X(pairsDiffU3) X(pairsDiffU4) X(pairsDiffU5) /* pair census: consecutive draws, and what differs between those with the same program (MSC_VGX_PAIRDIAG=1 or GLSTATS) */ \
    X(mergedDraws) /* MSC_VGX_GLMERGE=1: draws issued as part of the previous glDrawElements */ \
    X(uniDupCalls) X(uniDupBytes) /* MSC_VGX_UNIDUP=1/2: glUniform calls whose values equal what the program already holds (=2: not made) */ \
    X(rtPassScenes) X(rtCopyScenes) X(rtDispScenes) /* W5 RT: scenes on a pass target (atlas page or pass buffer), on a copy destination, on the host's display (progress bar); the rest are efbScenes */ \
    X(rtClearFirst) X(rtFullDepthFirst) /* scenes whose first operation is a clear; of those a clear of the whole depth buffer */                   \
    X(rtDepthScenes) X(rtEmptyScenes) X(rtReturns) /* scenes with a depth-tested draw; with no draw and no copy quad (clears only); on the target of the scene before the previous one (A B A) */ \
    X(rtNoLoad) X(rtNoStore) X(rtDsKib) X(rtDsSkipKib) /* MSC_VGX_RT=1: scenes given NOLOAD / NOSTORE; depth bytes (KiB) a scene loads + stores on vitaGL with STORE_DEPTH_STENCIL; of those not moved */ \
    X(rtSimClears) X(rtHeldProgress) X(rtDeferredClears) /* MSC_VGX_RTSIM=1 hostile depth clears (host); progress bars held back (a pass or a dead-store EFB scene open); display-copy clears moved to the next buffer */ \
    X(rtFirstDraws) X(rtFirstDrawUs) X(rtSampleDraws) X(rtSampleDrawUs) /* MSC_VGX_RTTIME=1: draws that begin a scene and their time (us), every 32nd other draw and its time */ \
    X(streamSame1) X(streamSame3) X(streamAny1) X(idxBytes) X(idxSame1) /* MSC_VGX_STREAMDIFF=1: bytes of the stream upload (64-byte blocks) equal to the previous upload at the same place, to the one 3 uploads ago (the display buffer the vitaGL slice rotation comes back to), to any block of the previous upload; the index part separately */
struct Counts {
#define X(n) uint32_t n = 0;
    VGX_GL_COUNTERS(X)
#undef X
};
Counts C;
#define G(call) (++C.glCalls, call)

struct Program {
    GLuint id = 0;
    GLint proj = -1, mv = -1, tm = -1, ptm = -1, pal = -1, lt = -1, tev = -1;
    GLint nMv = 0, nTm = 0, nPtm = 0, nPal = 0, nLt = 0, nTev = 0; // elements to upload (vec4)
    uint32_t vsBytes = 0, fsBytes = 0;                           // size of the program's uniform buffers (estimate from the declarations)
    uint32_t applied[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
    float shadow[904];                      // W5 GLB (MSC_VGX_UNIDUP): what was last given to the program, block after block (kShOff)
    bool shadowOk[UB_COUNT] = {};
    uint64_t serial = 0;
    uint64_t usedSerial = 0;               // last frame a draw used it
    uint32_t blendKey = ~0u, layout = ~0u; // of its last draw (see BlendKey)
    uint32_t layoutNeed = 0;               // vertex-layout bits (VL_*) of the attributes the vertex shader declares
    bool ok = false;
    uint8_t failures = 0;
    uint64_t retryAfter = 0;
    // W5 SKIN: the bone matrices of a GPU-skinning shader (u_bone, DrawRec::skinBlock), the block last given, and (MSC_VGX_UNIDUP) its values
    GLint bone = -1, nBone = 0;
    uint32_t appliedSkin = kNoBlock;
    bool skinShadowOk = false;
    std::vector<float> skinShadow;
};
struct Tex {
    GLuint id = 0;
    uint16_t w = 0, h = 0;
    bool mips = false;
    uint8_t wrapS = 0xFF, wrapT = 0xFF, minF = 0xFF, magF = 0xFF;
    bool renderTarget = false;
};

int g_fixedW = 0, g_fixedH = 0; // 0 = follow the EFB size
int g_userFixedW = 0;            // MSC_VITA_RENDER / the host's fixed size (Init): the dynamic resolution (DYNRES, below) then stays off
void DynTargetSize(int efbW, int efbH, int* w, int* h); // DYNRES: the render-target size for an EFB size at the level in force
int g_rtW = 0, g_rtH = 0;
int g_efbW = 640, g_efbH = 528;
GLuint g_efbFbo = 0, g_efbColor = 0, g_efbDepthRb = 0, g_efbDepthTex = 0;
GLuint g_ibo = 0, g_whiteTex = 0, g_quadVbo = 0;
std::vector<GLuint> g_streamVbo; // one per chunk of the frame's vertex arena
bool g_hasDepth24 = false, g_hasDepthTex = false, g_hasS3tc = false, g_hasMaxLevel = false;
bool g_hasDxt5 = false; // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT (1.1: HD character textures; decoded to RGBA8 without it)
bool g_quadClear = true;
GLuint g_copyProg = 0;
GLint g_copyRect = -1, g_copyMat = -1, g_copyAdd = -1;

std::vector<GLuint> g_vs, g_fs;
std::vector<const ShaderSource*> g_vsSrc, g_fsSrc;
std::unordered_map<uint32_t, Program> g_programs;
std::vector<Tex> g_tex;
struct StaticBuf { GLuint vbo = 0, ibo = 0; };
std::vector<StaticBuf> g_sbuf; // by BufUpload::bufId
uint64_t g_serial = 0;
Stats g_stats;
uint32_t g_progsThisFrame = 0;
uint32_t g_evictions = 0, g_evicted = 0; // W5 SEED: EvictPrograms passes that deleted programs, and programs deleted in all

std::unique_ptr<validate::Session> g_validation;
bool g_validateFrame = false;
struct ValidationFrame {
    uint64_t draws = 0, badDraws = 0, uncheckedDraws = 0, references = 0, vertexBytes = 0, indexBytes = 0, uniformBytes = 0;
    uint64_t hash = validate::kHashStart;
    unsigned extraLines = 0;
};
ValidationFrame g_validationFrame;

bool g_capture = false;
PresentHook g_presentHook = nullptr;
bool g_hidden = false; // ExecOpts::hidden of the buffer being executed
bool g_skipDraws = false; // harness fast-forward: geometry of frames nobody looks at is not rasterised
std::vector<uint8_t> g_captured;
int g_capW = 0, g_capH = 0;

// Framebuffer objects for everything that is not the EFB: copy destinations and
// offscreen passes (PassCmd in vgx_cmd.h). They are pooled by size, and one of
// them is used at most g_fboMaxUses times per frame, because of how the
// target GPU's render targets work (sceGxm, through vitaGL):
//  - a render target belongs to a framebuffer object and is created for a
//    size: attaching a texture of another size destroys and re-creates it,
//    attaching another texture of the same size is cheap;
//  - a render target is sized for a number of scenes per frame (at most 8); a
//    further scene on it in the same frame waits for the GPU;
//  - the number of render targets is limited (a few dozen).
// So: never re-attach across sizes, never draw to one object more than 8
// times a frame, and do not make one object per texture.
//   MSC_VGX_FBO_USES=n   uses per frame of one pooled object (default 8)
struct PoolFbo {
    int w = 0, h = 0;
    GLuint fbo = 0;
    GLuint color = 0;                 // own colour texture (passes that are copied from afterwards), made on demand
    GLuint depthTex = 0, depthRb = 0; // made on demand (passes)
    GLuint attached = 0;              // the colour texture attached at the moment
    uint32_t uses = 0;                // this frame
    uint32_t lastPresent = 0;         // g_poolPresents when it was last used (TrimPool)
};
std::vector<PoolFbo> g_pool;
uint32_t g_poolPresents = 0;      // presents so far (TrimPool's clock)
uint32_t g_fboMaxUses = 8;
// The usage hint of the display-list cache's static vertex and index buffers.
// MSC_VGX_STATIC_USAGE=dynamic (the Vita host's default; "static" elsewhere): on vitaGL the
// hint decides which memory pool a buffer comes from (source/buffers.c, glBufferData):
// GL_STATIC_DRAW takes VIDEO memory first, GL_DYNAMIC_DRAW and GL_STREAM_DRAW main memory
// first. In open play the static buffers are 18.5 MiB in 741 buffers (census of the stub-GL
// build, MSC_VGX_GLMEM=1), a quarter of what the renderer holds, and the console's video
// memory pool had 6.5 MiB left at that point while its main-memory pool had 44 MiB free
// (hw/run3-gl). Nothing else differs between the hints in the vitaGL build we use (the
// "scratch" pool they also select is compiled out).
GLenum g_staticUsage = GL_STATIC_DRAW;
int g_pass = -1;                // index in g_pool of the target of the open pass; -1: no pass target (the EFB)
bool g_inPass = false;          // draws currently go to g_pool[g_pass]
uint32_t g_passDirectTex = 0;   // not 0: the open pass is drawn straight into this copy texture (PassCmd::directTex)
int g_passX = 0, g_passY = 0;   // what to subtract from EFB pixel coordinates inside the pass (the pass rectangle's origin, less its place in an atlas)
int g_passW = 0, g_passH = 0;   // size of the pass's render target
// DYNRES (dynamic resolution, below): large offscreen passes (glow and blur chains, full-screen filters: a pass rectangle of at least an eighth of the
// EFB, not drawn into an atlas) are drawn at g_passScale of the render scale. g_psx / g_psy: the open pass's scale, EFB pixels to pass pixels
// (the render scale times g_passScale, or the render scale itself).
float g_passScale = 1.0f;
float g_psx = 1.0f, g_psy = 1.0f;
inline float CurScaleX() { return g_inPass ? g_psx : static_cast<float>(g_rtW) / static_cast<float>(g_efbW); }
inline float CurScaleY() { return g_inPass ? g_psy : static_cast<float>(g_rtH) / static_cast<float>(g_efbH); }

// ------------------------------------------------------------ cached GL state
// Nothing below calls GL unless the value changes. Invalidate() after
// anything outside this file may have touched the context (the present hook).
struct GlState {
    GLuint fbo = ~0u, program = ~0u, arrayBuf = ~0u, elemBuf = ~0u;
    int unit = -1;
    GLuint tex[8];
    int8_t scissorOn = -1, blendOn = -1, depthTest = -1, depthMask = -1, cullOn = -1, colorMask = -1;
    int sc[4] = {-1, -1, -1, -1}, vp[4] = {-1, -1, -1, -1};
    float zn = -1, zf = -1, lineWidth = -1;
    int blendEq = -1, blendSrc = -1, blendDst = -1, depthFunc = -1, cullFace = -1;
    // vertex arrays
    uint32_t attrEnabled = 0, attrConstSet = 0;
    GLuint layoutVbo = ~0u; // the attribute pointers describe this buffer ...
    uint32_t layoutMask = 0, layoutBase = ~0u; // ... with this layout at this byte offset
    GlState() { for (GLuint& t : tex) t = ~0u; }
} S;
// vitaGL's view of the render target, for the scene and scissor counts:
// a scene begins at the first draw or clear after the bound object changed
// (gxm.c scene_reset) or after its attachment was replaced while it was the
// one in use (shared.h _glFramebufferTexture2D -> dirty_framebuffer).
GLuint g_inUseFbo = ~0u;
bool g_fbDirty = false, g_scissorDirty = true;
unsigned g_dispFbo = 0; // the host's display for the progress bar (SetProgressDisplay)
int g_dispW = 0, g_dispH = 0;
bool g_uniDirtyV = true, g_uniDirtyF = true; // vitaGL: dirty_shader_vert_unifs / dirty_shader_frag_unifs

void Invalidate() {
    const uint32_t en = S.attrEnabled;
    S = GlState();
    // Which arrays are enabled is this file's own business (nobody else touches them).
    S.attrEnabled = en;
    g_scissorDirty = true;
    g_uniDirtyV = g_uniDirtyF = true;
}
// Called before every draw and clear.
bool g_sceneLog = false; // MSC_VGX_SCENELOG=<frame>: one line per scene of that frame
std::atomic<uint32_t> g_dbgExecCmd{0xFFFFFFFFu}; // MSC_VGX_ASYNCLOG (VRS): what the back end is executing, read by the stall watcher
void LogScene() {
    int w = g_rtW, h = g_rtH;
    const char* what = "EFB";
    if (S.fbo != g_efbFbo) {
        what = "?";
        for (const PoolFbo& f : g_pool)
            if (f.fbo == S.fbo) { w = f.w; h = f.h; what = f.attached ? "texture" : "pass buffer"; }
    }
    std::fprintf(stderr, "[scene] %2u: %s %dx%d (after %u draws, %u clears, %u copy quads)%s\n", C.scenes, what, w, h, C.draws, C.clears, C.copyQuads,
                 (S.fbo == g_inUseFbo) ? " same object, re-attached" : "");
}
// ------------------------------------------------------------ W5 RT: scenes and their depth load / store
// On vitaGL (STORE_DEPTH_STENCIL) every scene loads its target's whole depth buffer when it begins and stores it when it ends (4 bytes a
// sample), whatever it draws. The patched vitaGL (platform/vita/vitagl/0003-rt-*.patch, source/rt.c) takes a hint per scene: NOLOAD (nothing
// in the scene reads depth that was there before it began) and NOSTORE (nothing reads its depth after it). This back end knows its own command
// buffer, so it decides (MSC_VGX_RT=1):
//  - a pass target (atlas page or pass buffer) or a copy destination: NOLOAD | NOSTORE when the scene begins with the pass's clear, a copy
//    quad or a copy's clear. Every pass clears its cell (colour and depth) before it draws, its draws stay inside the cell, nothing ever reads
//    a pass's depth after its copy, and a copy never uses depth. What could break that is a scene change in the middle of a pass: the only one
//    this file makes is the progress bar, which waits while a pass is open (rtHeldProgress).
//  - the EFB: from a scan of the buffer (RtScan): NOLOAD when the scene's first operation clears the whole depth buffer or nothing reads the
//    depth that is there before it is cleared; NOSTORE when nothing reads the EFB's depth after the next command that ends the scene (a pass,
//    a copy, the display copy, a new target size) before it is cleared. The display copy's clear (CopyDisp) is moved into the next buffer, just
//    before its first command that uses the EFB (rtDeferredClears): the scene it begins (the frame's main EFB scene) is decided with that buffer
//    in view, and when a frame begins with passes (shadows, impostors) the clear no longer makes an EFB scene of its own before them.
// MSC_VGX_RTSIM=1 (host GL, for the picture proof): where the console would leave depth undefined (a scene that does not load, or that loads
// what a NOSTORE scene did not store), the depth buffer is cleared to a hostile value (0.0 or 0.37, alternating per frame) first. Pictures equal
// to a run without it show that no hint drops depth that is read.
enum : uint8_t { OP_DRAW, OP_CLEAR, OP_CLEAR_FULLDEPTH, OP_PASS_CLEAR, OP_COPY };
struct RtScene {
    bool open = false;
    uint8_t kind = 0; // 0 EFB, 1 pass, 2 copy, 3 display, 4 other
    uint32_t draws = 0, depthDraws = 0, copies = 0;
};
struct Rt {
    int mode = -1;              // MSC_VGX_RT: 0 off, 1 hints (default: 0 on the host, the Vita host sets the default)
    bool sim = false;           // MSC_VGX_RTSIM (host builds only)
    int timeDraws = 0;          // MSC_VGX_RTTIME
    std::vector<uint8_t> live;  // [i]: the EFB depth that is there before command i is read later (before a whole clear); [n]: conservative 1
    std::vector<int> nextBreak; // [i]: the first command after i that ends an EFB scene (n: none in this buffer)
    int firstBreak = 0;         // the first such command of the buffer (for the deferred clear)
    int deferAt = -1;           // the display copy whose clear moves to the next buffer
    int clearAt = -1;           // the first command of this buffer that uses the EFB: the moved clear is made just before it (-1: none here)
    bool clearInCopy = false;   // ... inside that command, a copy whose only use of the EFB is its own clear: just before that clear
    bool beforeCmd = false;     // the scene that begins now begins before command `cmd` (the moved clear)
    std::vector<uint8_t> brk;   // [i]: command i ends an EFB scene
    int cmd = -1;               // the command being executed (-1: before the first)
    int n = 0;
    bool scanned = false;
    RtScene cur;
    GLuint prevFbo = ~0u, prevPrevFbo = ~0u;
    uint32_t curHints = 0;
    bool firstDraw = false;     // no draw yet in the scene that began last (RtTimedDraw: "the first draw after a target change")
    std::vector<GLuint> lost;   // RTSIM: framebuffer objects whose depth a NOSTORE scene did not keep
    float simDepth = 0.0f;
    bool havePending = false;   // a display copy's clear waits for the next buffer
    ClearSpec pendingClear;
    EfbRect pendingRect;
} g_rt;
SceneHintHook g_hintHook = nullptr;
MicroClock g_microClock = nullptr;
uint32_t MicroNow() {
    if (g_microClock) return g_microClock();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
int RtMode() {
    if (g_rt.mode < 0) {
        g_rt.mode = static_cast<int>(EnvInt("MSC_VGX_RT", 0));
#if !defined(__VGX_VITA__)
        g_rt.sim = EnvInt("MSC_VGX_RTSIM", 0) != 0;
#endif
        g_rt.timeDraws = static_cast<int>(EnvInt("MSC_VGX_RTTIME", 0));
        if (EnvInt("MSC_VGX_PROF", 0) == 1) g_rt.mode = 0; // (PROF=1 calls glFinish after copies: a scene change this file does not plan for)
    }
    return g_rt.mode;
}
uint32_t DepthKib(int w, int h) { // vitaGL's depth surface of a w x h target (DF32M, tiles of 32 x 32), KiB
    const uint32_t aw = (static_cast<uint32_t>(w) + 31u) & ~31u, ah = (static_cast<uint32_t>(h) + 31u) & ~31u;
    return (aw * ah * 4u) >> 10;
}
void SimLostDepth() { // RTSIM: what the console leaves in a depth buffer that was not loaded: something else
#if !defined(__VGX_VITA__) && !defined(VGX_GL_STUB)
    const int8_t sc = S.scissorOn, dm = S.depthMask;
    if (sc != 0) G(glDisable(GL_SCISSOR_TEST));
    if (dm != 1) G(glDepthMask(GL_TRUE));
    GLfloat keep = 1.0f;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &keep);
    G(glClearDepthf(g_rt.simDepth));
    G(glClear(GL_DEPTH_BUFFER_BIT));
    G(glClearDepthf(keep));
    if (sc == 1) G(glEnable(GL_SCISSOR_TEST));
    else if (sc < 0) S.scissorOn = 0;
    if (dm == 0) G(glDepthMask(GL_FALSE));
    else if (dm < 0) S.depthMask = 1;
    ++C.rtSimClears;
#endif
}
void RtSceneEnd() {
    RtScene& sc = g_rt.cur;
    if (!sc.open) return;
    if (sc.depthDraws) ++C.rtDepthScenes;
    if (!sc.draws && !sc.copies) ++C.rtEmptyScenes;
    sc = RtScene{};
}
// A scene begins (TouchTarget): what it is, and the hints for it.
void RtSceneBegin(uint8_t op) {
    RtSceneEnd();
    const int mode = RtMode();
    RtScene& sc = g_rt.cur;
    sc.open = true;
    int w = g_rtW, h = g_rtH;
    if (S.fbo == g_efbFbo) {
        sc.kind = 0;
    } else if (S.fbo == static_cast<GLuint>(g_dispFbo)) {
        sc.kind = 3;
        w = g_dispW;
        h = g_dispH;
        ++C.rtDispScenes;
    } else {
        sc.kind = 4;
        for (const PoolFbo& f : g_pool)
            if (f.fbo == S.fbo) {
                w = f.w;
                h = f.h;
                // (BeginPass's clear comes before g_inPass is set)
                sc.kind = (op == OP_PASS_CLEAR || (g_inPass && g_pass >= 0 && g_pool[g_pass].fbo == S.fbo)) ? 1 : 2;
                break;
            }
        if (sc.kind == 1) ++C.rtPassScenes;
        else if (sc.kind == 2) ++C.rtCopyScenes;
    }
    if (S.fbo == g_rt.prevPrevFbo && S.fbo != g_rt.prevFbo) ++C.rtReturns;
    g_rt.prevPrevFbo = g_rt.prevFbo;
    g_rt.prevFbo = S.fbo;
    if (op == OP_CLEAR || op == OP_CLEAR_FULLDEPTH || op == OP_PASS_CLEAR) ++C.rtClearFirst;
    if (op == OP_CLEAR_FULLDEPTH) ++C.rtFullDepthFirst;
    const uint32_t kib = DepthKib(w, h);
    C.rtDsKib += 2 * kib;
    uint32_t hints = 0;
    if (mode > 0 && g_rt.scanned) {
        if (sc.kind == 0) {
            // the EFB: what is there before this operation, and what is there when the scene ends
            const int i = g_rt.cmd;
            bool before = true;
            if (op == OP_CLEAR_FULLDEPTH) before = false;
            else if (i >= 0 && i < g_rt.n) before = g_rt.live[i] != 0;
            else if (i < 0) before = g_rt.n > 0 ? g_rt.live[0] != 0 : true;
            if (!before) hints |= kHintNoLoad;
            int j = i < 0 ? g_rt.firstBreak : (i < g_rt.n ? g_rt.nextBreak[i] : g_rt.n);
            if (g_rt.beforeCmd && i >= 0 && i < g_rt.n && g_rt.brk[static_cast<size_t>(i)]) j = i; // (the moved clear: command i itself may end the scene)
            if (j < g_rt.n && !g_rt.live[j]) hints |= kHintNoStore;
        } else if (sc.kind == 1 || sc.kind == 2) {
            if (op != OP_DRAW) hints |= kHintNoLoad | kHintNoStore;
        } else if (sc.kind == 3) {
            hints |= kHintNoLoad | kHintNoStore; // the progress bar: no depth
        }
    }
    g_rt.curHints = hints;
    g_rt.firstDraw = true;
    if (hints & kHintNoLoad) { ++C.rtNoLoad; C.rtDsSkipKib += kib; }
    if (hints & kHintNoStore) { ++C.rtNoStore; C.rtDsSkipKib += kib; }
    if (hints && g_hintHook) g_hintHook(S.fbo, hints);
    if (g_rt.sim) {
        auto it = std::find(g_rt.lost.begin(), g_rt.lost.end(), S.fbo);
        const bool lost = it != g_rt.lost.end();
        if (lost) g_rt.lost.erase(it);
        if (((hints & kHintNoLoad) || lost) && sc.kind != 3) SimLostDepth();
        if (hints & kHintNoStore) g_rt.lost.push_back(S.fbo);
    }
}
inline void RtNoteDraw(bool depthTest) {
    g_rt.firstDraw = false;
    ++g_rt.cur.draws;
    if (depthTest) ++g_rt.cur.depthDraws;
}
inline void TouchTarget(uint8_t op = OP_DRAW) {
    if (S.fbo != g_inUseFbo || g_fbDirty) {
        if (g_sceneLog) LogScene();
        ++C.scenes;
        if (S.fbo == g_efbFbo) ++C.efbScenes;
        RtSceneBegin(op);
        g_inUseFbo = S.fbo;
        g_fbDirty = false;
        g_scissorDirty = true; // (vitaGL, STORE_DEPTH_STENCIL: every scene on a framebuffer object rebuilds the mask)
    }
    if (g_scissorDirty) { ++C.scissorUpdates; g_scissorDirty = false; }
}
inline void BindFbo(GLuint fbo) {
    if (S.fbo == fbo) return;
    G(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    ++C.fboBinds;
    S.fbo = fbo;
}
inline void AttachColor(GLuint tex) { // to the bound framebuffer object
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
    ++C.fboAttach;
    if (g_inUseFbo == S.fbo) g_fbDirty = true;
}
inline void UseProgram(GLuint id) {
    if (S.program == id) return;
    G(glUseProgram(id));
    ++C.programUses;
    S.program = id;
    g_uniDirtyV = g_uniDirtyF = true;
}
inline void BindTex(int unit, GLuint name) {
    if (S.tex[unit] == name) return;
    if (S.unit != unit) { G(glActiveTexture(GL_TEXTURE0 + unit)); S.unit = unit; }
    G(glBindTexture(GL_TEXTURE_2D, name));
    ++C.texBinds;
    S.tex[unit] = name;
}
// For glTexImage2D / glTexParameteri on `name`: bound on unit 0 and unit 0 active.
inline void BindTexForEdit(GLuint name) {
    if (S.unit != 0) { G(glActiveTexture(GL_TEXTURE0)); S.unit = 0; }
    if (S.tex[0] != name) { G(glBindTexture(GL_TEXTURE_2D, name)); ++C.texBinds; S.tex[0] = name; }
}
inline void ForgetTex(GLuint name) { // a deleted name may be handed out again
    for (GLuint& t : S.tex)
        if (t == name) t = ~0u;
}
inline void BindArrayBuf(GLuint b) {
    if (S.arrayBuf == b) return;
    G(glBindBuffer(GL_ARRAY_BUFFER, b));
    ++C.vboBinds;
    S.arrayBuf = b;
}
inline void BindElemBuf(GLuint b) {
    if (S.elemBuf == b) return;
    G(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b));
    ++C.vboBinds;
    S.elemBuf = b;
}
inline void SetScissor(bool on, int x = 0, int y = 0, int w = 0, int h = 0) {
    if (S.scissorOn != static_cast<int8_t>(on)) {
        if (on) G(glEnable(GL_SCISSOR_TEST)); else G(glDisable(GL_SCISSOR_TEST));
        ++C.scissorCalls;
        S.scissorOn = on;
        g_scissorDirty = true;
    }
    if (on && (S.sc[0] != x || S.sc[1] != y || S.sc[2] != w || S.sc[3] != h)) {
        G(glScissor(x, y, w, h));
        ++C.scissorCalls;
        S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h;
        g_scissorDirty = true;
    }
}
inline void SetViewport(int x, int y, int w, int h) {
    if (S.vp[0] == x && S.vp[1] == y && S.vp[2] == w && S.vp[3] == h) return;
    G(glViewport(x, y, w, h));
    ++C.viewportCalls;
    S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h;
}
inline void SetDepthRange(float n, float f) {
    if (S.zn == n && S.zf == f) return;
    G(glDepthRangef(n, f));
    ++C.stateCalls;
    S.zn = n; S.zf = f;
}
inline void SetBlendOff() {
    if (S.blendOn == 0) return;
    G(glDisable(GL_BLEND));
    ++C.stateCalls;
    S.blendOn = 0;
}
inline void SetBlend(GLenum eq, GLenum src, GLenum dst) {
    if (S.blendOn != 1) { G(glEnable(GL_BLEND)); ++C.stateCalls; S.blendOn = 1; }
    if (S.blendEq != static_cast<int>(eq)) { G(glBlendEquation(eq)); ++C.stateCalls; S.blendEq = static_cast<int>(eq); }
    if (S.blendSrc != static_cast<int>(src) || S.blendDst != static_cast<int>(dst)) {
        G(glBlendFunc(src, dst));
        ++C.stateCalls;
        S.blendSrc = static_cast<int>(src); S.blendDst = static_cast<int>(dst);
    }
}
inline void SetDepthTest(bool on, GLenum func = GL_ALWAYS) {
    if (S.depthTest != static_cast<int8_t>(on)) {
        if (on) G(glEnable(GL_DEPTH_TEST)); else G(glDisable(GL_DEPTH_TEST));
        ++C.stateCalls;
        S.depthTest = on;
    }
    if (on && S.depthFunc != static_cast<int>(func)) { G(glDepthFunc(func)); ++C.stateCalls; S.depthFunc = static_cast<int>(func); }
}
inline void SetDepthMask(bool on) {
    if (S.depthMask == static_cast<int8_t>(on)) return;
    G(glDepthMask(on ? GL_TRUE : GL_FALSE));
    ++C.stateCalls;
    S.depthMask = on;
}
inline void SetCull(uint8_t cull) { // CULL_*
    if (cull == CULL_NONE) {
        if (S.cullOn != 0) { G(glDisable(GL_CULL_FACE)); ++C.stateCalls; S.cullOn = 0; }
        return;
    }
    if (S.cullOn != 1) { G(glEnable(GL_CULL_FACE)); ++C.stateCalls; S.cullOn = 1; }
    const GLenum face = cull == CULL_BACK ? GL_BACK : (cull == CULL_FRONT ? GL_FRONT : GL_FRONT_AND_BACK);
    if (S.cullFace != static_cast<int>(face)) { G(glCullFace(face)); ++C.stateCalls; S.cullFace = static_cast<int>(face); }
}
inline void SetColorMask(bool color, bool alpha) {
    const int8_t m = static_cast<int8_t>((color ? 1 : 0) | (alpha ? 2 : 0));
    if (S.colorMask == m) return;
    G(glColorMask(color, color, color, alpha));
    ++C.stateCalls;
    S.colorMask = m;
}

// ---- shader work of the command buffer being executed (see SetProgressDisplay in vgx_cmd.h)
inline double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct ShaderWork {
    uint32_t total = 0, done = 0;       // weighted units: shader 10, program 2, variant 1
    uint32_t compiles = 0, simHits = 0, links = 0, variants = 0, shown = 0;
    double ms = 0;                      // measured + simulated
    double sinceShow = 1e9;
    double simPending = 0;              // simulated charge of the command being executed
} g_sw;
ProgressShowHook g_showHook = nullptr;

// MSC_VGX_SIM_COMPILE_MS: what a compile would have cost on the console, with the on-disk
// cache vitaGL keeps (key: a hash of the shader text; a hit costs nothing here).
void SimulateCompile(const char* text) {
    static const long simMs = EnvInt("MSC_VGX_SIM_COMPILE_MS", 0);
    if (simMs <= 0) return;
    static const char* const dir = std::getenv("MSC_VGX_SIM_CACHE");
    if (dir && *dir) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/%016llx.bin", dir, static_cast<unsigned long long>(HashKey(text, static_cast<uint32_t>(std::strlen(text)))));
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            ++g_sw.simHits;
            ++g_stats.shaderSimCacheHits;
            return;
        }
        if (FILE* f = std::fopen(path, "wb")) { std::fputc(1, f); std::fclose(f); }
    }
    g_sw.simPending += static_cast<double>(simMs);
#if !defined(__VGX_VITA__)
    static const bool sleep = EnvInt("MSC_VGX_SIM_COMPILE_SLEEP", 0) != 0;
    if (sleep) {
        // (a system call with a relative time: the harness's virtual clock does not shorten it)
        struct timespec ts = {static_cast<time_t>(simMs / 1000), static_cast<long>(simMs % 1000) * 1000000L};
        nanosleep(&ts, nullptr);
    }
#endif
}

GLuint Compile(GLenum type, const char* text, const char* what) {
    const GLuint s = G(glCreateShader(type));
    if (!s || s == 0xFFFFFFFFu) return 0;
    const char* parts[2] = {"#version 100\n", text};
    G(glShaderSource(s, 2, parts, nullptr));
    G(glCompileShader(s));
    ++C.shaderCompiles;
    ++g_sw.compiles;
    ++g_stats.shaderCompiles;
    SimulateCompile(text);
    GLint ok = 0;
    G(glGetShaderiv(s, GL_COMPILE_STATUS, &ok));
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        VGX_ERR("%s shader compile failed:\n%s\n---- source ----\n%s\n", what, log, text);
        G(glDeleteShader(s));
        return 0;
    }
    return s;
}

struct ShaderAttempt { unsigned count = 0; uint64_t retryAfter = 0; };
std::vector<ShaderAttempt> g_vsAttempts, g_fsAttempts;
uint64_t g_shaderRetryFrame = ~0ull;

GLuint CompileShaderSlot(const ShaderSource& source, bool initial) {
    auto& ids = source.fragment ? g_fs : g_vs;
    auto& sources = source.fragment ? g_fsSrc : g_vsSrc;
    auto& attempts = source.fragment ? g_fsAttempts : g_vsAttempts;
    const size_t index = source.id;
    if (ids.size() <= index) ids.resize(index + 1, 0);
    if (sources.size() <= index) sources.resize(index + 1, nullptr);
    if (attempts.size() <= index) attempts.resize(index + 1);
    sources[index] = &source;
    if (ids[index]) return ids[index];
    ShaderAttempt& attempt = attempts[index];
    if (attempt.count >= 4 || g_serial < attempt.retryAfter) return 0;
    // Initial catalogue compilation is unchanged. Recovery is shared by all
    // pairs using this shader, with at most one recovery compile per frame.
    if (!initial || attempt.count) {
        if (g_shaderRetryFrame == g_serial) return 0;
        g_shaderRetryFrame = g_serial;
    }
    ++attempt.count;
    ids[index] = Compile(source.fragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER,
                         source.text.c_str(), source.fragment ? "fragment" : "vertex");
    if (!ids[index]) attempt.retryAfter = g_serial + (60ull << (attempt.count - 1));
    return ids[index];
}

void SetupNewTexture(GLenum minMag) { // the texture being edited: no mips, clamped
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(minMag)));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(minMag)));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    C.texParams += 4;
}

extern "C" int vgx_tex_has_storage(void) __attribute__((weak));
// false: no memory for the new colour target (the console reports no error); nothing was attached, g_rtW/g_rtH are unchanged.
bool CreateTarget() {
    int w = 0, h = 0;
    DynTargetSize(g_efbW, g_efbH, &w, &h);
    if (w == g_rtW && h == g_rtH && g_efbFbo) return true;
    if (!g_efbFbo) {
        G(glGenFramebuffers(1, &g_efbFbo));
        G(glGenTextures(1, &g_efbColor));
    }
    BindTexForEdit(g_efbColor);
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    if (vgx_tex_has_storage && !vgx_tex_has_storage()) {
        VGX_ERR("no GPU memory for a %dx%d render target: the size stays %dx%d\n", w, h, g_rtW, g_rtH);
        return false;
    }
    g_rtW = w;
    g_rtH = h;
    SetupNewTexture(GL_LINEAR);
    BindFbo(g_efbFbo);
    AttachColor(g_efbColor);
    if (g_hasDepthTex) {
        // A depth *texture* lets EFB depth copies be sampled.
        if (!g_efbDepthTex) G(glGenTextures(1, &g_efbDepthTex));
        BindTexForEdit(g_efbDepthTex);
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr));
        SetupNewTexture(GL_NEAREST);
        G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_efbDepthTex, 0));
    } else {
        if (!g_efbDepthRb) G(glGenRenderbuffers(1, &g_efbDepthRb));
        G(glBindRenderbuffer(GL_RENDERBUFFER, g_efbDepthRb));
        G(glRenderbufferStorage(GL_RENDERBUFFER, g_hasDepth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16, w, h));
        G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_efbDepthRb));
    }
    const GLenum st = G(glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (st != GL_FRAMEBUFFER_COMPLETE) VGX_ERR("EFB framebuffer incomplete: 0x%X\n", st);
    SetViewport(0, 0, w, h);
    SetScissor(false);
    SetColorMask(true, true);
    SetDepthMask(true);
    G(glClearColor(0, 0, 0, 1));
    G(glClearDepthf(1.0f));
    TouchTarget(OP_CLEAR_FULLDEPTH);
    G(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ++C.clears;
    VGX_LOG("render target %dx%d for EFB %dx%d (depth %s)\n", w, h, g_efbW, g_efbH,
            g_hasDepthTex ? "texture" : (g_hasDepth24 ? "24-bit rb" : "16-bit rb"));
    return true;
}

// 1.1 (resize stalls): EFB targets of sizes left by the dynamic resolution, kept for a return to that size (a Mega Strike's start and
// end): their framebuffer object keeps its GPU render target, which vitaGL would otherwise make again. At most three; dropped after 1800
// presents unused, or at once when less than 24 MiB of video memory is free (TrimPool). MSC_VGX_KEEP_TARGETS=0: off.
struct EfbSlot { int w = 0, h = 0; GLuint fbo = 0, color = 0, depthRb = 0, depthTex = 0; uint32_t lastPresent = 0; };
EfbSlot g_efbKept[3];
uint32_t g_efbReused = 0;
bool EfbKeepOn() {
    static const bool on = EnvInt("MSC_VGX_KEEP_TARGETS", 1) != 0;
    return on;
}
void EfbDrop(EfbSlot& k) {
    if (!k.fbo) return;
    if (S.fbo == k.fbo) { G(glBindFramebuffer(GL_FRAMEBUFFER, 0)); S.fbo = 0; g_fbDirty = true; }
    if (g_inUseFbo == k.fbo) g_inUseFbo = ~0u;
    if (k.color) { GLuint t = k.color; G(glDeleteTextures(1, &t)); ForgetTex(t); }
    if (k.depthTex) { GLuint t = k.depthTex; G(glDeleteTextures(1, &t)); ForgetTex(t); }
    if (k.depthRb) G(glDeleteRenderbuffers(1, &k.depthRb));
    G(glDeleteFramebuffers(1, &k.fbo));
    k = EfbSlot{};
}
bool EfbTake(int w, int h, EfbSlot& out) {
    for (EfbSlot& k : g_efbKept)
        if (k.fbo && k.w == w && k.h == h) { out = k; k = EfbSlot{}; return true; }
    return false;
}
void EfbKeep(const EfbSlot& cur) {
    EfbSlot* dst = nullptr;
    for (EfbSlot& k : g_efbKept) if (!k.fbo) { dst = &k; break; }
    if (!dst) {   // (all used: the one left longest ago goes)
        dst = &g_efbKept[0];
        for (EfbSlot& k : g_efbKept) if (k.lastPresent < dst->lastPresent) dst = &k;
        EfbDrop(*dst);
    }
    *dst = cur;
}
// A taken-back target: depth cleared to 1.0 as a new one starts (its colour is overwritten with the old picture).
void ClearTargetDepth() {
    BindFbo(g_efbFbo);
    SetViewport(0, 0, g_rtW, g_rtH);
    SetScissor(false);
    SetColorMask(true, true);
    SetDepthMask(true);
    G(glClearColor(0, 0, 0, 1));
    G(glClearDepthf(1.0f));
    TouchTarget(OP_CLEAR_FULLDEPTH);
    G(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ++C.clears;
}

// In front of g_programs: the program last asked for in each of 64 classes (entries of an
// unordered_map do not move; EvictPrograms empties this when it deletes any).
struct ProgramFront { uint32_t key; Program* p; };
ProgramFront g_programFront[1024] = {};   // (1.1: 64 collided in a match; from the Galaxy port)
uint64_t g_programRetryFrame = ~0ull;

// W5 SEED (after Galaxy 98a0d5e). vitaGL has room for 1,024 linked programs and its
// glCreateProgram does not fail gracefully when they are used up (custom_shaders.c: the slot
// index stays 0xFFFFFFFF, the next glAttachShader / glLinkProgram / glUseProgram on it set
// GL_INVALID_VALUE and the patcher logs "patching failed ... shader 0x0"). The shader catalogue
// of this game is 1,054 programs; a session that visits many stadiums and captains links a few
// hundred (one match 276, thirty 3-minute matches 650, tools/seed/session_growth.py), and the
// console's own warm-up list grows by every program met, so a long-lived install could pass
// the limit. MSC_VGX_MAXPROGRAMS (default 900 on the console, none elsewhere; 0 = no limit):
//  * the warm-up (CMD_PROGRAM / CMD_VARIANT) links no new program once that many are linked
//    (a list longer than the cap is linked lazily: first use is a link from the shader cache);
//  * at the start of a frame with more than that many linked, the programs that have gone
//    longest without a draw (none drawn in the last 600 frames) are deleted, a hundred at a
//    time. A deleted program is linked again when a draw asks for it (its shaders stay
//    compiled; the console's disk cache answers the link in a few milliseconds).
size_t ProgramLimit() {
#ifdef __VGX_VITA__
    static const size_t limit = static_cast<size_t>(EnvInt("MSC_VGX_MAXPROGRAMS", 900));
#else
    static const size_t limit = static_cast<size_t>(EnvInt("MSC_VGX_MAXPROGRAMS", 0));
#endif
    return limit;
}
bool WarmRoom(uint16_t vsId, uint16_t fsId) {
    const size_t limit = ProgramLimit();
    if (!limit || g_stats.programs < limit) return true;
    return g_programs.find((static_cast<uint32_t>(vsId) << 16) | fsId) != g_programs.end();
}
// Deletes up to `want` programs, the ones longest without a draw, none that a draw used less than `minAge` frames ago.
size_t DeletePrograms(size_t want, uint64_t minAge) {
    static std::vector<std::pair<uint64_t, uint32_t>> old; // (last frame used, key)
    old.clear();
    for (const auto& kv : g_programs)
        if (kv.second.ok && kv.second.usedSerial + minAge <= g_serial && kv.second.usedSerial != g_serial) old.emplace_back(kv.second.usedSerial, kv.first);
    if (old.empty()) return 0;
    std::sort(old.begin(), old.end());
    size_t n = 0;
    for (; n < old.size() && n < want; ++n) {
        auto it = g_programs.find(old[n].second);
        if (it->second.id && it->second.id != 0xFFFFFFFFu) {
            if (S.program == it->second.id) S.program = ~0u;
            G(glDeleteProgram(it->second.id));
            if (g_stats.programs) --g_stats.programs;
        }
        g_programs.erase(it);
    }
    for (ProgramFront& f : g_programFront) f = ProgramFront{};
    ++g_evictions;
    g_evicted += static_cast<uint32_t>(n);
    return n;
}
void EvictPrograms() {
    const size_t limit = ProgramLimit();
    if (!limit || g_stats.programs <= limit) return;
    const size_t want = g_stats.programs - (limit > 100 ? limit - 100 : 0);
    const size_t n = DeletePrograms(want, 601);
    if (n) VGX_LOG("%u programs deleted (unused for 600 frames or more); %u linked now\n", static_cast<unsigned>(n), static_cast<unsigned>(g_stats.programs));
}

Program& GetProgram(uint16_t vsId, uint16_t fsId) {
    const uint32_t key = (static_cast<uint32_t>(vsId) << 16) | fsId;
    auto& fr = g_programFront[((vsId * 7u + fsId) * 2654435761u) >> 22];
    if (fr.p && fr.key == key && fr.p->ok) return *fr.p;
    auto it = g_programs.find(key);
    uint8_t failures = 0;
    if (it != g_programs.end()) {
        if (it->second.ok || it->second.failures >= 4 || g_serial < it->second.retryAfter || g_programRetryFrame == g_serial) {
            fr.key = key;
            fr.p = &it->second;
            return it->second;
        }
        failures = it->second.failures;
    }
    Program p;
    p.failures = failures;
    auto store = [&]() -> Program& {
        if (it != g_programs.end()) {
            it->second = std::move(p);
            return it->second;
        }
        return g_programs.emplace(key, std::move(p)).first->second;
    };
    auto failed = [&](bool linkAttempt = true) -> Program& {
        p.usedSerial = g_serial;
        if (linkAttempt) ++p.failures;
        p.retryAfter = g_serial + (linkAttempt ? (60ull << (p.failures - 1)) : 1);
        return store();
    };
    if (vsId < g_vsSrc.size() && fsId < g_fsSrc.size() && g_vsSrc[vsId] && g_fsSrc[fsId]) {
        if (vsId >= g_vs.size() || !g_vs[vsId]) CompileShaderSlot(*g_vsSrc[vsId], false);
        if (fsId >= g_fs.size() || !g_fs[fsId]) CompileShaderSlot(*g_fsSrc[fsId], false);
    }
    if (vsId < g_vs.size() && fsId < g_fs.size() && vsId < g_vsSrc.size() && fsId < g_fsSrc.size() &&
        g_vs[vsId] && g_fs[fsId] && g_vsSrc[vsId] && g_fsSrc[fsId]) {
        if (it != g_programs.end()) g_programRetryFrame = g_serial;
        p.id = G(glCreateProgram());
        if (!p.id || p.id == 0xFFFFFFFFu) {
            // vitaGL's table is full (MSC_VGX_MAXPROGRAMS is 0 or above 1,024, or a hundred programs were
            // made within 600 frames): free the hundred longest without a draw, none used this frame, and ask again.
            const size_t n = DeletePrograms(100, 1);
            static bool said = false;
            if (!said || n) { said = true; VGX_LOG("no free program slot: %u programs deleted, %u linked now\n", static_cast<unsigned>(n), static_cast<unsigned>(g_stats.programs)); }
            p.id = G(glCreateProgram());
            if (!p.id || p.id == 0xFFFFFFFFu) {
                p.id = 0; // no program for this pair: its draws are skipped (!ok), as for a pair without shaders
                return failed();
            }
        }
        G(glAttachShader(p.id, g_vs[vsId]));
        G(glAttachShader(p.id, g_fs[fsId]));
        G(glBindAttribLocation(p.id, AT_POS, "a_pos"));
        G(glBindAttribLocation(p.id, AT_NRM, "a_nrm"));
        G(glBindAttribLocation(p.id, AT_COL0, "a_col0"));
        G(glBindAttribLocation(p.id, AT_COL1, "a_col1"));
        for (int t = 0; t < 8; ++t) {
            char n[16];
            std::snprintf(n, sizeof n, "a_tex%d", t);
            G(glBindAttribLocation(p.id, AT_TEX0 + t, n));
        }
        G(glBindAttribLocation(p.id, AT_TMI0, "a_tmi0"));
        G(glBindAttribLocation(p.id, AT_TMI1, "a_tmi1"));
        if (g_vsSrc[vsId]->nBone) { // (W5 SKIN; the other programs bind exactly what they did)
            G(glBindAttribLocation(p.id, AT_BIX, "a_bix"));
            G(glBindAttribLocation(p.id, AT_BW, "a_bw"));
        }
        G(glLinkProgram(p.id));
        ++C.programLinks;
        ++g_sw.links;
        ++g_stats.programLinks;
        GLint ok = 0;
        G(glGetProgramiv(p.id, GL_LINK_STATUS, &ok));
        if (!ok) {
            char log[2048] = {};
            glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
            VGX_ERR("program link failed (vs %u, fs %u):\n%s\n---- vs ----\n%s\n---- fs ----\n%s\n", vsId, fsId, log,
                    g_vsSrc[vsId]->text.c_str(), g_fsSrc[fsId]->text.c_str());
            G(glDeleteProgram(p.id));
            p.id = 0;
        } else {
            p.ok = true;
            UseProgram(p.id);
            const ShaderSource& vs = *g_vsSrc[vsId];
            // What the shader text declares (vec4 elements per array).
            const GLint dMv = vs.nMv, dTm = vs.nTm, dPtm = vs.nPtm, dPal = vs.nPal, dLt = vs.nLt, dTev = g_fsSrc[fsId]->nTev, dBone = vs.nBone;
            p.vsBytes = 64u + 16u * static_cast<uint32_t>(dMv + dTm + dPtm + dPal + dLt + dBone);
            p.fsBytes = 16u * static_cast<uint32_t>(dTev);
            if (vs.text.find("a_nrm;") != std::string::npos) p.layoutNeed |= VL_NRM;
            if (vs.text.find("a_col0;") != std::string::npos) p.layoutNeed |= VL_COL0;
            if (vs.text.find("a_col1;") != std::string::npos) p.layoutNeed |= VL_COL1;
            if (vs.text.find("a_tmi0;") != std::string::npos) p.layoutNeed |= VL_TMI;
            if (dBone) p.layoutNeed |= VL_SKIN;
            for (int t = 0; t < 8; ++t) {
                const char name[8] = {'a', '_', 't', 'e', 'x', static_cast<char>('0' + t), ';', 0};
                if (vs.text.find(name) != std::string::npos) p.layoutNeed |= VL_TEX0 << t;
            }
#ifdef VGX_GL_STUB
            // No GL to ask: take the declarations.
            p.proj = 1;
            if (dMv) { p.mv = 1; p.nMv = dMv; }
            if (dTm) { p.tm = 1; p.nTm = dTm; }
            if (dPtm) { p.ptm = 1; p.nPtm = dPtm; }
            if (dPal) { p.pal = 1; p.nPal = dPal; }
            if (dLt) { p.lt = 1; p.nLt = dLt; }
            if (dBone) { p.bone = 1; p.nBone = dBone; }
            p.tev = 1; p.nTev = dTev;
#else
            // Array sizes as the GL reports them (a compiler may shorten an array to its
            // highest used element, and on vitaGL an upload beyond the reported size would
            // write past the array in the program's uniform buffer), never more than declared.
            const auto lim = [](GLint reported, GLint declared) { return reported < declared ? reported : declared; };
            GLint n = 0;
            G(glGetProgramiv(p.id, GL_ACTIVE_UNIFORMS, &n));
            for (GLint i = 0; i < n; ++i) {
                char name[64];
                GLint size = 0;
                GLenum type = 0;
                G(glGetActiveUniform(p.id, static_cast<GLuint>(i), sizeof name, nullptr, &size, &type, name));
                if (char* br = std::strchr(name, '[')) *br = 0;
                const GLint loc = G(glGetUniformLocation(p.id, name));
                if (!std::strcmp(name, "u_proj")) p.proj = loc;
                else if (!std::strcmp(name, "u_mv")) { p.mv = loc; p.nMv = lim(size, dMv); }
                else if (!std::strcmp(name, "u_tm")) { p.tm = loc; p.nTm = lim(size, dTm); }
                else if (!std::strcmp(name, "u_ptm")) { p.ptm = loc; p.nPtm = lim(size, dPtm); }
                else if (!std::strcmp(name, "u_pal")) { p.pal = loc; p.nPal = lim(size, dPal); }
                else if (!std::strcmp(name, "u_lt")) { p.lt = loc; p.nLt = lim(size, dLt); }
                else if (!std::strcmp(name, "u_tev")) { p.tev = loc; p.nTev = lim(size, dTev); }
                else if (!std::strcmp(name, "u_bone")) { p.bone = loc; p.nBone = lim(size, dBone); }
                else if (!std::strncmp(name, "u_tex", 5)) G(glUniform1i(loc, name[5] - '0'));
            }
#endif
        }
        if (p.ok) ++g_stats.programs;
        if (!p.ok) return failed();
    } else {
        return failed(false);
    }
    return store();
}

GLenum MapBlend(uint8_t f) {
    switch (f) {
    case BF_ZERO: return GL_ZERO;
    case BF_ONE: return GL_ONE;
    case BF_SRC_COLOR: return GL_SRC_COLOR;
    case BF_INV_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
    case BF_SRC_ALPHA: return GL_SRC_ALPHA;
    case BF_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case BF_DST_ALPHA: return GL_DST_ALPHA;
    case BF_INV_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case BF_DST_COLOR: return GL_DST_COLOR;
    default: return GL_ONE_MINUS_DST_COLOR;
    }
}
const GLenum kCmp[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};

// floor(v + 0.5) without the call into the maths library (eight per draw).
inline int Rnd(float v) {
    const float w = v + 0.5f;
    const int i = static_cast<int>(w);
    return i - (w < static_cast<float>(i) ? 1 : 0);
}

// The draw state that was applied last (with the target it was applied for).
RenderState g_rs;
bool g_rsValid = false;

// Returns false when nothing can be drawn (empty scissor).
bool ApplyRenderState(const RenderState& rs) {
    // The state of the last draw, applied for this same target (every change of target or
    // of GL state behind this function's back clears g_rsValid; a state whose scissor
    // came out empty is never remembered): nothing to do.
    if (g_rsValid && SameStruct(rs, g_rs)) return true;
    const float sx = CurScaleX(), sy = CurScaleY(); // (inside a pass: the pass's own scale, see BeginPass)
    // Inside an offscreen pass the target is the pass rectangle: same pixels, moved to the origin.
    const int ox = g_inPass ? g_passX : 0, oy = g_inPass ? g_passY : 0;
    const int tw = g_inPass ? g_passW : g_rtW, th = g_inPass ? g_passH : g_rtH;
    int x0 = Rnd(rs.scX * sx) - ox, y0 = Rnd(rs.scY * sy) - oy, x1 = Rnd((rs.scX + rs.scW) * sx) - ox, y1 = Rnd((rs.scY + rs.scH) * sy) - oy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    if (x1 <= x0 || y1 <= y0) return false;
    // A scissor that covers the whole target is no scissor (and on vitaGL an enabled
    // scissor costs one more full-target draw per update than a disabled one).
    if (x0 == 0 && y0 == 0 && x1 == tw && y1 == th) SetScissor(false);
    else SetScissor(true, x0, y0, x1 - x0, y1 - y0);
    SetViewport(Rnd(rs.vpX * sx) - ox, Rnd(rs.vpY * sy) - oy, Rnd(rs.vpW * sx), Rnd(rs.vpH * sy));
    SetDepthRange(rs.zNear, rs.zFar);
    if (rs.blend) SetBlend(rs.blendEq == BE_REVSUB ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, MapBlend(rs.blendSrc), MapBlend(rs.blendDst));
    else SetBlendOff();
    if (rs.depthTest) {
        SetDepthTest(true, kCmp[rs.depthFunc & 7]);
        SetDepthMask(rs.depthWrite != 0);
    } else {
        SetDepthTest(false);
        SetDepthMask(false);
    }
    SetCull(rs.cull);
    SetColorMask(rs.colorWrite != 0, rs.alphaWrite != 0);
    g_rs = rs;
    g_rsValid = true;
    return true;
}

void BindTexture(int unit, const TexBind& b) {
    if (b.texId == 0 || b.texId >= g_tex.size() || g_tex[b.texId].id == 0) {
        BindTex(unit, g_whiteTex);
        return;
    }
    Tex& t = g_tex[b.texId];
    BindTex(unit, t.id);
    static const GLenum kWrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    static const GLenum kMin[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                   GL_LINEAR_MIPMAP_LINEAR};
    uint8_t minF = b.minFilter;
    if (!t.mips && minF >= FILT_NEAR_MIP_NEAR) minF = (minF == FILT_NEAR_MIP_NEAR || minF == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
    if (t.wrapS == b.wrapS && t.wrapT == b.wrapT && t.minF == minF && t.magF == b.magFilter) return;
    if (S.unit != unit) { G(glActiveTexture(GL_TEXTURE0 + unit)); S.unit = unit; }
    if (t.wrapS != b.wrapS) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(kWrap[b.wrapS % 3]))); t.wrapS = b.wrapS; ++C.texParams; }
    if (t.wrapT != b.wrapT) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(kWrap[b.wrapT % 3]))); t.wrapT = b.wrapT; ++C.texParams; }
    if (t.minF != minF) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(kMin[minF % 6]))); t.minF = minF; ++C.texParams; }
    if (t.magF != b.magFilter) {
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, b.magFilter == FILT_NEAR ? GL_NEAREST : GL_LINEAR));
        t.magF = b.magFilter;
        ++C.texParams;
    }
}

// The constant an attribute location supplies while its array is disabled is
// fixed per location. It is (re)established whenever the array is switched
// off, because some GL implementations leave the current value undefined
// after a draw that used the array, and once per frame (Execute), because
// vitaGL keeps it in a ring; not on every draw.
inline void SetAttrib(int loc, bool on, GLint size, GLenum type, GLboolean norm, GLsizei stride, size_t offset, float d0, float d1, float d2,
                      float d3) {
    const uint32_t bit = 1u << loc;
    if (on) {
        if (!(S.attrEnabled & bit)) { G(glEnableVertexAttribArray(static_cast<GLuint>(loc))); S.attrEnabled |= bit; ++C.attrToggles; }
        G(glVertexAttribPointer(static_cast<GLuint>(loc), size, type, norm, stride, reinterpret_cast<const void*>(offset)));
        ++C.attrPointers;
    } else {
        if (S.attrEnabled & bit) { G(glDisableVertexAttribArray(static_cast<GLuint>(loc))); S.attrEnabled &= ~bit; S.attrConstSet &= ~bit; ++C.attrToggles; }
        if (!(S.attrConstSet & bit)) { G(glVertexAttrib4f(static_cast<GLuint>(loc), d0, d1, d2, d3)); S.attrConstSet |= bit; ++C.attrConsts; }
    }
}

// Points the attribute arrays at `vbo` with the decoded-vertex layout
// `layoutMask` starting `base` bytes into it. Nothing is done when they
// already are (consecutive draws from one buffer with one layout).
void SetLayout(GLuint vbo, uint32_t layoutMask, uint32_t base) {
    if (S.layoutVbo == vbo && S.layoutMask == layoutMask && S.layoutBase == base) return;
    ++C.layoutChanges;
    BindArrayBuf(vbo);
    const VtxLayout L = MakeLayout(layoutMask);
    const GLsizei st = L.stride;
    SetAttrib(AT_POS, true, 4, GL_FLOAT, GL_FALSE, st, base, 0, 0, 0, 0);
    SetAttrib(AT_NRM, (L.mask & VL_NRM) != 0, 3, GL_FLOAT, GL_FALSE, st, base + L.offNrm, 0, 0, 1, 0);
    SetAttrib(AT_COL0, (L.mask & VL_COL0) != 0, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, base + L.offCol0, 1, 1, 1, 1);
    SetAttrib(AT_COL1, (L.mask & VL_COL1) != 0, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, base + L.offCol1, 1, 1, 1, 1);
    for (int t = 0; t < 8; ++t)
        SetAttrib(AT_TEX0 + t, (L.mask & (VL_TEX0 << t)) != 0, 2, GL_FLOAT, GL_FALSE, st, base + L.offTex[t], 0, 0, 0, 1);
    SetAttrib(AT_TMI0, (L.mask & VL_TMI) != 0, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, base + L.offTmi, 60, 60, 60, 60);
    SetAttrib(AT_TMI1, (L.mask & VL_TMI) != 0, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, base + L.offTmi + 4, 60, 60, 60, 60);
    // W5 SKIN: bone rows (3 x bone number, whole numbers) and weights; without them weight 0 everywhere (no shader without VL_SKIN reads them)
    if ((L.mask & VL_SKIN) || (S.attrEnabled & ((1u << AT_BIX) | (1u << AT_BW)))) {
        SetAttrib(AT_BIX, (L.mask & VL_SKIN) != 0, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, base + L.offSkin, 0, 0, 0, 0);
        SetAttrib(AT_BW, (L.mask & VL_SKIN) != 0, 4, GL_FLOAT, GL_FALSE, st, base + L.offSkin + 4, 0, 0, 0, 0);
    }
    S.layoutVbo = vbo;
    S.layoutMask = layoutMask;
    S.layoutBase = base;
}

// Program variants (BlendKey / VariantCmd in vgx_cmd.h). vitaGL compares, per draw, the
// blend function and colour mask with what the program was last drawn with and calls the
// shader patcher when they differ (setup_frag_program in custom_shaders.c), and asks its
// vertex program cache for (vertex shader, attributes, streams) on every draw
// (vglCreateVertexProgram). A combination that was never used before is built there and then.
std::unordered_set<uint32_t> g_fragVariants; // vsId << 22 ^ fsId << 12 ^ blend key (the patcher also keys on the vertex shader the fragment program links to)
std::unordered_set<uint32_t> g_vtxVariants;  // vsId << 12 | layout mask
inline void NoteVariant(Program& p, uint16_t vsId, uint16_t fsId, uint32_t layoutMask, uint32_t blendKey) {
    static const bool census = EnvInt("MSC_VGX_PROF", 0) != 0 || std::getenv("MSC_VGX_GLSTATS") != nullptr;
    if (p.usedSerial != g_serial) { p.usedSerial = g_serial; ++g_progsThisFrame; }
    if (p.layoutNeed & ~layoutMask) {
        ++C.constAttrDraws;
        static std::unordered_set<uint32_t> seen;
        static const bool dbg = EnvInt("MSC_VGX_CONSTATTR_LOG", 0) != 0;
        if (dbg && seen.insert((p.layoutNeed << 12) | layoutMask).second) std::fprintf(stderr, "[constattr] shader needs %03x, layout has %03x, missing %03x\n", p.layoutNeed, layoutMask, p.layoutNeed & ~layoutMask);
    }
    if (p.blendKey != blendKey) {
        if (p.blendKey != ~0u) ++C.fragSwitches;
        p.blendKey = blendKey;
        if (census && g_fragVariants.insert((static_cast<uint32_t>(vsId) << 22) ^ (static_cast<uint32_t>(fsId) << 12) ^ blendKey).second) ++C.fragVariants;
    }
    if (p.layout != layoutMask) {
        p.layout = layoutMask;
        if (census && g_vtxVariants.insert((static_cast<uint32_t>(vsId) << 13) | layoutMask).second) ++C.vtxVariants;
    }
}

// CMD_VARIANT: one empty triangle (three times vertex 0 of a buffer of zeros) with the
// program, vertex layout, blend function and colour mask a later draw will use, so that
// the console builds the patched programs for it now. Draws nothing anywhere.
GLuint g_warmVbo = 0, g_warmIbo = 0;
void WarmVariant(const VariantCmd& v) {
    if (!WarmRoom(v.vsId, v.fsId)) return; // W5 SEED: over MSC_VGX_MAXPROGRAMS the warm-up leaves new programs to their first draw
    Program& p = GetProgram(v.vsId, v.fsId);
    if (!p.ok) return;
    if (!g_warmVbo) {
        static const uint8_t zeros[256] = {};
        G(glGenBuffers(1, &g_warmVbo));
        G(glGenBuffers(1, &g_warmIbo));
        BindArrayBuf(g_warmVbo);
        G(glBufferData(GL_ARRAY_BUFFER, sizeof zeros, zeros, GL_STATIC_DRAW));
        BindElemBuf(g_warmIbo);
        G(glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6, zeros, GL_STATIC_DRAW));
    }
    UseProgram(p.id);
    if (v.blendKey & 1u) SetBlend((v.blendKey & 2u) ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, MapBlend((v.blendKey >> 2) & 15u), MapBlend((v.blendKey >> 6) & 15u));
    else SetBlendOff();
    SetColorMask((v.blendKey & (1u << 10)) != 0, (v.blendKey & (1u << 11)) != 0);
    for (int m = 0; m < 8; ++m) BindTex(m, g_whiteTex);
    SetLayout(g_warmVbo, v.layoutMask, 0);
    BindElemBuf(g_warmIbo);
    TouchTarget();
    G(glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr));
    ++C.warmDraws;
    g_rsValid = false; // blend and colour mask no longer are what the last draw's render state says
    NoteVariant(p, v.vsId, v.fsId, v.layoutMask, v.blendKey);
}

// MSC_VGX_GLCACHE=0 (diagnostic): issue GL state the way this file did before it cached it
// (program, textures and attribute pointers for every draw, the whole raster state whenever
// any of it changes), for before/after call counts with MSC_VGX_GLSTATS.
bool g_noCache = false;

void ValidateDraw(const CmdBuf& buf, const DrawRec& r, const Program& p) {
    validate::BufferView view;
    if (r.vbuf) view = g_validation->Find(r.vbuf);
    else {
        view.indices = buf.idx.data(); view.indexCount = buf.idx.size();
        const size_t chunks = buf.vtxChunks.empty() ? 1 : buf.vtxChunks.size();
        if (r.vchunk < chunks) {
            const size_t first = buf.vtxChunks.empty() ? 0 : buf.vtxChunks[r.vchunk];
            const size_t last = r.vchunk + 1u < chunks ? buf.vtxChunks[r.vchunk + 1] : buf.vtx.size();
            if (first <= last && last <= buf.vtx.size() && !buf.vtx.empty()) {
                view.vertices = buf.vtx.data() + first; view.vertexBytes = last - first;
            }
        }
    }
    const validate::UniformRange ranges[] = {
        {UB_PROJ, 0, p.proj != -1 ? 16u : 0u},
        {UB_MV, 0, p.mv != -1 ? static_cast<uint32_t>(p.nMv) * 4u : 0u},
        {UB_TEXM, 0, p.tm != -1 ? static_cast<uint32_t>(p.nTm) * 4u : 0u},
        {UB_TEXM, 96, p.ptm != -1 ? static_cast<uint32_t>(p.nPtm) * 4u : 0u},
        {UB_PAL, 0, p.pal != -1 ? static_cast<uint32_t>(p.nPal) * 4u : 0u},
        {UB_LIGHT, 0, p.lt != -1 ? static_cast<uint32_t>(p.nLt) * 4u : 0u},
        {UB_TEV, 0, p.tev != -1 ? static_cast<uint32_t>(p.nTev) * 4u : 0u}
    };
    const ShaderSource* vs = r.vsId < g_vsSrc.size() ? g_vsSrc[r.vsId] : nullptr;
    const ShaderSource* fs = r.fsId < g_fsSrc.size() ? g_fsSrc[r.fsId] : nullptr;
    const bool positionPalette = vs && vs->usesPalette && !vs->nMv;
    const unsigned clampRow = vs && vs->nPal && vs->nPal != kPalRowsFull ? vs->nPal / 2u - 3u : 61u; // (the shader clamps the row to P - 3: P = half the short palette's rows)
    const bool unavailableStatic = r.vbuf && !view.vertices;
    DrawRec inspected = r;
    if (unavailableStatic) { inspected.idxOffset = 0; inspected.idxCount = 0; }
    const validate::Result result = validate::Inspect(inspected, view, buf.uni.data(), buf.uni.size(), ranges, 7, positionPalette, clampRow);
    ValidationFrame& total = g_validationFrame;
    const uint64_t ordinal = total.draws++;
    total.badDraws += result.errors != 0;
    total.uncheckedDraws += unavailableStatic;
    total.references += result.references;
    total.vertexBytes += result.vertexBytes; total.indexBytes += result.indexBytes; total.uniformBytes += result.uniformBytes;
    const uint64_t words[] = {vs ? vs->keyHash : 0, fs ? fs->keyHash : 0, r.layoutMask, r.prim, r.idxCount, r.vtxCount,
                              result.vertexHash, result.indexHash, result.uniformHash, result.errors, unavailableStatic};
    for (uint64_t word : words) total.hash = validate::HashWord(word, total.hash);
    if (ordinal < 16 || ((result.errors || unavailableStatic) && total.extraLines < 8)) {
        if (ordinal >= 16) ++total.extraLines;
        std::fprintf(stderr, "[validate] frame=%llu draw=%llu source=%s:%u vchunk=%u stride=%u base=%u declared=%u+%u indices=%u+%u minmax=%u:%u buffer_bytes=%llu:%llu "
                             "vs=%016llx fs=%016llx vhash=%016llx ihash=%016llx uhash=%016llx errors=%u unavailable_static=%u bad_layout=%u bad_index_range=%u bad_vertex_refs=%u outside_declared_refs=%u bad_uniform_ranges=%u "
                             "nonfinite_pos_refs=%u nonfinite_attr_refs=%u nonfinite_uniforms=%u nonfinite_blocks=%u,%u,%u,%u,%u,%u bad_palette_refs=%u clamped_palette_refs=%u pos0_bits=%08x,%08x,%08x,%08x\n",
                     static_cast<unsigned long long>(buf.frame), static_cast<unsigned long long>(ordinal), r.vbuf ? "static" : "stream", r.vbuf,
                     r.vchunk, MakeLayout(r.layoutMask).stride, r.vtxOffset, r.vtxFirst, r.vtxCount, r.idxOffset, r.idxCount, result.minIndex, result.maxIndex,
                     static_cast<unsigned long long>(view.vertexBytes), static_cast<unsigned long long>(view.indexCount * sizeof(uint16_t)),
                     static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]), static_cast<unsigned long long>(result.vertexHash),
                     static_cast<unsigned long long>(result.indexHash), static_cast<unsigned long long>(result.uniformHash), result.errors,
                     unavailableStatic ? 1u : 0u, result.badLayout, result.badIndexRange, result.badVertexRefs, result.outsideDeclaredRefs, result.badUniformRanges, result.nonfinitePositionRefs,
                     result.nonfiniteAttributeRefs, result.nonfiniteUniforms, result.nonfiniteByBlock[UB_PROJ], result.nonfiniteByBlock[UB_MV],
                     result.nonfiniteByBlock[UB_TEXM], result.nonfiniteByBlock[UB_PAL], result.nonfiniteByBlock[UB_LIGHT], result.nonfiniteByBlock[UB_TEV],
                     result.badPaletteRefs, result.clampedPaletteRefs,
                     result.firstPosition[0], result.firstPosition[1], result.firstPosition[2], result.firstPosition[3]);
        std::fflush(stderr);
    }
}

// W5 GLB, MSC_VGX_UNIDUP: 0 (default) the uniform blocks a draw names are given to the program when their id differs from the one applied (and at the first
// use in a frame); 1 the same, but the values are compared with what the program holds and the repeats counted (census); 2 the values decide: a block with
// the values the program already holds is not given again (vitaGL keeps them in the program's shadow buffer across frames; only the draws that open a scene
// and Z-freeze draws still give theirs).
const int g_uniDup = static_cast<int>(EnvInt("MSC_VGX_UNIDUP", 0));
constexpr uint32_t kShOff[UB_COUNT] = {0, 16, 40, 232, 616, 792}; // running sums of kUbSize (904 floats)
bool BlockDiffers(Program& p, int b, const float* src, int n0, int off1, int n1) {
    float* sh = p.shadow + kShOff[b];
    const bool diff = !p.shadowOk[b] || std::memcmp(sh, src, static_cast<size_t>(n0) * 4) != 0 || (n1 && std::memcmp(sh + n0, src + off1, static_cast<size_t>(n1) * 4) != 0);
    if (diff) {
        std::memcpy(sh, src, static_cast<size_t>(n0) * 4);
        if (n1) std::memcpy(sh + n0, src + off1, static_cast<size_t>(n1) * 4);
        p.shadowOk[b] = true;
    }
    return diff;
}

// W5 GLB: does draw `b` continue draw `a` (the previous command)? 2: yes, one glDrawElements could do both (same shaders, uniform blocks, textures that are
// sampled, render state, vertex buffer and layout, triangle lists, `b`'s indices follow `a`'s); 1: the same state with other indices; 0: no.
template <class UniVec> int ContinuesDraw(const DrawRec& a, const DrawRec& b, const UniVec& uni) { // (template: GXD made the arena a NoInitAlloc vector)
    if (a.vsId != b.vsId || a.fsId != b.fsId || a.prim != b.prim || a.texMask != b.texMask || a.layoutMask != b.layoutMask || a.vbuf != b.vbuf ||
        a.vtxOffset != b.vtxOffset || a.vchunk != b.vchunk || a.skinBlock != b.skinBlock || a.fx != b.fx)
        return 0;
    if (std::memcmp(&a.rs, &b.rs, sizeof a.rs) != 0) return 0;
    for (int k = 0; k < UB_COUNT; ++k) { // (the blocks by value: the front end gives every draw its own block ids)
        if (a.ublock[k] == b.ublock[k]) continue;
        if (a.ublock[k] == kNoBlock || b.ublock[k] == kNoBlock) return 0;
        if (a.ublock[k] + kUbSize[k] > uni.size() || b.ublock[k] + kUbSize[k] > uni.size()) return 0;
        if (std::memcmp(uni.data() + a.ublock[k], uni.data() + b.ublock[k], kUbSize[k] * sizeof(float)) != 0) return 0;
    }
    for (int m = 0; m < 8; ++m)
        if ((a.texMask & (1u << m)) && std::memcmp(&a.tex[m], &b.tex[m], sizeof a.tex[m]) != 0) return 0;
    return (a.prim == PRIM_TRIANGLES && b.idxOffset == a.idxOffset + a.idxCount) ? 2 : 1;
}

// W5 GLB: what differs between consecutive draws that use the same program (the pair census: why mergeable stays small).
template <class UniVec> void PairCensus(const DrawRec& a, const DrawRec& b, const UniVec& uni) {
    ++C.pairs;
    if (a.vsId != b.vsId || a.fsId != b.fsId) return;
    ++C.pairsSameProg;
    if (a.prim != b.prim || a.texMask != b.texMask || a.layoutMask != b.layoutMask || a.vbuf != b.vbuf || a.vtxOffset != b.vtxOffset || a.vchunk != b.vchunk) ++C.pairsDiffStatic;
    if (std::memcmp(&a.rs, &b.rs, sizeof a.rs) != 0) ++C.pairsDiffRs;
    for (int m = 0; m < 8; ++m)
        if ((a.texMask & (1u << m)) && std::memcmp(&a.tex[m], &b.tex[m], sizeof a.tex[m]) != 0) { ++C.pairsDiffTex; break; }
    uint32_t* const cu[UB_COUNT] = {&C.pairsDiffU0, &C.pairsDiffU1, &C.pairsDiffU2, &C.pairsDiffU3, &C.pairsDiffU4, &C.pairsDiffU5};
    for (int k = 0; k < UB_COUNT; ++k) {
        if (a.ublock[k] == b.ublock[k]) continue;
        if (a.ublock[k] == kNoBlock || b.ublock[k] == kNoBlock) { ++*cu[k]; continue; }
        if (a.ublock[k] + kUbSize[k] > uni.size() || b.ublock[k] + kUbSize[k] > uni.size() ||
            std::memcmp(uni.data() + a.ublock[k], uni.data() + b.ublock[k], kUbSize[k] * sizeof(float)) != 0) ++*cu[k];
    }
}

void Draw(const CmdBuf& buf, const DrawRec& r) {
    if (g_skipDraws) return;
    Program& p = GetProgram(r.vsId, r.fsId);
    if (!p.ok || r.idxCount == 0) return;
    if (g_validateFrame) ValidateDraw(buf, r, p);
    if (g_noCache) {
        S.program = ~0u;
        S.layoutVbo = ~0u;
        S.unit = -1;
        for (GLuint& t : S.tex) t = ~0u;
        if (!(g_rsValid && std::memcmp(&r.rs, &g_rs, sizeof r.rs) == 0)) {
            S.scissorOn = S.blendOn = S.depthTest = S.depthMask = S.cullOn = S.colorMask = -1;
            S.sc[2] = S.vp[2] = -1;
            S.zn = -1;
            S.blendEq = S.blendSrc = S.depthFunc = S.cullFace = -1;
        }
    }
    GLuint vbo, ibo;
    if (r.vbuf) {
        if (r.vbuf >= g_sbuf.size() || !g_sbuf[r.vbuf].vbo) return;
        vbo = g_sbuf[r.vbuf].vbo;
        ibo = g_sbuf[r.vbuf].ibo;
    } else {
        if (r.vchunk >= g_streamVbo.size()) return;
        vbo = g_streamVbo[r.vchunk];
        ibo = g_ibo;
    }
    if (!ApplyRenderState(r.rs)) return;
    // The first draw of a scene gives its uniform blocks again when it continues with the
    // program of the last draw before the render-target change: vitaGL would otherwise keep
    // using the buffers it handed to sceGxm in the previous scene (it re-reserves them only
    // after glUseProgram / glUniform*, custom_shaders.c upload_uniforms; gxm.c scene_reset
    // restores the viewport and the region clip, nothing else).
    const bool sceneStart = (S.fbo != g_inUseFbo || g_fbDirty) && S.program == p.id;
    const bool progSwitched = S.program != p.id;
    const uint32_t uniCalls0 = C.uniformCalls;
    UseProgram(p.id);
    const bool fresh = p.serial != g_serial || sceneStart;
    if (sceneStart) ++C.sceneReuploads;
    const float* u = buf.uni.data();
    // n0 floats at the block's start, then n1 floats at off1 (the texture matrices and the post matrices share a block)
    auto need = [&](int b, int n0, int off1 = 0, int n1 = 0) {
        if (r.ublock[b] == kNoBlock) return false;
        const bool old = fresh || p.applied[b] != r.ublock[b];
        if (g_uniDup == 0 || g_noCache) return old;
        const float* src = u + r.ublock[b];
        if (g_uniDup == 1) {
            if (old && !BlockDiffers(p, b, src, n0, off1, n1)) { ++C.uniDupCalls; C.uniDupBytes += static_cast<uint32_t>(n0 + n1) * 4u; }
            return old;
        }
        if (!sceneStart && p.serial == g_serial && p.applied[b] == r.ublock[b]) return false;
        if (sceneStart) p.shadowOk[b] = false;
        return BlockDiffers(p, b, src, n0, off1, n1);
    };
    auto up4 = [&](GLint loc, GLint n, const float* v) {
        G(glUniform4fv(loc, n, v));
        ++C.uniformCalls;
        C.uniformBytes += static_cast<uint32_t>(n) * 16u;
    };
    if (p.proj != -1 && need(UB_PROJ, 16)) {
        G(glUniformMatrix4fv(p.proj, 1, GL_FALSE, u + r.ublock[UB_PROJ]));
        ++C.uniformCalls;
        C.uniformBytes += 64;
        g_uniDirtyV = true;
    }
    if (p.mv != -1 && need(UB_MV, p.nMv * 4)) { up4(p.mv, p.nMv, u + r.ublock[UB_MV]); g_uniDirtyV = true; }
    if ((p.tm != -1 || p.ptm != -1) && need(UB_TEXM, p.tm != -1 ? p.nTm * 4 : 0, 96, p.ptm != -1 ? p.nPtm * 4 : 0)) {
        if (p.tm != -1) { up4(p.tm, p.nTm, u + r.ublock[UB_TEXM]); g_uniDirtyV = true; }
        if (p.ptm != -1) { up4(p.ptm, p.nPtm, u + r.ublock[UB_TEXM] + 96); g_uniDirtyV = true; }
    }
    if (p.pal != -1 && need(UB_PAL, p.nPal * 4)) {
        up4(p.pal, p.nPal, u + r.ublock[UB_PAL]);
        ++C.palCalls;
        C.palBytes += static_cast<uint32_t>(p.nPal) * 16u;
        g_uniDirtyV = true;
    }
    if (p.lt != -1 && need(UB_LIGHT, p.nLt * 4)) { up4(p.lt, p.nLt, u + r.ublock[UB_LIGHT]); g_uniDirtyV = true; }
    const ShaderSource* const fsSrc = r.fsId < g_fsSrc.size() ? g_fsSrc[r.fsId] : nullptr;
    if (fsSrc && fsSrc->zfVec && p.tev != -1 && r.ublock[UB_TEV] != kNoBlock) {
        // Z-freeze (vgx_zfreeze.h): the block holds the reference plane in EFB pixels; the shader evaluates it at gl_FragCoord, which
        // is in pixels of the render target (or of the pass target, whose origin is the pass rectangle's: see ApplyRenderState).
        // Always uploaded: the same block is a different plane inside and outside a pass.
        float tmp[112];
        const int n = p.nTev * 4 < 112 ? p.nTev * 4 : 112;
        std::memcpy(tmp, u + r.ublock[UB_TEV], static_cast<size_t>(n) * sizeof(float));
        const float sx = CurScaleX(), sy = CurScaleY();
        const float ox = g_inPass ? static_cast<float>(g_passX) : 0.0f, oy = g_inPass ? static_cast<float>(g_passY) : 0.0f;
        float* pl = tmp + fsSrc->zfVec * 4;
        pl[2] += pl[0] * ox / sx + pl[1] * oy / sy;
        pl[0] /= sx;
        pl[1] /= sy;
        up4(p.tev, p.nTev, tmp);
        g_uniDirtyF = true;
        p.shadowOk[UB_TEV] = false;
    } else if (p.tev != -1 && need(UB_TEV, p.nTev * 4)) { up4(p.tev, p.nTev, u + r.ublock[UB_TEV]); g_uniDirtyF = true; }
    if (p.bone != -1 && r.skinBlock != kNoBlock && r.skinBlock + static_cast<uint32_t>(p.nBone) * 4u <= buf.uni.size()) { // W5 SKIN: the bone matrices
        bool give = fresh || p.appliedSkin != r.skinBlock;
        if (g_uniDup != 0 && !g_noCache) { // (as need() above: UNIDUP=2 lets the values decide; 1 counts)
            const float* src = u + r.skinBlock;
            const size_t n = static_cast<size_t>(p.nBone) * 4u;
            if (p.skinShadow.size() != n) { p.skinShadow.assign(n, 0.0f); p.skinShadowOk = false; }
            if (sceneStart) p.skinShadowOk = false;
            const bool differs = !p.skinShadowOk || std::memcmp(p.skinShadow.data(), src, n * 4) != 0;
            if (g_uniDup == 2 && !(sceneStart || p.serial != g_serial || p.appliedSkin != r.skinBlock)) give = false;
            else if (g_uniDup == 2) give = differs;
            else if (give && !differs) { ++C.uniDupCalls; C.uniDupBytes += static_cast<uint32_t>(n) * 4u; }
            if (differs) { std::memcpy(p.skinShadow.data(), src, n * 4); p.skinShadowOk = true; }
        }
        if (give) { up4(p.bone, p.nBone, u + r.skinBlock); g_uniDirtyV = true; }
        p.appliedSkin = r.skinBlock;
    }
    std::memcpy(p.applied, r.ublock, sizeof p.applied);
    p.serial = g_serial;
    if (progSwitched && C.uniformCalls == uniCalls0) ++C.progSwitchOnly;
    if (g_uniDirtyV) { C.ringBytes += p.vsBytes; g_uniDirtyV = false; }
    if (g_uniDirtyF) { C.ringBytes += p.fsBytes; g_uniDirtyF = false; }

    for (int m = 0; m < 8; ++m)
        if (r.texMask & (1u << m)) BindTexture(m, r.tex[m]);

    SetLayout(vbo, r.layoutMask, r.vtxOffset);
    BindElemBuf(ibo);

    GLenum mode = GL_TRIANGLES;
    if (r.prim == PRIM_LINES) {
        mode = GL_LINES;
        const float lw = r.rs.lineWidth6 / 6.0f < 1.0f ? 1.0f : r.rs.lineWidth6 / 6.0f;
        if (S.lineWidth != lw) { G(glLineWidth(lw)); ++C.stateCalls; S.lineWidth = lw; }
    } else if (r.prim == PRIM_POINTS) {
        mode = GL_POINTS;
    }
    TouchTarget();
    RtNoteDraw(r.rs.depthTest != 0);
    G(glDrawElements(mode, static_cast<GLsizei>(r.idxCount), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(static_cast<size_t>(r.idxOffset) * 2)));
    ++C.draws;
    C.drawIndices += r.idxCount;
    ++g_stats.drawCallsThisFrame;
    NoteVariant(p, r.vsId, r.fsId, r.layoutMask, BlendKey(r.rs));
}

Tex& TexSlotFor(uint32_t id) {
    if (id >= g_tex.size()) g_tex.resize(id + 64);
    return g_tex[id];
}

// A texture about to be deleted is detached from every pooled framebuffer object first: vitaGL only frees a texture that no
// framebuffer holds (one left attached stays allocated until that object is trimmed), and a reused name of the same size would
// otherwise be taken for "already attached" by AcquireFbo while vitaGL has dropped it (a NULL colour surface at the next scene).
void DetachFromPool(GLuint name) {
    const GLuint prev = S.fbo;
    bool moved = false;
    for (PoolFbo& f : g_pool)
        if (f.attached == name) {
            // (never the target of the open scene: vitaGL ends that scene later, with its target; the texture stays alive until then)
            if (f.fbo != g_inUseFbo) {
                BindFbo(f.fbo);
                AttachColor(0);
                moved = true;
            }
            f.attached = 0;
        }
    if (moved && prev != ~0u) BindFbo(prev);
}
bool BoundTexHasStorage() { return !vgx_tex_has_storage || vgx_tex_has_storage() != 0; }

void DeleteTex(Tex& t) {
    if (!t.id) return;
    DetachFromPool(t.id);
    G(glDeleteTextures(1, &t.id));
    ++C.texDeletes;
    ForgetTex(t.id);
    t = Tex{};
}

// DXT1 -> RGBA8, for a driver without S3TC (the desktop harness's fallback).
void DecodeDxt1(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, src += 8) {
            const uint32_t c0 = src[0] | (src[1] << 8), c1 = src[2] | (src[3] << 8);
            uint8_t pal[4][4];
            const auto dec = [](uint32_t v, uint8_t* o) {
                const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
                o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
            };
            dec(c0, pal[0]);
            dec(c1, pal[1]);
            for (int k = 0; k < 3; ++k) {
                if (c0 > c1) {
                    pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k] + 1) / 3);
                    pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k] + 1) / 3);
                } else {
                    pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
                    pal[3][k] = 0;
                }
            }
            pal[2][3] = 255;
            pal[3][3] = c0 > c1 ? 255 : 0;
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px >= w || py >= h) continue;
                    std::memcpy(&out[(static_cast<size_t>(py) * w + px) * 4], pal[(src[4 + y] >> (x * 2)) & 3], 4);
                }
        }
}

// (from the Galaxy port)
void DecodeDxt5(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> colour(static_cast<size_t>(bw) * bh * 8);
    for (size_t b = 0; b < static_cast<size_t>(bw) * bh; ++b) {
        std::memcpy(&colour[b * 8], src + b * 16 + 8, 8);
        // (a DXT5 colour block is always read in four-colour mode)
        uint16_t c0, c1;
        std::memcpy(&c0, &colour[b * 8], 2);
        std::memcpy(&c1, &colour[b * 8 + 2], 2);
        if (c0 <= c1) {
            // swap the end points and remap the indices so DXT1 decoding picks the same colours
            std::memcpy(&colour[b * 8], &c1, 2);
            std::memcpy(&colour[b * 8 + 2], &c0, 2);
            for (int i = 4; i < 8; ++i) {
                uint8_t v = colour[b * 8 + i], r = 0;
                for (int k = 0; k < 4; ++k) {
                    static const uint8_t kMap[4] = {1, 0, 3, 2};
                    r |= static_cast<uint8_t>(kMap[(v >> (2 * k)) & 3] << (2 * k));
                }
                colour[b * 8 + i] = r;
            }
            if (c0 == c1) for (int i = 4; i < 8; ++i) colour[b * 8 + i] = 0;
        }
    }
    DecodeDxt1(colour.data(), w, h, out);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* a = src + (static_cast<size_t>(by) * bw + bx) * 16;
            uint8_t pal[8] = {a[0], a[1]};
            if (a[0] > a[1]) for (int i = 1; i < 7; ++i) pal[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
            else { for (int i = 1; i < 5; ++i) pal[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5); pal[6] = 0; pal[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(a[2 + i]) << (8 * i);
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) out[(static_cast<size_t>(py) * w + px) * 4 + 3] = pal[(bits >> (3 * (y * 4 + x))) & 7];
                }
        }
}

void Upload(const TexUpload& up) {
    Tex& t = TexSlotFor(up.texId);
    if (t.renderTarget && t.id) DeleteTex(t);
    if (!t.id) { G(glGenTextures(1, &t.id)); ++g_stats.textures; }
    BindTexForEdit(t.id);
#if !defined(__VGX_VITA__)
    // LA88 has odd-width two-byte rows. Restore the caller's alignment for this opt-in path.
    // vitaGL copies tightly packed input and rejects GL_UNPACK_ALIGNMENT (textures.c).
    GLint unpackAlignment = 1;
    if (up.format == TF_LA88) G(glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment));
    G(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
#endif
    uint32_t w = up.width, h = up.height;
    bool reserveMipChain = false;
#if defined(__VGX_VITA__)
    // vitaGL grows compressed storage to the highest supplied level. Upload it
    // after level zero so intermediate levels reuse that allocation.
    if (((up.format == TF_DXT1 && g_hasS3tc) || (up.format == TF_DXT5 && g_hasDxt5)) && !up.genMips && up.levels.size() > 2 && up.levels.size() < 32 &&
        w && h && !(w & (w - 1)) && !(h & (h - 1))) {
        const size_t last = up.levels.size() - 1;
        reserveMipChain = (w >> last) >= 4 && (h >> last) >= 4;
    }
#endif
    std::vector<uint8_t> tmp;
    for (size_t i = 0; i < up.levels.size(); ++i) {
        const size_t l = reserveMipChain ? (i == 1 ? up.levels.size() - 1 : i > 1 ? i - 1 : 0) : i;
        if (reserveMipChain) { w = up.width >> l; h = up.height >> l; }
        const std::vector<uint8_t>& lv = up.levels[l];
        if ((up.format == TF_DXT1 && g_hasS3tc) || (up.format == TF_DXT5 && g_hasDxt5)) {
            G(glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l),
                                     up.format == TF_DXT5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, static_cast<GLsizei>(w),
                                     static_cast<GLsizei>(h), 0, static_cast<GLsizei>(lv.size()), lv.data()));
            g_stats.textureBytes += lv.size();
            C.texUploadBytes += static_cast<uint32_t>(lv.size());
        } else {
            const uint8_t* px = lv.data();
            if (up.format == TF_DXT1) { DecodeDxt1(lv.data(), w, h, tmp); px = tmp.data(); }
            if (up.format == TF_DXT5) { DecodeDxt5(lv.data(), w, h, tmp); px = tmp.data(); }
            const GLenum format = up.format == TF_LA88 ? GL_LUMINANCE_ALPHA : GL_RGBA;
            const uint64_t bytes = static_cast<uint64_t>(w) * h * (up.format == TF_LA88 ? 2u : 4u);
            G(glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), format, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, format,
                           GL_UNSIGNED_BYTE, px));
            g_stats.textureBytes += bytes;
            C.texUploadBytes += static_cast<uint32_t>(bytes);
        }
        ++C.texUploads;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
#if !defined(__VGX_VITA__)
    if (up.format == TF_LA88) G(glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment));
#endif
    if (up.genMips) {
        G(glGenerateMipmap(GL_TEXTURE_2D));
        ++C.mipGens;
    }
#if !defined(__VGX_VITA__) && !defined(VGX_GL_STUB)
    // Only the authored levels were sent (Caps::mipClamp): a desktop GL needs to be told
    // where the chain ends (sceGxm takes the level count with the texture). After the loop
    // w x h is the size of the level that would follow the last one given.
    if (!up.genMips && up.levels.size() > 1) {
        uint32_t lw = up.width, lh = up.height;
        for (size_t l = 1; l < up.levels.size(); ++l) { lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
        if (lw > 1 || lh > 1) {
            if (g_hasMaxLevel) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(up.levels.size() - 1))); ++C.texParams; }
            else VGX_LOG_N(4, "texture with an incomplete mip chain and no GL_TEXTURE_MAX_LEVEL on this GL (MSC_VGX_MIPCLAMP needs ES 3): it will not sample\n");
        }
    }
#endif
    t.w = up.width;
    t.h = up.height;
    t.mips = up.levels.size() > 1 || up.genMips;
    t.renderTarget = false;
    t.wrapS = t.wrapT = t.minF = t.magF = 0xFF;
}

void EnsureCopyProgram() {
    if (g_copyProg) return;
    static unsigned failures = 0;
    static uint64_t retryAfter = 0;
    if (failures >= 4 || g_serial < retryAfter) return;
    retryAfter = g_serial + (60ull << failures++);
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = u_rect.xy + (a_pos.xy * 0.5 + 0.5) * u_rect.zw; gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    static const char* fs =
        "precision mediump float;\nuniform sampler2D u_src;\nuniform mat4 u_mat;\nuniform vec4 u_add;\nvarying vec2 v_uv;\n"
        "void main() { gl_FragColor = u_mat * texture2D(u_src, v_uv) + u_add; }\n";
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "copy vertex"), f = Compile(GL_FRAGMENT_SHADER, fs, "copy fragment");
    if (!v || !f) {
        if (v) G(glDeleteShader(v));
        if (f) G(glDeleteShader(f));
        return;
    }
    g_copyProg = G(glCreateProgram());
    if (!g_copyProg || g_copyProg == 0xFFFFFFFFu) {
        g_copyProg = 0;
        G(glDeleteShader(v)); G(glDeleteShader(f));
        return;
    }
    G(glAttachShader(g_copyProg, v));
    G(glAttachShader(g_copyProg, f));
    G(glBindAttribLocation(g_copyProg, AT_POS, "a_pos"));
    G(glLinkProgram(g_copyProg));
    G(glDeleteShader(v)); G(glDeleteShader(f));
    GLint ok = 0;
    G(glGetProgramiv(g_copyProg, GL_LINK_STATUS, &ok));
    if (!ok) { G(glDeleteProgram(g_copyProg)); g_copyProg = 0; return; }
    UseProgram(g_copyProg);
    g_copyRect = G(glGetUniformLocation(g_copyProg, "u_rect"));
    g_copyMat = G(glGetUniformLocation(g_copyProg, "u_mat"));
    g_copyAdd = G(glGetUniformLocation(g_copyProg, "u_add"));
    G(glUniform1i(glGetUniformLocation(g_copyProg, "u_src"), 0));
    failures = 0;
}

// Draws the copy program's quad over the whole viewport: out = m * texel + add.
// Raster state (target, viewport, scissor, masks) is the caller's.
void DrawQuad(GLuint srcTex, float rx, float ry, float rw, float rh, const float* m16, const float* add4) {
    EnsureCopyProgram();
    if (!g_copyProg) return;
    UseProgram(g_copyProg);
    BindTex(0, srcTex);
    G(glUniform4f(g_copyRect, rx, ry, rw, rh));
    G(glUniformMatrix4fv(g_copyMat, 1, GL_FALSE, m16));
    G(glUniform4f(g_copyAdd, add4[0], add4[1], add4[2], add4[3]));
    C.uniformCalls += 3;
    C.uniformBytes += 96;
    C.ringBytes += 16 + 80;
    g_uniDirtyV = g_uniDirtyF = false;
    // Position only, from the quad's own buffer (2 floats per vertex).
    if (S.layoutVbo != g_quadVbo) {
        ++C.layoutChanges;
        BindArrayBuf(g_quadVbo);
        for (int a = 1; a < AT_COUNT; ++a)
            if (S.attrEnabled & (1u << a)) {
                G(glDisableVertexAttribArray(static_cast<GLuint>(a)));
                S.attrEnabled &= ~(1u << a);
                S.attrConstSet &= ~(1u << a);
                ++C.attrToggles;
            }
        if (!(S.attrEnabled & 1u)) { G(glEnableVertexAttribArray(AT_POS)); S.attrEnabled |= 1u; ++C.attrToggles; }
        G(glVertexAttribPointer(AT_POS, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        ++C.attrPointers;
        S.layoutVbo = g_quadVbo;
        S.layoutMask = 0;
        S.layoutBase = 0;
    }
    TouchTarget(OP_COPY);
    ++g_rt.cur.copies;
    G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
}

// ---------------------------------------------------------------- picture scaling (LOOK, wave 5)
// DrawPicture (vgx_cmd.h): the display copy to the screen through one of three fragment
// programs, made when first asked for. The vertex program is the copy program's: a position-only
// quad whose uv rectangle (u_rect: u0, v at the quad's bottom edge, du, dv; dv < 0 flips) is
// stretched over the viewport. u_size is the EFB texture's (w, h, 1/w, 1/h), u_lim the first and
// last texel centre of the picture (the bilinear fetch must not read outside the display copy
// rectangle), u_p (dst pixels per source texel x, y, unsharp amount).
struct PicProg {
    GLuint prog = 0;
    bool failed = false;   // compile or link failed (no shader compiler on the console): the host falls back to its blit
    GLint rect = -1, size = -1, lim = -1, p = -1;
};
PicProg g_pic[4]; // PIC_LINEAR, PIC_SHARP, PIC_UNSHARP, PIC_SMOOTH
int g_screenW = 0, g_screenH = 0, g_screenMode = 0;
float g_screenSharpen = 0.35f;
GLuint g_screenFbo = 0, g_screenTex = 0;
int g_screenTexW = 0, g_screenTexH = 0;

PicProg* EnsurePictureProgram(int mode) {
    PicProg& pp = g_pic[mode - PIC_LINEAR];
    if (pp.prog) return &pp;
    if (pp.failed) return nullptr;
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = vec2(u_rect.x + (a_pos.x * 0.5 + 0.5) * u_rect.z, u_rect.y + (a_pos.y * 0.5 + 0.5) * u_rect.w); gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    std::string fs =
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_size;\nuniform vec4 u_lim;\nuniform vec4 u_p;\nvarying vec2 v_uv;\nvoid main() {\n";
    if (mode == PIC_SHARP || mode == PIC_UNSHARP)
        fs += "  vec2 t = v_uv * u_size.xy;\n  vec2 fl = floor(t);\n  vec2 cd = (t - fl) - 0.5;\n  vec2 range = 0.5 - 0.5 / u_p.xy;\n"
              "  vec2 g = (cd - clamp(cd, -range, range)) * u_p.xy + 0.5;\n  vec2 uv = clamp((fl + g) * u_size.zw, u_lim.xy, u_lim.zw);\n";
    else
        fs += "  vec2 uv = clamp(v_uv, u_lim.xy, u_lim.zw);\n";
    fs += "  vec4 c = texture2D(u_src, uv);\n";
    if (mode == PIC_UNSHARP || mode == PIC_SMOOTH)
        // (1.1: one sample per statement. The console's shader compiler stopped with "fatal internal error" on the nested
        // single-expression form, so the sharpening was never on there.)
        fs += "  vec2 dx = vec2(u_size.z, 0.0);\n  vec2 dy = vec2(0.0, u_size.w);\n"
              "  vec2 u1 = clamp(uv + dx, u_lim.xy, u_lim.zw);\n  vec2 u2 = clamp(uv - dx, u_lim.xy, u_lim.zw);\n"
              "  vec2 u3 = clamp(uv + dy, u_lim.xy, u_lim.zw);\n  vec2 u4 = clamp(uv - dy, u_lim.xy, u_lim.zw);\n"
              "  vec4 s1 = texture2D(u_src, u1);\n  vec4 s2 = texture2D(u_src, u2);\n  vec4 s3 = texture2D(u_src, u3);\n  vec4 s4 = texture2D(u_src, u4);\n"
              "  vec3 b = (s1.rgb + s2.rgb + s3.rgb + s4.rgb) * 0.25;\n";
    if (mode == PIC_SMOOTH)
        // 1.1: edge smoothing in the same pass (u_p.w: amount, 0 = off), from the four neighbours it already reads. Where the
        // picture has a contrast edge (a character's outline against the pitch), the pixel is taken a fraction of a texel across
        // the edge (FXAA's sub-pixel step, without its search along the edge), which softens the stair steps of the upscaled
        // picture; there the unsharp mask is mostly left out, as it would sharpen the steps again. One more texture read.
        fs += "  vec3 lw = vec3(0.299, 0.587, 0.114);\n"
              "  float lC = dot(c.rgb, lw);\n  float lE = dot(s1.rgb, lw);\n  float lW = dot(s2.rgb, lw);\n"
              "  float lS = dot(s3.rgb, lw);\n  float lN = dot(s4.rgb, lw);\n"
              "  float lMax = max(max(max(lE, lW), max(lS, lN)), lC);\n  float lMin = min(min(min(lE, lW), min(lS, lN)), lC);\n"
              "  float range = lMax - lMin;\n"
              "  float on = step(max(0.06, 0.15 * lMax), range) * u_p.w;\n"
              "  float sub = clamp(abs((lE + lW + lS + lN) * 0.25 - lC) / max(range, 0.001), 0.0, 1.0);\n"
              "  float sm = sub * sub * (3.0 - 2.0 * sub);\n"
              "  float blend = sm * sm * 0.75 * on;\n"
              "  float horz = step(abs(lE + lW - 2.0 * lC), abs(lN + lS - 2.0 * lC));\n"
              "  float sy = step(abs(lN - lC), abs(lS - lC)) * 2.0 - 1.0;\n"
              "  float sx = step(abs(lW - lC), abs(lE - lC)) * 2.0 - 1.0;\n"
              "  vec2 nrm = vec2((1.0 - horz) * sx * u_size.z, horz * sy * u_size.w);\n"
              "  vec2 ua = clamp(uv + nrm * blend, u_lim.xy, u_lim.zw);\n"
              "  vec4 a = texture2D(u_src, ua);\n"
              "  float keep = 1.0 - 0.7 * on;\n"
              "  c.rgb = a.rgb + (c.rgb - b) * (u_p.z * keep);\n";
    else if (mode == PIC_UNSHARP)
        fs += "  c.rgb = c.rgb + (c.rgb - b) * u_p.z;\n";
    fs += "  gl_FragColor = vec4(c.rgb, 1.0);\n}\n";
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "picture vertex"), f = Compile(GL_FRAGMENT_SHADER, fs.c_str(), "picture fragment");
    if (!v || !f) {
        if (v) G(glDeleteShader(v));
        if (f) G(glDeleteShader(f));
        pp.failed = true;
        return nullptr;
    }
    pp.prog = G(glCreateProgram());
    if (!pp.prog || pp.prog == 0xFFFFFFFFu) {
        pp.prog = 0; pp.failed = true;
        G(glDeleteShader(v)); G(glDeleteShader(f));
        return nullptr;
    }
    G(glAttachShader(pp.prog, v));
    G(glAttachShader(pp.prog, f));
    G(glBindAttribLocation(pp.prog, AT_POS, "a_pos"));
    G(glLinkProgram(pp.prog));
    GLint vok = 0, fok = 0, lok = 0;   // an optional shader: no compiler (libshacccg missing) must not break the picture
    G(glGetShaderiv(v, GL_COMPILE_STATUS, &vok));
    G(glGetShaderiv(f, GL_COMPILE_STATUS, &fok));
    G(glGetProgramiv(pp.prog, GL_LINK_STATUS, &lok));
    G(glDeleteShader(v)); G(glDeleteShader(f));
    if (!vok || !fok || !lok) {
        VGX_ERR("picture shader (mode %d) unavailable: the present falls back to the plain blit\n", mode);
        G(glDeleteProgram(pp.prog));
        pp.prog = 0;
        pp.failed = true;
        return nullptr;
    }
    UseProgram(pp.prog);
    pp.rect = G(glGetUniformLocation(pp.prog, "u_rect"));
    pp.size = G(glGetUniformLocation(pp.prog, "u_size"));
    pp.lim = G(glGetUniformLocation(pp.prog, "u_lim"));
    pp.p = G(glGetUniformLocation(pp.prog, "u_p"));
    G(glUniform1i(glGetUniformLocation(pp.prog, "u_src"), 0));
    return &pp;
}

} // namespace (closed to define the public names in order; reopened below)
int PictureModeFromString(const char* text) {
    if (!text || !*text) return -1;
    auto is = [&](const char* a) { return std::strcmp(text, a) == 0; };
    if (is("off") || is("0") || is("blit")) return PIC_BLIT;
    if (is("linear") || is("bilinear") || is("1")) return PIC_LINEAR;
    if (is("sharp") || is("2")) return PIC_SHARP;
    if (is("sharp+") || is("unsharp") || is("sharp+unsharp") || is("3")) return PIC_UNSHARP;
    if (is("smooth") || is("smooth+") || is("4")) return PIC_SMOOTH;
    return -1;
}
namespace {

void UsePictureQuadLayout() { // as DrawQuad's: the quad's own position-only buffer
    if (S.layoutVbo == g_quadVbo) return;
    ++C.layoutChanges;
    BindArrayBuf(g_quadVbo);
    for (int a = 1; a < AT_COUNT; ++a)
        if (S.attrEnabled & (1u << a)) {
            G(glDisableVertexAttribArray(static_cast<GLuint>(a)));
            S.attrEnabled &= ~(1u << a);
            S.attrConstSet &= ~(1u << a);
            ++C.attrToggles;
        }
    if (!(S.attrEnabled & 1u)) { G(glEnableVertexAttribArray(AT_POS)); S.attrEnabled |= 1u; ++C.attrToggles; }
    G(glVertexAttribPointer(AT_POS, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
    ++C.attrPointers;
    S.layoutVbo = g_quadVbo;
    S.layoutMask = 0;
    S.layoutBase = 0;
}

} // namespace
bool DrawPicture(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, int mode, float sharpen, bool flipY) {
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return true;
    if (mode < PIC_LINEAR) mode = PIC_LINEAR;
    if (mode > PIC_SMOOTH) mode = PIC_SMOOTH;
    PicProg* ppp = EnsurePictureProgram(mode);
    if (!ppp) return false;   // nothing of the GL state has been touched yet
    PicProg& pp = *ppp;
    BindFbo(fbo);
    SetViewport(dx, dy, dw, dh);
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    UseProgram(pp.prog);
    BindTex(0, g_efbColor);
    const float tw = static_cast<float>(g_rtW), th = static_cast<float>(g_rtH);
    const float u0 = static_cast<float>(x) / tw, du = static_cast<float>(w) / tw;
    const float vTop = static_cast<float>(y) / th, dv = static_cast<float>(h) / th;
    // flipY: the quad's bottom edge shows the picture's last row.
    if (flipY) G(glUniform4f(pp.rect, u0, vTop + dv, du, -dv));
    else G(glUniform4f(pp.rect, u0, vTop, du, dv));
    G(glUniform4f(pp.size, tw, th, 1.0f / tw, 1.0f / th));
    G(glUniform4f(pp.lim, (static_cast<float>(x) + 0.5f) / tw, (static_cast<float>(y) + 0.5f) / th, (static_cast<float>(x + w) - 0.5f) / tw,
                  (static_cast<float>(y + h) - 0.5f) / th));
    static const float edge = [] {   // MSC_VITA_EDGE_AA=0: the smooth picture without the edge smoothing (1.1; 0..1, default 1)
        const char* e = std::getenv("MSC_VITA_EDGE_AA");
        const float v = (e && *e) ? static_cast<float>(std::atof(e)) : 1.0f;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }();
    G(glUniform4f(pp.p, static_cast<float>(dw) / static_cast<float>(w), static_cast<float>(dh) / static_cast<float>(h), sharpen, mode == PIC_SMOOTH ? edge : 0.0f));
    C.uniformCalls += 4;
    C.uniformBytes += 64;
    g_uniDirtyV = g_uniDirtyF = false;
    UsePictureQuadLayout();
    TouchTarget();
    G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    return true;
}
void SetScreenCapture(int w, int h, int mode, float sharpen) {
    g_screenW = w > 0 && h > 0 ? w : 0;
    g_screenH = g_screenW ? h : 0;
    g_screenMode = mode;
    g_screenSharpen = sharpen;
}
int g_screenExtra[4];
int g_screenExtraN = 0;
std::vector<uint8_t> g_screenExtraPx[4];
void SetScreenCaptureExtra(const int* modes, int count) {
    g_screenExtraN = count < 0 ? 0 : (count > 4 ? 4 : count);
    for (int i = 0; i < g_screenExtraN; ++i) g_screenExtra[i] = modes[i];
}
const std::vector<uint8_t>& LastScreenVariant(int index, int& width, int& height) {
    static const std::vector<uint8_t> none;
    if (index < 0 || index >= g_screenExtraN) {
        width = height = 0;
        return none;
    }
    width = g_screenW;
    height = g_screenH;
    return g_screenExtraPx[index];
}
namespace {

// MSC_VGX_SCREEN: the screen-size read-back target (made on the first capture).
void EnsureScreenTarget() {
    if (g_screenFbo && g_screenTexW == g_screenW && g_screenTexH == g_screenH) return;
    if (!g_screenFbo) {
        G(glGenFramebuffers(1, &g_screenFbo));
        G(glGenTextures(1, &g_screenTex));
    }
    BindTexForEdit(g_screenTex);
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_screenW, g_screenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    SetupNewTexture(GL_NEAREST);
    BindFbo(g_screenFbo);
    AttachColor(g_screenTex);
    g_screenTexW = g_screenW;
    g_screenTexH = g_screenH;
}

// The progress bar (SetProgressDisplay in vgx_cmd.h): a dark screen with a framed bar in its
// lower third, drawn with the copy program (a quad of one colour is "0 * texel + colour" over
// a viewport of the bar's size), then shown by the host. Whatever target was bound is bound
// again afterwards and no cached state is trusted: for the commands that follow this is the
// same situation as after the present hook.
void ShowProgress() {
    static const bool enabled = EnvInt("MSC_VGX_PROGRESS", 1) != 0;
    if (!enabled || !g_showHook || g_dispW <= 0 || g_dispH <= 0 || g_sw.total == 0) return;
    const GLuint prevFbo = S.fbo;
    static const float zero[16] = {};
    BindFbo(g_dispFbo);
    SetScissor(false);
    SetColorMask(true, true);
    SetBlendOff();
    SetDepthTest(false);
    SetDepthMask(false);
    SetCull(CULL_NONE);
    G(glClearColor(0.02f, 0.02f, 0.04f, 1.0f));
    TouchTarget();
    G(glClear(GL_COLOR_BUFFER_BIT));
    ++C.clears;
    const int w = g_dispW / 2, h = g_dispH / 34 > 6 ? g_dispH / 34 : 6, x = (g_dispW - w) / 2, y = g_dispH / 4;
    const uint32_t done = g_sw.done < g_sw.total ? g_sw.done : g_sw.total;
    const int fw = static_cast<int>(static_cast<uint64_t>(w) * done / g_sw.total);
    static const float frame[4] = {0.45f, 0.45f, 0.50f, 1.0f}, track[4] = {0.06f, 0.06f, 0.09f, 1.0f}, fill[4] = {0.95f, 0.78f, 0.20f, 1.0f};
    SetViewport(x - 2, y - 2, w + 4, h + 4);
    DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, frame);
    SetViewport(x, y, w, h);
    DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, track);
    if (fw > 0) {
        SetViewport(x, y, fw, h);
        DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, fill);
    }
    g_showHook(done, g_sw.total);
    ++g_sw.shown;
    ++g_stats.progressFrames;
    Invalidate();
    g_rsValid = false;
    g_inUseFbo = ~0u; // (the host showed the display: a new scene whatever comes next)
    BindFbo(prevFbo);
}

// The clear that goes with a copy: it clears the rectangle that was copied
// (EFB coordinates), not the whole frame buffer. The EFB is bound.
void Clear(const ClearSpec& c, const EfbRect& r) {
    if (!c.enable) return;
    BindFbo(g_efbFbo);
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    int x0 = Rnd(r.x * sx), y0 = Rnd(r.y * sy), x1 = Rnd((r.x + r.w) * sx), y1 = Rnd((r.y + r.h) * sy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_rtW) x1 = g_rtW;
    if (y1 > g_rtH) y1 = g_rtH;
    if (x1 <= x0 || y1 <= y0) return;
    g_rsValid = false;
    if (x0 == 0 && y0 == 0 && x1 == g_rtW && y1 == g_rtH) SetScissor(false);
    else SetScissor(true, x0, y0, x1 - x0, y1 - y0);
    bool colorByClear = c.color || c.alpha;
    if (colorByClear && c.color != c.alpha && g_quadClear) {
        // Colour without alpha or alpha without colour: glClear cannot be trusted
        // to honour the colour mask (vitaGL ignores it), a draw can.
        static const float zero[16] = {};
        SetViewport(0, 0, g_rtW, g_rtH);
        SetBlendOff();
        SetDepthTest(false);
        SetDepthMask(false);
        SetCull(CULL_NONE);
        SetColorMask(c.color, c.alpha);
        DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, c.rgba);
        ++C.quadClears;
        colorByClear = false;
    }
    GLbitfield mask = 0;
    if (colorByClear) {
        SetColorMask(c.color, c.alpha);
        G(glClearColor(c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]));
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (c.depth) {
        SetDepthMask(true);
        G(glClearDepthf(c.z));
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask) {
        TouchTarget((c.depth && S.scissorOn == 0) ? OP_CLEAR_FULLDEPTH : OP_CLEAR);
        G(glClear(mask));
        ++C.clears;
    }
}

// A pooled framebuffer object of w x h that has uses left this frame (bound on return).
// tex: the colour texture to draw into (of exactly this size); 0: the object's own colour
// texture. wantDepth: with a depth buffer. An object that already has `tex` attached is
// taken first and not attached to again: on vitaGL attaching to the object in use ends the
// scene (shared.h _glFramebufferTexture2D), so draws that follow each other into one
// texture (the passes of one atlas) stay one scene only this way.
int AcquireFbo(int w, int h, GLuint tex, bool wantDepth) {
    int idx = -1;
    if (tex) {
        for (size_t i = 0; i < g_pool.size(); ++i)
            if (g_pool[i].attached == tex && g_pool[i].w == w && g_pool[i].h == h && (g_pool[i].uses < g_fboMaxUses || g_inUseFbo == g_pool[i].fbo)) {
                idx = static_cast<int>(i);
                break;
            }
    }
    if (idx < 0)
        for (size_t i = 0; i < g_pool.size(); ++i)
            if (g_pool[i].w == w && g_pool[i].h == h && g_pool[i].uses < g_fboMaxUses) { idx = static_cast<int>(i); break; }
    if (idx < 0) {
        PoolFbo f;
        f.w = w;
        f.h = h;
        G(glGenFramebuffers(1, &f.fbo));
        g_pool.push_back(f);
        idx = static_cast<int>(g_pool.size() - 1);
        VGX_LOG("framebuffer object %dx%d created (%u pooled)\n", w, h, static_cast<unsigned>(g_pool.size()));
    }
    PoolFbo& f = g_pool[idx];
    f.lastPresent = g_poolPresents;
    BindFbo(f.fbo);
    if (!tex) {
        if (!f.color) {
            G(glGenTextures(1, &f.color));
            BindTexForEdit(f.color);
            G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            SetupNewTexture(GL_LINEAR);
        }
        tex = f.color;
    }
    // A use is a scene: going on drawing into what is attached and in use is not a new one.
    if (!(f.attached == tex && g_inUseFbo == f.fbo && !g_fbDirty)) ++f.uses;
    if (f.attached != tex) {
        AttachColor(tex);
        f.attached = tex;
    }
    if (wantDepth && !f.depthTex && !f.depthRb) {
        // Depth with the same storage as the EFB's, so depth tests come out the same.
        if (g_hasDepthTex) {
            G(glGenTextures(1, &f.depthTex));
            BindTexForEdit(f.depthTex);
            G(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr));
            SetupNewTexture(GL_NEAREST);
            G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, f.depthTex, 0));
        } else {
            G(glGenRenderbuffers(1, &f.depthRb));
            G(glBindRenderbuffer(GL_RENDERBUFFER, f.depthRb));
            G(glRenderbufferStorage(GL_RENDERBUFFER, g_hasDepth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16, w, h));
            G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, f.depthRb));
        }
    }
    return idx;
}

// 1.1 (console crash, 23 Oct): every pass size the dynamic resolution ever used kept its framebuffer object, colour and depth
// textures in the pool for the rest of the session, so a session that changed levels filled the video memory, and a target made
// with no memory left crashed the GPU set-up. At each present, objects not used for 60 presents are deleted (vitaGL frees their
// memory once the GPU is done with them). Nothing holds a pool index across a present.
uint32_t g_poolFastTrim = 0;   // presents left in which objects unused for 2 presents are deleted (after a target size change)
int DynVramFreeKib(bool fresh = false);
static const bool g_keepSizes = EnvInt("MSC_VGX_KEEP_SIZES", 1) != 0;   // MSC_VGX_KEEP_SIZES=0: the 60-present age always
void TrimPool() {
    ++g_poolPresents;
    if (g_pass >= 0 || g_inPass) return;   // (an index into the pool is in use: never compact under it)
    // 1.1 (resize stalls, after the Galaxy port's finding): on the console a size change stalled the picture for 80-190 ms, much of it
    // making the new size's pass objects (vitaGL makes a GPU render target for each). With video memory to spare the objects of the
    // sizes used in the last 20 s stay, so the size a Mega Strike goes back to finds its objects still there. Short of memory: as before.
    const int freeKib = DynVramFreeKib(false);
    const bool roomy = freeKib < 0 || freeKib >= 24576;
    const uint32_t age = (g_poolFastTrim && !roomy) ? 2u : (roomy && g_keepSizes ? 600u : 60u);
    if (g_poolFastTrim) --g_poolFastTrim;
    size_t kept = 0, freed = 0;
    for (size_t i = 0; i < g_pool.size(); ++i) {
        PoolFbo& f = g_pool[i];
        if (g_poolPresents - f.lastPresent > age && f.fbo != g_inUseFbo && f.fbo != S.fbo) {   // (not the open scene's target)
            if (S.fbo == f.fbo) {
                G(glBindFramebuffer(GL_FRAMEBUFFER, 0));
                S.fbo = 0;
                g_fbDirty = true;
            }
            if (g_inUseFbo == f.fbo) g_inUseFbo = ~0u;
            if (f.color) { const GLuint t = f.color; G(glDeleteTextures(1, &t)); ForgetTex(t); }
            if (f.depthTex) { const GLuint t = f.depthTex; G(glDeleteTextures(1, &t)); ForgetTex(t); }
            if (f.depthRb) G(glDeleteRenderbuffers(1, &f.depthRb));
            G(glDeleteFramebuffers(1, &f.fbo));
            ++freed;
            continue;
        }
        if (kept != i) g_pool[kept] = f;
        ++kept;
    }
    for (EfbSlot& k : g_efbKept)
        if (k.fbo && k.fbo != g_inUseFbo && (!roomy || g_poolPresents - k.lastPresent > 1800)) EfbDrop(k);
    if (freed) {
        g_pool.resize(kept);
        VGX_LOG_N(40, "framebuffer objects: %u unused for %u frames deleted, %u pooled\n", static_cast<unsigned>(freed), static_cast<unsigned>(age), static_cast<unsigned>(kept));
    }
}

// The texture a copy writes to: a render-target texture of w x h. atlas: only rectangles of it are ever written (cells of a
// pass atlas, rectangles of a copy), so the rest of it would hold whatever the video memory held on a console (the host's GL
// hands out zeros): such a texture is created with zeros (MSC_VGX_RTZERO=0: as before, uninitialised). Whole-texture copies
// write every texel before anything samples them.
Tex& CopyTexture(uint32_t texId, int w, int h, bool atlas = false) {
    Tex& t = TexSlotFor(texId);
    // DYNRES: a copy texture whose size follows the render scale gets a new size when the scale changes. Its picture is kept, scaled: some of
    // these textures are read before they are written again (a picture kept from an earlier frame, the other cells of an atlas); started
    // empty, such a texture showed as dark or missing effects for a few frames after every change. MSC_VGX_RESIZE_KEEP=0: start them empty.
    static const bool keepOnResize = EnvInt("MSC_VGX_RESIZE_KEEP", 1) != 0;
    if (keepOnResize && t.id && t.renderTarget && t.w && t.h && (t.w != w || t.h != h) && (EnsureCopyProgram(), g_copyProg != 0)) {
        GLuint fresh = 0;
        G(glGenTextures(1, &fresh));
        ++g_stats.textures;
        BindTexForEdit(fresh);
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        if (!BoundTexHasStorage()) {   // no memory for the second copy: give up the picture, respecify in place below
            G(glDeleteTextures(1, &fresh));
            ForgetTex(fresh);
            VGX_LOG_N(8, "copy texture %dx%d: no memory to keep its picture over the size change\n", w, h);
            goto respecify;
        }
        SetupNewTexture(GL_LINEAR);
        AcquireFbo(w, h, fresh, false);
        SetViewport(0, 0, w, h);
        SetScissor(false);
        SetBlendOff();
        SetDepthTest(false);
        SetCull(CULL_NONE);
        SetDepthMask(false);
        SetColorMask(true, true);
        g_rsValid = false;
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, kZero[4] = {0, 0, 0, 0};
        DrawQuad(t.id, 0.0f, 0.0f, 1.0f, 1.0f, kIdentity, kZero);
        ++C.copyQuads;
        const GLuint old = t.id;
        DetachFromPool(old);
        G(glDeleteTextures(1, &old));
        ++C.texDeletes;
        ForgetTex(old);
        t.id = fresh;
        t.w = static_cast<uint16_t>(w);
        t.h = static_cast<uint16_t>(h);
        t.mips = false;
        t.wrapS = t.wrapT = WRAP_CLAMP;
        t.minF = t.magF = FILT_LINEAR;
        return t;
    }
respecify:
    if (!t.id) { G(glGenTextures(1, &t.id)); ++g_stats.textures; }
    if (!t.renderTarget || t.w != w || t.h != h) {
        BindTexForEdit(t.id);
        static const bool zeroAtlas = EnvInt("MSC_VGX_RTZERO", 1) != 0;
        static std::vector<uint8_t> zeros;
        const void* init = nullptr;
        if (atlas && zeroAtlas) {
            const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
            if (zeros.size() < n) zeros.assign(n, 0);
            init = zeros.data();
        }
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, init));
        SetupNewTexture(GL_LINEAR);
        t.w = static_cast<uint16_t>(w);
        t.h = static_cast<uint16_t>(h);
        t.mips = false;
        t.renderTarget = true;
        t.wrapS = t.wrapT = WRAP_CLAMP;
        t.minF = t.magF = FILT_LINEAR;
    }
    return t;
}

// FX (23 Oct): additive effect sprites at a lower resolution. The game marks the draws of its additive sprite meshes (DrawRec::fx, from
// EXT_FX: the native port's np/fx.h): the glow and haze of Mega Strikes, item and super-ability effects, sparks. Additive light adds up in any
// order, so a run of such draws can be drawn into a target of half the size (a quarter of the pixels; MSC_FX_LOWRES=4: a sixteenth) that
// starts black and holds the scene's depth (each of its pixels the depth of the EFB pixel under its centre, so the sprites are still
// hidden behind what is in front of them), and then be added to the scene in one full-screen pass, scaled up. The sprites are soft by
// nature; what costs on the Vita is their overdraw (a Mega Strike stacks about 40 screens of them), which this divides by four. The run ends
// at the first command that is not such a draw (a draw of something else, a copy, a pass, the display copy). Only draws that add
// (blend ADD, destination factor ONE, source ALPHA or ONE), do not write depth and have no destination-alpha second pass, outside the
// offscreen passes, and only with a depth texture. Used in Mega Strike scenes always, and elsewhere from dynamic-resolution level 4 (the GPU
// over its budget at the game's own size), each run costing two scene changes and a full-screen pass (worth it only for heavy effects).
// MSC_FX_LOWRES=0: off; =2 (default): half size; =4: a quarter of each side; =always: in every scene (tests).
int g_fxDiv = -1;              // 0 off, 2 or 4
bool g_fxAlways = false;
bool g_fxBuf = false;          // the buffer being executed draws its FX runs at the lower resolution (decided before its RT scan)
bool g_fxOpen = false;         // an FX run is being drawn into the FX target
bool g_fxAlpha = false;        // a draw of the open run writes alpha (the composite then adds alpha as well)
int g_fxPoolIdx = -1;
GLuint g_fxDepthProg = 0;
GLint g_fxDepthRect = -1;
bool g_fxDepthFailed = false;
#ifdef __VGX_VITA__
GLuint g_fxDepthSrc = 0;       // (console) a texture name that shows the EFB's depth buffer (vglStrikersBindFramebufferDepth)
#endif
// The scene's depth can be read: a depth texture (desktop GL), or the console's depth buffer through the patched vitaGL. vitaGL has no
// GL_OES_depth_texture and attaches colour only, so without the second the effects were never drawn at the lower resolution there.
bool FxDepthAvailable() {
#ifdef __VGX_VITA__
    if (vglStrikersBindFramebufferDepth && EnvInt("MSC_FX_VITA_DEPTH", 1) != 0) return true;
#endif
    return g_hasDepthTex && g_efbDepthTex;
}

int FxDiv() {
    if (g_fxDiv < 0) {
        const char* e = std::getenv("MSC_FX_LOWRES");
        g_fxDiv = 2;
        if (e && *e) {
            if (std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0) g_fxDiv = 0;
            else if (std::strcmp(e, "4") == 0) g_fxDiv = 4;
            else if (std::strcmp(e, "always") == 0) g_fxAlways = true;
        }
    }
    return g_fxDiv;
}
bool FxEligible(const DrawRec& dr) {
    const RenderState& rs = dr.rs;
    return dr.fx && rs.blend && rs.blendEq == BE_ADD && rs.blendDst == BF_ONE && (rs.blendSrc == BF_SRC_ALPHA || rs.blendSrc == BF_ONE) &&
           !rs.depthWrite && !rs.dstAlphaPass;
}
// Commands that draw nothing and may come inside a run.
bool FxNeutral(CmdType t) { return t == CMD_TEX_UPLOAD || t == CMD_TEX_FREE || t == CMD_SHADER || t == CMD_BUF_UPLOAD || t == CMD_BUF_FREE; }

bool EnsureFxDepthProgram() {
    if (g_fxDepthProg) return true;
    if (g_fxDepthFailed) return false;
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = u_rect.xy + (a_pos.xy * 0.5 + 0.5) * u_rect.zw; gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    // (as the Z-freeze shaders: GLES2 names the output gl_FragDepthEXT; vitaGL's translator knows gl_FragDepth and drops extension lines)
    static const char* fs =
        "#ifdef GL_EXT_frag_depth\n#extension GL_EXT_frag_depth : enable\n#define VGX_FRAGDEPTH gl_FragDepthEXT\n#else\n#define VGX_FRAGDEPTH gl_FragDepth\n#endif\n"
        "precision highp float;\nuniform sampler2D u_src;\nvarying vec2 v_uv;\n"
        "void main() { VGX_FRAGDEPTH = texture2D(u_src, v_uv).r; gl_FragColor = vec4(0.0); }\n";
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "fx depth vertex"), f = Compile(GL_FRAGMENT_SHADER, fs, "fx depth fragment");
    GLuint p = 0;
    GLint ok = 0;
    if (v && f) {
        p = G(glCreateProgram());
        if (p && p != 0xFFFFFFFFu) {
            G(glAttachShader(p, v));
            G(glAttachShader(p, f));
            G(glBindAttribLocation(p, AT_POS, "a_pos"));
            G(glLinkProgram(p));
            G(glGetProgramiv(p, GL_LINK_STATUS, &ok));
        } else {
            p = 0;
        }
    }
    if (v) G(glDeleteShader(v));
    if (f) G(glDeleteShader(f));
    if (!ok) {
        if (p) G(glDeleteProgram(p));
        g_fxDepthFailed = true;
        VGX_ERR("FX: the depth program did not build: effects are drawn at full size\n");
        return false;
    }
    g_fxDepthProg = p;
    UseProgram(p);
    g_fxDepthRect = G(glGetUniformLocation(p, "u_rect"));
    G(glUniform1i(glGetUniformLocation(p, "u_src"), 0));
    return true;
}

// Before the RT scan of a buffer: which of its draws go to the FX target (g_fxCmd, by command index; empty: none). A run pays two scene changes
// and about a screen and a quarter of pixels (the depth copy, the composite), so outside Mega Strikes (and MSC_FX_LOWRES=always) only runs of
// at least kFxMinVerts vertices (100 sprites) use it.
std::vector<uint8_t> g_fxCmd;
constexpr uint32_t kFxMinVerts = 400;
void FxDecide(const CmdBuf& buf, const DrawRec* drawRecs, bool mega, int dynLevel) {
    g_fxBuf = FxDiv() > 0 && FxDepthAvailable() && !g_fxDepthFailed && (g_fxAlways || mega || dynLevel >= 4);
    g_fxCmd.clear();
    if (!g_fxBuf) return;
    const int n = static_cast<int>(buf.cmds.size());
    g_fxCmd.assign(static_cast<size_t>(n), 0);
    bool inPass = g_inPass, any = false;
    int runStart = -1;
    uint32_t runVerts = 0;
    auto close = [&](int end) {
        if (runStart >= 0 && (g_fxAlways || mega || runVerts >= kFxMinVerts)) {
            for (int k = runStart; k < end; ++k)
                if (buf.cmds[static_cast<size_t>(k)].type == CMD_DRAW) g_fxCmd[static_cast<size_t>(k)] = 1;
            any = true;
        }
        runStart = -1;
        runVerts = 0;
    };
    for (int i = 0; i < n; ++i) {
        const Cmd& c = buf.cmds[static_cast<size_t>(i)];
        if (c.type == CMD_DRAW && FxEligible(drawRecs[c.arg]) && (runStart >= 0 || !inPass)) {
            if (runStart < 0) runStart = i;
            runVerts += drawRecs[c.arg].vtxCount;
            continue;
        }
        if (runStart >= 0 && !FxNeutral(c.type)) close(i);
        if (c.type == CMD_PASS_BEGIN) inPass = true;   // (as the RT scan: a copy that draws ends a pass, a skipped one when it is the pass's)
        else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& ct = buf.copyTex[c.arg];
            if (ct.skip ? ct.fromPass : !ct.srcTex) inPass = false;
        }
    }
    close(n);
    if (!any) { g_fxBuf = false; g_fxCmd.clear(); }
}
inline bool FxCmd(int i) { return g_fxBuf && i >= 0 && static_cast<size_t>(i) < g_fxCmd.size() && g_fxCmd[static_cast<size_t>(i)]; }

void BeginFx() {
    if (!EnsureFxDepthProgram()) { g_fxBuf = false; return; }
    // the depth source and the part of it the picture covers (u: the console's depth buffer rows are 32-pixel aligned)
    GLuint depthTex = g_efbDepthTex;
    float depthU = 1.0f;
    const bool consoleDepth = !(g_hasDepthTex && g_efbDepthTex);
#ifndef __VGX_VITA__
    if (consoleDepth) { g_fxBuf = false; return; }
#endif
    const int div = FxDiv();
    const int w = (g_rtW + div - 1) / div, h = (g_rtH + div - 1) / div;
    g_fxPoolIdx = AcquireFbo(w, h, 0, true);
    SetViewport(0, 0, w, h);
    SetScissor(false);
    SetColorMask(true, true);
    SetDepthMask(true);
    G(glClearColor(0, 0, 0, 0));
    G(glClearDepthf(1.0f));
    TouchTarget(OP_PASS_CLEAR);
    G(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ++C.clears;
#ifdef __VGX_VITA__
    if (consoleDepth) {
        // (bound here, after the EFB's scene has ended: its depth is stored by then; the RT scan marked the run start as a depth read)
        if (!g_fxDepthSrc) G(glGenTextures(1, &g_fxDepthSrc));
        BindTexForEdit(g_fxDepthSrc);
        const int aligned = vglStrikersBindFramebufferDepth ? vglStrikersBindFramebufferDepth(g_efbFbo) : 0;
        if (aligned > 0) {
            G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
            G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
            depthTex = g_fxDepthSrc;
            depthU = static_cast<float>(g_rtW) / static_cast<float>(aligned);
        } else {
            depthTex = 0;   // (no depth buffer: the effects are not hidden behind anything this once)
        }
    }
#endif
    // the scene's depth: the EFB depth under each pixel's centre (the depth texture is NEAREST)
    SetColorMask(false, false);
    SetBlendOff();
    SetCull(CULL_NONE);
    SetDepthTest(true, GL_ALWAYS);
    SetDepthMask(true);
    if (depthTex) {
        UseProgram(g_fxDepthProg);
        BindTex(0, depthTex);
        G(glUniform4f(g_fxDepthRect, 0.0f, 0.0f, depthU, 1.0f));
        ++C.uniformCalls;
        g_uniDirtyV = g_uniDirtyF = false;
        UsePictureQuadLayout();
        TouchTarget();
        G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
        ++C.copyQuads;
    }
    // the run's draws: like a pass over the whole EFB at 1/div of the render scale
    g_pass = g_fxPoolIdx;
    g_inPass = true;
    g_passDirectTex = 0;
    g_passX = g_passY = 0;
    g_passW = w;
    g_passH = h;
    g_psx = static_cast<float>(w) / static_cast<float>(g_efbW);
    g_psy = static_cast<float>(h) / static_cast<float>(g_efbH);
    g_rsValid = false;
    g_fxOpen = true;
    g_fxAlpha = false;
    ++C.fxRuns;
}

void EndFx() {
    if (!g_fxOpen) return;
    g_fxOpen = false;
    const GLuint color = g_pool[static_cast<size_t>(g_fxPoolIdx)].color;
    g_inPass = false;
    g_pass = -1;
    g_fxPoolIdx = -1;
    BindFbo(g_efbFbo);
    SetViewport(0, 0, g_rtW, g_rtH);
    SetScissor(false);
    SetDepthTest(false);
    SetDepthMask(false);
    SetCull(CULL_NONE);
    SetColorMask(true, g_fxAlpha);
    SetBlend(GL_FUNC_ADD, GL_ONE, GL_ONE);
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, kZero[4] = {0, 0, 0, 0};
    DrawQuad(color, 0.0f, 0.0f, 1.0f, 1.0f, kIdentity, kZero);
    ++C.copyQuads;
    g_rsValid = false;
}

// CMD_PASS_BEGIN: the draws up to the next copy go to a target of the pass
// rectangle's size that starts cleared to the values the EFB rectangle held.
// That includes passes over the whole EFB rectangle: they may use one plane
// only (the blur chain's last steps write alpha) while another plane of the
// EFB holds the picture.
void BeginPass(const PassCmd& p) {
    float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    int limW = g_rtW, limH = g_rtH;
    // DYNRES: a large pass outside an atlas at the reduced pass scale (its copy and every draw inside it follow g_psx / g_psy)
    if (g_passScale < 0.999f && !p.atlasTex && static_cast<int64_t>(p.rect.w) * p.rect.h * 8 >= static_cast<int64_t>(g_efbW) * g_efbH) {
        sx *= g_passScale;
        sy *= g_passScale;
        limW = Rnd(g_efbW * sx);
        limH = Rnd(g_efbH * sy);
        ++C.passesScaled;
    }
    g_psx = sx;
    g_psy = sy;
    int x0 = Rnd(p.rect.x * sx), y0 = Rnd(p.rect.y * sy), x1 = Rnd((p.rect.x + p.rect.w) * sx), y1 = Rnd((p.rect.y + p.rect.h) * sy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > limW) x1 = limW;
    if (y1 > limH) y1 = limH;
    g_pass = -1;
    g_inPass = false;
    g_passDirectTex = 0;
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return; // (the draws then go to the EFB with an empty scissor: nothing is drawn)
    const int cx = Rnd(p.atlasX * sx), cy = Rnd(p.atlasY * sy);
    if (p.atlasTex) {
        // A cell of an atlas (see PassCmd): the target is the atlas, the pass rectangle's
        // pixels land in the cell, and the cell is cleared with its gutter.
        const int aw = Rnd(p.atlasW * sx), ah = Rnd(p.atlasH * sy);
        const GLuint dest = CopyTexture(p.atlasTex, aw, ah, true).id;
        g_pass = AcquireFbo(aw, ah, dest, true);
        g_passDirectTex = p.directTex;
        g_passW = aw;
        g_passH = ah;
        g_passX = x0 - cx;
        g_passY = y0 - cy;
    } else {
        // The colour buffer: the pooled object's own texture, or (PassCmd::directTex, when the
        // destination has exactly the pass's size at this render scale) the copy's destination.
        static const bool allowDirect = EnvInt("MSC_VGX_PASSDIRECT", 1) != 0;
        bool direct = false;
        if (p.directTex && allowDirect) {
            int dw = Rnd(p.rect.w * sx), dh = Rnd(p.rect.h * sy); // what CopyTex would make the destination
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            direct = dw == w && dh == h;
        }
        if (direct) {
            const GLuint dest = CopyTexture(p.directTex, w, h).id;
            g_pass = AcquireFbo(w, h, dest, true);
            g_passDirectTex = p.directTex;
        } else {
            g_pass = AcquireFbo(w, h, 0, true);
        }
        g_passW = w;
        g_passH = h;
        g_passX = x0;
        g_passY = y0;
    }
    const GLenum st = G(glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (st != GL_FRAMEBUFFER_COMPLETE) VGX_LOG_N(8, "pass framebuffer %dx%d incomplete: 0x%X\n", w, h, st);
    if (p.atlasTex) {
        const int gx = Rnd(p.atlasGutter * sx), gy = Rnd(p.atlasGutter * sy);
        SetScissor(true, cx - gx, cy - gy, w + 2 * gx, h + 2 * gy);
    } else {
        SetScissor(false);
    }
    SetColorMask(true, true);
    SetDepthMask(true);
    G(glClearColor(p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]));
    G(glClearDepthf(p.z));
    TouchTarget(OP_PASS_CLEAR);
    G(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ++C.clears;
    g_inPass = true;
    g_rsValid = false;
}

void EndPass() { // back to the EFB
    BindFbo(g_efbFbo);
    g_inPass = false;
    g_pass = -1;
    g_passDirectTex = 0;
    g_rsValid = false;
}

// W5 RT: the moved display-copy clear when this copy is the buffer's first use of the EFB and that use is only the copy's own clear (RtScan):
// made just before that clear, so that the two make one EFB scene.
void RtPendingClearInCopy() {
    if (!g_rt.havePending || !g_rt.scanned || !g_rt.clearInCopy || g_rt.cmd != g_rt.clearAt) return;
    g_rt.havePending = false;
    ++C.rtDeferredClears;
    Clear(g_rt.pendingClear, g_rt.pendingRect);
}

void CopyTex(const CopyTexCmd& c) {
    if (c.skip) { // only its clear matters (a dead copy, or one that is made later from an atlas cell)
        if (c.fromPass && g_inPass) EndPass();
        RtPendingClearInCopy();
        Clear(c.clear, c.src);
        return;
    }
    if (c.fromPass && g_pass >= 0 && g_passDirectTex == c.texId) {
        // The pass was drawn straight into this texture: nothing to copy.
        EndPass();
        RtPendingClearInCopy();
        Clear(c.clear, c.src);
        return;
    }
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    // DYNRES: the copy that ends a pass drawn at a reduced scale makes its texture at that scale (it is sampled by normalised coordinates);
    // a copy into an atlas rectangle keeps the atlas at the render scale (the atlas holds other pictures)
    const bool passScaled = c.fromPass && g_pass >= 0 && g_inPass && !c.dstTexW && (g_psx != sx || g_psy != sy);
    int w = Rnd(c.dstW * (passScaled ? g_psx : sx)), h = Rnd(c.dstH * (passScaled ? g_psy : sy));
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // (The pass that this copy ends keeps its object: g_pass still names it while it is sampled.)
    const GLuint passColor = (c.fromPass && g_pass >= 0) ? g_pool[g_pass].color : 0;
    if (c.dstTexW) {
        // Into a rectangle of an atlas texture.
        const int tw = Rnd(c.dstTexW * sx), th = Rnd(c.dstTexH * sy);
        Tex& t = CopyTexture(c.texId, tw, th, true);
        AcquireFbo(tw, th, t.id, false);
        SetViewport(Rnd(c.dstX * sx), Rnd(c.dstY * sy), w, h);
    } else {
        Tex& t = CopyTexture(c.texId, w, h);
        AcquireFbo(w, h, t.id, false);
        SetViewport(0, 0, w, h);
    }
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    const bool fromPass = c.fromPass && g_pass >= 0;
    if (c.srcTex) {
        // Out of an atlas: a rectangle of another texture, unchanged.
        float m[16];
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) m[col * 4 + r] = c.mat[r][col];
        const GLuint src = (c.srcTex < g_tex.size() && g_tex[c.srcTex].id) ? g_tex[c.srcTex].id : g_whiteTex;
        DrawQuad(src, static_cast<float>(c.src.x) / c.srcTexW, static_cast<float>(c.src.y) / c.srcTexH, static_cast<float>(c.src.w) / c.srcTexW,
                 static_cast<float>(c.src.h) / c.srcTexH, m, c.add);
        ++C.copyQuads;
        BindFbo(g_efbFbo);
        g_rsValid = false;
        return; // (not the end of a pass, and it has no clear)
    }
    if (c.depth && !g_hasDepthTex) {
        G(glClearColor(1, 1, 1, 1));
        TouchTarget(OP_COPY);
        G(glClear(GL_COLOR_BUFFER_BIT));
        ++C.clears;
    } else if (c.fill && !c.depth) {
        // The source is one known colour (as stored in the 8-bit EFB): write its converted value.
        float q[4], o[4];
        for (int i = 0; i < 4; ++i) {
            const float v = c.fillRgba[i] < 0 ? 0 : (c.fillRgba[i] > 1 ? 1 : c.fillRgba[i]);
            q[i] = std::floor(v * 255.0f + 0.5f) / 255.0f;
        }
        for (int r = 0; r < 4; ++r) o[r] = c.mat[r][0] * q[0] + c.mat[r][1] * q[1] + c.mat[r][2] * q[2] + c.mat[r][3] * q[3] + c.add[r];
        G(glClearColor(o[0], o[1], o[2], o[3]));
        TouchTarget(OP_COPY);
        G(glClear(GL_COLOR_BUFFER_BIT));
        ++C.clears;
    } else {
        float m[16];
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) m[col * 4 + r] = c.depth ? (col == 0 ? 1.0f : 0.0f) : c.mat[r][col];
        const GLuint src = c.depth ? g_efbDepthTex : (fromPass ? passColor : g_efbColor);
        if (fromPass) DrawQuad(src, 0.0f, 0.0f, 1.0f, 1.0f, m, c.add);
        else
            DrawQuad(src, static_cast<float>(c.src.x) / g_efbW, static_cast<float>(c.src.y) / g_efbH, static_cast<float>(c.src.w) / g_efbW,
                     static_cast<float>(c.src.h) / g_efbH, m, c.add);
        ++C.copyQuads;
    }
    EndPass(); // a pass ends with its copy
    RtPendingClearInCopy();
    Clear(c.clear, c.src);
}

// W5 RT: the scan of a command buffer for the EFB's depth (see "W5 RT" above). Command i reads the EFB's depth: a draw outside a pass with the
// depth test on, a depth copy of the EFB. It kills it: a clear of the whole depth buffer (a copy's or the display copy's clear over the whole
// target), a new target size. It ends an EFB scene: a pass that has a target, a copy that draws (not a skipped one), the display copy (the host
// presents), a new target size.
bool RtWholeRect(const EfbRect& r) {
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    return Rnd(r.x * sx) <= 0 && Rnd(r.y * sy) <= 0 && Rnd((r.x + r.w) * sx) >= g_rtW && Rnd((r.y + r.h) * sy) >= g_rtH;
}
bool RtKills(const ClearSpec& c, const EfbRect& r) { return c.enable && c.depth && RtWholeRect(r); }
void RtScan(const CmdBuf& buf, const DrawRec* drawRecs) {
    Rt& rt = g_rt;
    const int n = static_cast<int>(buf.cmds.size());
    rt.n = n;
    rt.scanned = true;
    rt.cmd = -1;
    rt.live.assign(static_cast<size_t>(n) + 1, 1);
    rt.nextBreak.assign(static_cast<size_t>(n), n);
    static std::vector<uint8_t> rd, kill;
    std::vector<uint8_t>& brk = rt.brk;
    brk.assign(static_cast<size_t>(n), 0);
    rt.clearAt = -1;
    rd.assign(static_cast<size_t>(n), 0);
    kill.assign(static_cast<size_t>(n), 0);
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    bool inPass = g_inPass;
    bool inFx = false; // FX: a run of effect draws drawn into the FX target (as ExecuteEx: it reads the EFB's depth and ends the EFB scene)
    rt.clearInCopy = false;
    const bool presents = (g_presentHook || g_capture) && !g_hidden;
    for (int i = 0; i < n; ++i) {
        const Cmd& c = buf.cmds[static_cast<size_t>(i)];
        // The first command that uses the EFB, where a moved display-copy clear is made: outside a pass a draw, a warm-up draw, the display copy,
        // a target change (just before the command); a copy whose only use of the EFB is its own clear (most copies that end a pass: just before
        // that clear, inside CopyTex, so the two clears make one scene); a copy that reads the EFB (its colour, or the depth): just before the
        // command, or, inside a pass, at the start of the buffer (as before: never inside a pass, and never between two passes on one target).
        if (rt.clearAt < 0) {
            if (c.type == CMD_COPY_TEX) {
                const CopyTexCmd& ct = buf.copyTex[c.arg];
                if (!ct.srcTex) { // (a copy out of an atlas neither reads nor clears the EFB)
                    const bool reads = !ct.skip && (ct.depth || (!ct.fill && !(ct.fromPass && inPass)));
                    if (reads) rt.clearAt = inPass ? 0 : i;
                    else if (ct.clear.enable) { rt.clearAt = i; rt.clearInCopy = true; }
                }
            } else if (!inPass && (c.type == CMD_DRAW || c.type == CMD_VARIANT || c.type == CMD_COPY_DISP || c.type == CMD_SET_TARGET)) {
                rt.clearAt = i;
            }
        }
        if (g_fxBuf) {
            const bool fxDraw = FxCmd(i);
            if (fxDraw) {
                if (!inFx) { brk[i] = 1; rd[i] = 1; inFx = true; }
                continue;
            }
            if (inFx && !FxNeutral(c.type)) { brk[i] = 1; inFx = false; }
        }
        switch (c.type) {
        case CMD_DRAW:
            if (!inPass && drawRecs[c.arg].rs.depthTest) rd[i] = 1;
            break;
        case CMD_PASS_BEGIN: {
            const PassCmd& p = buf.passes[c.arg];
            const int x0 = Rnd(p.rect.x * sx), y0 = Rnd(p.rect.y * sy), x1 = Rnd((p.rect.x + p.rect.w) * sx), y1 = Rnd((p.rect.y + p.rect.h) * sy);
            const int w = std::min(x1, g_rtW) - std::max(x0, 0), h = std::min(y1, g_rtH) - std::max(y0, 0);
            inPass = w > 0 && h > 0;
            if (inPass) brk[i] = 1;
            break;
        }
        case CMD_COPY_TEX: {
            const CopyTexCmd& ct = buf.copyTex[c.arg];
            if (ct.skip) {
                if (ct.fromPass) inPass = false;
                if (RtKills(ct.clear, ct.src)) kill[i] = 1;
            } else if (ct.srcTex) {
                brk[i] = 1;
            } else {
                brk[i] = 1;
                if (ct.depth) rd[i] = 1; // (CopyTex samples the EFB's depth for every depth copy, a pass's included)
                inPass = false;
                if (RtKills(ct.clear, ct.src)) kill[i] = 1;
            }
            break;
        }
        case CMD_COPY_DISP: {
            const CopyDispCmd& cd = buf.copyDisp[c.arg];
            if (presents) brk[i] = 1;
            if (RtKills(cd.clear, cd.src)) kill[i] = 1;
            break;
        }
        case CMD_SET_TARGET: {
            int w = 0, h = 0;
            DynTargetSize(buf.targets[c.arg].efbW, buf.targets[c.arg].efbH, &w, &h);
            if (w != g_rtW || h != g_rtH) { brk[i] = 1; kill[i] = 1; }
            break;
        }
        default: break;
        }
    }
    // The display copy's clear moves to the next buffer (CopyDisp) when nothing after it here uses the EFB: then the EFB's depth
    // after this buffer is killed there (or not), not at the copy.
    rt.live[static_cast<size_t>(n)] = 1;
    rt.deferAt = -1;
    for (int i = n - 1; i >= 0; --i) {
        const CmdType t = buf.cmds[static_cast<size_t>(i)].type;
        if (t == CMD_DRAW || t == CMD_PASS_BEGIN || t == CMD_COPY_TEX || t == CMD_SET_TARGET) break;
        if (t == CMD_COPY_DISP) {
            if (kill[i]) { kill[i] = 0; rt.live[static_cast<size_t>(n)] = 0; rt.deferAt = i; }
            break;
        }
    }
    for (int i = n - 1; i >= 0; --i)
        rt.live[static_cast<size_t>(i)] = rd[i] ? 1 : (kill[i] ? 0 : rt.live[static_cast<size_t>(i) + 1]);
    for (int i = n - 2; i >= 0; --i) rt.nextBreak[static_cast<size_t>(i)] = brk[i + 1] ? i + 1 : rt.nextBreak[static_cast<size_t>(i) + 1];
    rt.firstBreak = n > 0 ? (brk[0] ? 0 : rt.nextBreak[0]) : 0;
}
// May the progress bar change scenes now (after command i)? Not inside a pass, and not inside an EFB scene that will not store a depth
// that is read later.
bool RtProgressAllowed() {
    if (RtMode() <= 0) return true;
    if (g_inPass) return false;
    if (g_rt.cur.open && g_rt.cur.kind == 0 && (g_rt.curHints & kHintNoStore)) {
        const int i = g_rt.cmd + 1;
        if (i >= 0 && i <= g_rt.n && g_rt.live[static_cast<size_t>(i)]) return false;
    }
    return true;
}

void CopyDisp(const CopyDispCmd& c) {
    if ((g_capture || g_presentHook) && !g_hidden) {
        const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
        int x = Rnd(c.src.x * sx), y = Rnd(c.src.y * sy), w = Rnd(c.src.w * sx), h = Rnd(c.src.h * sy);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > g_rtW) w = g_rtW - x;
        if (y + h > g_rtH) h = g_rtH - y;
        BindFbo(g_efbFbo);
        if (w > 0 && h > 0 && g_presentHook) {
            g_presentHook(g_efbFbo, x, y, w, h);
            // Whatever the hook did to the context: nothing cached is trusted any more,
            // and the EFB is the target again.
            Invalidate();
            g_rsValid = false;
            g_inUseFbo = ~0u; // (the host presented: a new scene whatever comes next)
            BindFbo(g_efbFbo);
        }
        if (w > 0 && h > 0 && g_capture && g_screenW > 0) {
            // MSC_VGX_SCREEN: the picture as the screen would show it (DrawPicture), read back top row first.
            EnsureScreenTarget();
            DrawPicture(g_screenFbo, x, y, w, h, 0, 0, g_screenW, g_screenH, g_screenMode, g_screenSharpen, false);
            g_captured.resize(static_cast<size_t>(g_screenW) * g_screenH * 4);
            G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
            G(glReadPixels(0, 0, g_screenW, g_screenH, GL_RGBA, GL_UNSIGNED_BYTE, g_captured.data()));
            ++C.readPixels;
            g_capW = g_screenW;
            g_capH = g_screenH;
            for (int i = 0; i < g_screenExtraN; ++i) {
                DrawPicture(g_screenFbo, x, y, w, h, 0, 0, g_screenW, g_screenH, g_screenExtra[i], g_screenSharpen, false);
                g_screenExtraPx[i].resize(static_cast<size_t>(g_screenW) * g_screenH * 4);
                G(glReadPixels(0, 0, g_screenW, g_screenH, GL_RGBA, GL_UNSIGNED_BYTE, g_screenExtraPx[i].data()));
                ++C.readPixels;
            }
            Invalidate();
            g_rsValid = false;
            g_inUseFbo = ~0u;
            BindFbo(g_efbFbo);
        } else if (w > 0 && h > 0 && g_capture) {
            g_captured.resize(static_cast<size_t>(w) * h * 4);
            G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
            G(glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_captured.data()));
            ++C.readPixels;
            g_capW = w;
            g_capH = h;
        }
    }
    if (RtMode() > 0 && g_rt.scanned && c.clear.enable && g_rt.cmd >= 0 && g_rt.cmd == g_rt.deferAt) {
        // (W5 RT) the clear begins the next frame's main EFB scene: it is made at the start of the next buffer (ExecuteEx), with that buffer scanned
        g_rt.havePending = true;
        g_rt.pendingClear = c.clear;
        g_rt.pendingRect = c.src;
        return;
    }
    Clear(c.clear, c.src);
}

// W5 GLB, MSC_VGX_STREAMDIFF=1 (census only: copies and hashes the upload, so never with a timing run): how much of the stream upload is what an earlier upload
// already held. A persistent stream buffer that wrote only the 64-byte blocks that differ from what its memory already contains would save the rest of the
// 3.9 ms per MB; this counts what that could save (same place = the same chunk and offset; the display buffers rotate through three slices, so "3 uploads ago" is
// what the memory of the slice in use holds; "any" = the block's hash occurs somewhere in the previous upload).
struct StreamHist {
    std::vector<std::vector<uint8_t>> chunks;
    std::vector<uint16_t> idx;
};
StreamHist g_streamHist[3];
uint64_t g_streamHistN = 0;
std::unordered_set<uint64_t> g_prevBlocks;
inline uint64_t BlockHash(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
void StreamDiff(const CmdBuf& buf, const uint8_t* vtx, bool indices) {
    StreamHist& h1 = g_streamHist[(g_streamHistN + 2) % 3]; // the previous upload
    StreamHist& h3 = g_streamHist[g_streamHistN % 3];       // three uploads ago: the slot this upload takes over
    std::unordered_set<uint64_t> blocks;
    const size_t nChunks = buf.vtxChunks.empty() ? 1 : buf.vtxChunks.size();
    h3.chunks.resize(nChunks);
    for (size_t i = 0; i < nChunks && !buf.vtx.empty(); ++i) {
        const size_t a = buf.vtxChunks.empty() ? 0 : buf.vtxChunks[i];
        const size_t b = i + 1 < nChunks ? buf.vtxChunks[i + 1] : buf.vtx.size();
        if (b <= a) { h3.chunks[i].clear(); continue; }
        const uint8_t* cur = vtx + a;
        const size_t n = b - a;
        const std::vector<uint8_t>* p1 = i < h1.chunks.size() ? &h1.chunks[i] : nullptr;
        const std::vector<uint8_t>* p3 = &h3.chunks[i];
        for (size_t o = 0; o < n; o += 64) {
            const size_t len = n - o < 64 ? n - o : 64;
            if (p1 && o + len <= p1->size() && std::memcmp(p1->data() + o, cur + o, len) == 0) C.streamSame1 += static_cast<uint32_t>(len);
            if (o + len <= p3->size() && std::memcmp(p3->data() + o, cur + o, len) == 0) C.streamSame3 += static_cast<uint32_t>(len);
            const uint64_t h = BlockHash(cur + o, len);
            if (g_prevBlocks.count(h)) C.streamAny1 += static_cast<uint32_t>(len);
            blocks.insert(h);
        }
        h3.chunks[i].assign(cur, cur + n);
    }
    if (indices && !buf.idx.empty()) {
        const size_t n = buf.idx.size() * 2;
        C.idxBytes += static_cast<uint32_t>(n);
        const uint8_t* cur = reinterpret_cast<const uint8_t*>(buf.idx.data());
        const uint8_t* old = reinterpret_cast<const uint8_t*>(h1.idx.data());
        const size_t on = h1.idx.size() * 2;
        for (size_t o = 0; o < n; o += 64) {
            const size_t len = n - o < 64 ? n - o : 64;
            if (o + len <= on && std::memcmp(old + o, cur + o, len) == 0) C.idxSame1 += static_cast<uint32_t>(len);
        }
        h3.idx.assign(buf.idx.begin(), buf.idx.end());
    }
    g_prevBlocks.swap(blocks);
    ++g_streamHistN;
}

// The frame's vertex arena goes into one buffer per chunk (CmdBuf::vtxChunks), its indices into one.
// `vtx`: the bytes to upload as the arena (buf.vtx.data() or a stand-in of the same size,
// ExecOpts::vtx); `indices` false: the index buffer already holds this frame's.
void UploadStream(const CmdBuf& buf, const uint8_t* vtx, bool indices) {
    static const bool streamDiff = EnvInt("MSC_VGX_STREAMDIFF", 0) != 0;
    if (streamDiff) StreamDiff(buf, vtx, indices);
    const size_t nChunks = buf.vtxChunks.empty() ? 1 : buf.vtxChunks.size();
    while (g_streamVbo.size() < nChunks) {
        GLuint b = 0;
        G(glGenBuffers(1, &b));
        g_streamVbo.push_back(b);
    }
    if (!buf.vtx.empty()) {
        for (size_t i = 0; i < nChunks; ++i) {
            const size_t a = buf.vtxChunks.empty() ? 0 : buf.vtxChunks[i];
            const size_t b = i + 1 < nChunks ? buf.vtxChunks[i + 1] : buf.vtx.size();
            if (b <= a) continue;
            BindArrayBuf(g_streamVbo[i]);
            G(glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(b - a), vtx + a, GL_STREAM_DRAW));
            ++C.streamUploads;
            C.streamBytes += static_cast<uint32_t>(b - a);
        }
    }
    if (indices && !buf.idx.empty()) {
        BindElemBuf(g_ibo);
        G(glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(buf.idx.size() * 2), buf.idx.data(), GL_STREAM_DRAW));
        ++C.streamUploads;
        C.streamBytes += static_cast<uint32_t>(buf.idx.size() * 2);
    }
}

#ifdef VGX_GL_STUB
// MSC_VGX_GLMEM=1 (stub-GL builds): the memory the driver has been asked to hold, by kind
// (src/glstub/GLES2/gl2.h), printed every 300 frames and whenever the total passes its
// previous peak by 1 MiB. On the Vita all of it comes out of vitaGL's video-memory pool
// first. Not in it: what vitaGL allocates for itself (display buffers, rings, patcher) and
// the previous frames' stream buffers it has not freed yet.
void ReportGlMem(uint64_t frame) {
    static const bool on = EnvInt("MSC_VGX_GLMEM", 0) != 0;
    if (!on) return;
    static uint64_t peak = 0;
    uint64_t b[VGX_STUB_KINDS];
    uint32_t n[VGX_STUB_KINDS];
    vgx_stub_mem(b, n);
    uint64_t total = 0;
    for (int i = 1; i < VGX_STUB_KINDS; ++i) total += b[i];
    const bool newPeak = total > peak + (1u << 20);
    if (!newPeak && frame % 300 != 0) return;
    if (total > peak) peak = total;
    const auto M = [](uint64_t v) { return v / 1048576.0; };
    std::fprintf(stderr, "[glmem] f=%llu total %.1f MiB%s: textures RGBA %.1f (%u) + DXT1 %.1f (%u) + LA88 %.1f (%u), render-target colour %.1f (%u) + depth %.1f (%u in textures, %u renderbuffers), "
                         "buffers static %.1f (%u) + static given as dynamic %.1f (%u) + stream %.1f (%u); %u pooled framebuffer objects\n",
                 static_cast<unsigned long long>(frame), M(total), newPeak ? " (NEW PEAK)" : "", M(b[VGX_STUB_TEX_RGBA]), n[VGX_STUB_TEX_RGBA], M(b[VGX_STUB_TEX_DXT]),
                 n[VGX_STUB_TEX_DXT], M(b[VGX_STUB_TEX_LA]), n[VGX_STUB_TEX_LA], M(b[VGX_STUB_RT_COLOR]), n[VGX_STUB_RT_COLOR], M(b[VGX_STUB_RT_DEPTH] + b[VGX_STUB_RB]), n[VGX_STUB_RT_DEPTH], n[VGX_STUB_RB],
                 M(b[VGX_STUB_BUF_STATIC]), n[VGX_STUB_BUF_STATIC], M(b[VGX_STUB_BUF_DYNAMIC]), n[VGX_STUB_BUF_DYNAMIC], M(b[VGX_STUB_BUF_STREAM]), n[VGX_STUB_BUF_STREAM],
                 static_cast<unsigned>(g_pool.size()));
    // MSC_VGX_GLMEM=2: the pooled objects too (size, own colour texture, depth buffer, uses this frame)
    if (EnvInt("MSC_VGX_GLMEM", 0) >= 2 && frame % 300 == 0) {
        std::fprintf(stderr, "[glmem] EFB object %dx%d; pool:", g_rtW, g_rtH);
        for (const PoolFbo& f : g_pool) std::fprintf(stderr, " %dx%d%s%s/%u", f.w, f.h, f.color ? "+c" : "", (f.depthTex || f.depthRb) ? "+d" : "", f.uses);
        std::fprintf(stderr, "\n");
    }
}
#endif

FILE* g_glStats = nullptr;
void WriteCounts(uint64_t frame) {
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (const char* p = std::getenv("MSC_VGX_GLSTATS")) {
            g_glStats = std::fopen(p, "w");
            if (g_glStats) {
                std::fprintf(g_glStats, "frame");
#define X(n) std::fprintf(g_glStats, "," #n);
                VGX_GL_COUNTERS(X)
#undef X
                std::fprintf(g_glStats, "\n");
            }
        }
    }
    if (!g_glStats) return;
    std::fprintf(g_glStats, "%llu", static_cast<unsigned long long>(frame));
#define X(n) std::fprintf(g_glStats, ",%u", C.n);
    VGX_GL_COUNTERS(X)
#undef X
    std::fprintf(g_glStats, "\n");
    if (frame % 64 == 0) std::fflush(g_glStats);
}

} // namespace

#if !defined(__VGX_VITA__)
namespace {
// MSC_VGX_PROGRESS_DUMP=<dir> (testing): a host without a display gets one made here, a
// 480x272 texture, and every bar shown is written to <dir>/progress_<n>.ppm (the first 6 and
// then every 25th).
std::string g_progDumpDir;
uint64_t g_progFrameNow = 0;
void DumpProgress(uint32_t done, uint32_t total) {
    static uint32_t n = 0;
    ++n;
    if (n > 6 && n % 25 != 0) return;
    std::vector<uint8_t> px(static_cast<size_t>(g_dispW) * g_dispH * 4);
    G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
    G(glReadPixels(0, 0, g_dispW, g_dispH, GL_RGBA, GL_UNSIGNED_BYTE, px.data()));
    char path[600];
    std::snprintf(path, sizeof path, "%s/progress_%04u_f%llu_%u_of_%u.ppm", g_progDumpDir.c_str(), n, static_cast<unsigned long long>(g_progFrameNow), done, total);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", g_dispW, g_dispH);
        for (int y = g_dispH - 1; y >= 0; --y) // GL row 0 is the bottom row
            for (int x = 0; x < g_dispW; ++x) std::fwrite(&px[(static_cast<size_t>(y) * g_dispW + x) * 4], 1, 3, f);
        std::fclose(f);
    }
}
void MakeProgressDumpDisplay() {
    const char* d = std::getenv("MSC_VGX_PROGRESS_DUMP");
    if (!d || !*d) return;
    g_progDumpDir = d;
    GLuint fbo = 0, tex = 0;
    G(glGenFramebuffers(1, &fbo));
    G(glGenTextures(1, &tex));
    BindTexForEdit(tex);
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    SetupNewTexture(GL_NEAREST);
    BindFbo(fbo);
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
    BindFbo(g_efbFbo);
    SetProgressDisplay(fbo, 480, 272, &DumpProgress);
}
} // namespace
#endif

bool Init(int renderWidth, int renderHeight) {
    const validate::Window validationWindow = validate::ParseWindow(std::getenv("MSC_VGX_VALIDATE"));
    if (validationWindow.invalid) std::fprintf(stderr, "[validate] invalid MSC_VGX_VALIDATE: use <frame> or <first>:<last>, at most four frames; disabled\n");
    if (validationWindow.enabled) {
        g_validation = std::make_unique<validate::Session>(validationWindow);
        std::fprintf(stderr, "[validate] enabled frames=%llu:%llu static_shadow_limit=16777216 detail_limit=16+8 CPU-contract-only; diagnostic timings are not performance evidence\n",
                     static_cast<unsigned long long>(validationWindow.first), static_cast<unsigned long long>(validationWindow.last));
    }
    g_fixedW = renderWidth;
    g_fixedH = renderHeight;
    g_userFixedW = renderWidth;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    g_hasDepth24 = ext && std::strstr(ext, "GL_OES_depth24");
    g_hasDepthTex = ext && std::strstr(ext, "GL_OES_depth_texture") && EnvInt("MSC_VGX_DEPTH_TEX", 1);
#ifdef __VGX_VITA__
    g_hasS3tc = true; // (vitaGL takes DXT1 whatever its extension string says)
    g_hasDxt5 = true; // (and DXT5: SCE_GXM_TEXTURE_FORMAT_UBC3)
#else
    g_hasS3tc = ext && (std::strstr(ext, "GL_EXT_texture_compression_s3tc") || std::strstr(ext, "GL_EXT_texture_compression_dxt1")) &&
                EnvInt("MSC_VGX_S3TC", 1);
    g_hasMaxLevel = (ver && std::strstr(ver, "OpenGL ES 3")) || (ext && std::strstr(ext, "GL_APPLE_texture_max_level"));
    g_hasDxt5 = g_hasS3tc && std::strstr(ext, "GL_EXT_texture_compression_s3tc");
#endif
    g_quadClear = EnvInt("MSC_VGX_QUADCLEAR", 1) != 0;
    g_noCache = EnvInt("MSC_VGX_GLCACHE", 1) == 0;
    g_fboMaxUses = static_cast<uint32_t>(EnvInt("MSC_VGX_FBO_USES", 8));
    if (const char* e = std::getenv("MSC_VGX_STATIC_USAGE"); e && std::strcmp(e, "dynamic") == 0) g_staticUsage = GL_DYNAMIC_DRAW;
    // MSC_VGX_EFB0=WxH: the size the EFB object is first made with (default 640x528). The game
    // sets its real size in its first frame; when that differs, the first object's colour and
    // depth memory are given back at once. (Diagnostic for Vita3K, whose surface cache goes on
    // treating the freed addresses as render targets: v3k/NOTES.md, session 3.)
    if (const char* e = std::getenv("MSC_VGX_EFB0")) {
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 1024 && h <= 1024) { g_efbW = w; g_efbH = h; }
    }
    if (g_fboMaxUses < 1) g_fboMaxUses = 1;
    G(glGenBuffers(1, &g_ibo));
    G(glGenBuffers(1, &g_quadVbo));
    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    BindArrayBuf(g_quadVbo);
    G(glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW));
    // Every attribute location gets a pointer into a buffer once (see the notes at the top).
    for (int a = 0; a < AT_COUNT; ++a) G(glVertexAttribPointer(static_cast<GLuint>(a), 2, GL_FLOAT, GL_FALSE, 0, nullptr));
    G(glGenTextures(1, &g_whiteTex));
    BindTexForEdit(g_whiteTex);
    const uint8_t white[4] = {255, 255, 255, 255};
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    g_tex.resize(256);
    CreateTarget();
    EnsureCopyProgram(); // compiled here, not at the first copy that needs it
#if !defined(__VGX_VITA__)
    MakeProgressDumpDisplay();
#endif
    G(glDisable(GL_DITHER));
    G(glFrontFace(GL_CCW)); // GX front faces are clockwise on screen; with the flipped Y that is CCW here
    VGX_LOG("GL back end: %s / %s; DXT1 %s, clears with a partial colour mask %s\n", glGetString(GL_RENDERER), ver ? ver : "?",
            g_hasS3tc ? "native" : "decoded here", g_quadClear ? "drawn" : "by glClear");
    return true;
}

namespace {
// MSC_VGX_PROF=1: wall time per command type on the render thread, printed every 100 presents.
double g_prof[CMD_TYPE_COUNT] = {};
uint32_t g_profCount[CMD_TYPE_COUNT] = {};
uint32_t g_profFrames = 0;
double g_profTotal = 0, g_profStream = 0;
uint64_t g_profSum[96] = {}; // the call census (VGX_GL_COUNTERS) summed over the same frames
static_assert(sizeof(Counts) / sizeof(uint32_t) <= 96, "g_profSum too small");
inline double Now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
float g_resizeCpuMs = 0, g_resizeCreateMs = 0;
bool g_resizeReused = false;
int g_resizeWatch = 0;
double g_resizeLast = 0;
uint64_t g_profCmds = 0; // commands timed (MSC_VGX_PROF=1/2) over the same frames
// What one Now() costs, measured once (W5 GLB). On the console steady_clock is a system call plus a 64-bit divide (about 6.7 us, measured from the
// 5 Oct log: a command that does nothing reads 0.01 ms), and MSC_VGX_PROF=1/2 reads it twice per command: about 5.6 ms of the 17.4 ms "all=" of that run
// was the profiler. net= subtracts it; MSC_VGX_PROF=3 reads the clock three times per frame only.
double ClockCostMs() {
    static double cost = -1.0;
    if (cost < 0.0) {
        const int n = 2000;
        volatile double sink = 0.0;
        const double a = Now();
        for (int i = 0; i < n; ++i) sink = Now();
        cost = (Now() - a) / (n + 1);
        (void)sink;
    }
    return cost;
}
}

// W5 RT, MSC_VGX_RTTIME=1: the first draw of every scene (on vitaGL the scene change happens inside it when no clear began the scene:
// sceGxmEndScene, sceGxmBeginScene, the mask; and in any case the program, streams, textures and uniform blocks are given again) and every 32nd
// other draw are timed with the host's microsecond clock (SetMicroClock).
uint64_t g_rtDrawTimers[4] = {}; // cumulative: first draws, their us, sampled draws, their us
uint64_t g_execFrame = 0;        // CmdBuf::frame of the buffer being executed (ExecutingFrame)
uint64_t g_execLatencyTag = 0;
void RtTimedDraw(const CmdBuf& buf, const DrawRec& dr) {
    if (g_rt.timeDraws <= 0) {
        Draw(buf, dr);
        return;
    }
    static uint32_t nth = 0;
    const bool first = g_rt.firstDraw || S.fbo != g_inUseFbo || g_fbDirty;
    if (!first && (++nth & 31u) != 0) {
        Draw(buf, dr);
        return;
    }
    const uint32_t t0 = MicroNow();
    Draw(buf, dr);
    const uint32_t dt = MicroNow() - t0;
    if (first) { ++C.rtFirstDraws; C.rtFirstDrawUs += dt; ++g_rtDrawTimers[0]; g_rtDrawTimers[1] += dt; }
    else { ++C.rtSampleDraws; C.rtSampleDrawUs += dt; ++g_rtDrawTimers[2]; g_rtDrawTimers[3] += dt; }
}

// DYNRES: dynamic resolution (23 Oct; replaces the Mega-only resolution of the same day). The renderer measures what the GPU spends on a frame
// (SetGpuTimeSource: the Vita host reads vitaGL's GPU timers, one fragment notification per scene) and moves along a ladder of picture levels so
// that a frame keeps inside the display budget of the frame rate in force (NpFpsDisplayInterval: 16.7 ms at 60, 33.4 ms at 30):
//   levels 0-2   the scene drawn LARGER than the game's own frame buffer (up to 1.2 per side, at most 960x544), only when the GPU has room to spare
//   level 3      the game's own size (the start)
//   levels 4-5   the large offscreen passes (glow and blur chains, full-screen filters: soft by nature) at 75 % / 50 % of the scene's scale: the
//                cheapest saving that is hard to see
//   levels 6-7   the scene itself at 93 % / 86 % per side: the floor of every ordinary scene (a match, the menus), so a match never looks soft
//   levels 8-12  Mega Strike scenes only (the game reports them, VgxMegaSceneHint): 78 % down to 50 % per side. A Mega Strike starts at 70 % at once
//                (its first frames would cost 60+ ms at full size); 20 frames after it the level from before it returns.
// Down: when the smoothed GPU time passes 90 % of the budget, straight to the best level predicted to fit in 82 % (a level's cost is modelled as a
// fixed quarter plus three quarters that follow its pixels). Up: one level when the next better one is predicted under 72 % for 45 measured frames.
// After a change the frames that were already queued are not measured. VgxGpuMinMs tells the frame-rate controller (np_fps_ctl.h) what the GPU
// would cost at this scene's floor, so it does not try 60 where the GPU cannot hold it. The size changes between frames only (never in an
// interpolated replay); the host's sharpening follows the scale (VgxRenderScale).
// MSC_DYNRES=off: the game's own size always (Mega Strikes included, unless MSC_MEGA_RENDER asks); MSC_DYNRES_MAX=1.0..1.2 caps the larger
// sizes, MSC_DYNRES_MIN=0.75..1.0 the floor of ordinary scenes; MSC_MEGA_RENDER=off keeps Mega Strikes at the ordinary floor, =WxH one fixed
// size for them. MSC_DYNRES_FAKE=a,b (desktop tests): a GPU time of a + b x the level's relative cost (x4 in Mega Strikes), two frames late.
// None of it with MSC_VITA_RENDER (a fixed size). Without a GPU time (no timers) only the Mega Strike start and its old 30 fps steps work.
namespace {
struct DynLevel { float s, p; };
const DynLevel kDynLevels[] = {{1.20f, 1.0f}, {1.12f, 1.0f}, {1.06f, 1.0f}, {1.00f, 1.0f}, {1.00f, 0.75f}, {1.00f, 0.5f}, {0.93f, 0.5f},
                               {0.86f, 0.5f}, {0.78f, 0.5f}, {0.70f, 0.5f}, {0.62f, 0.5f}, {0.56f, 0.5f}, {0.50f, 0.5f}};
const int kDynCount = static_cast<int>(sizeof kDynLevels / sizeof kDynLevels[0]);
const int kDynNative = 3, kDynFloor = 7, kDynMegaStart = 9, kDynLast = kDynCount - 1;
float DynCost(int l) {
    const DynLevel& v = kDynLevels[l];
    return 0.25f + 0.75f * v.s * v.s * (0.7f + 0.3f * v.p * v.p);
}

struct DynRes {
    bool inited = false;
    bool on = true;
    int megaMode = 0;             // -1 off, 0 auto, 1 fixed (megaW x megaH)
    int megaW = 0, megaH = 0;
    int minLevel = 0;             // the best level allowed (MSC_DYNRES_MAX)
    int floorLevel = kDynFloor;   // ordinary scenes (MSC_DYNRES_MIN)
    int floor60 = kDynFloor + 1;  // ordinary scenes while the game runs at 60 (0.78; MSC_DYNRES_MIN60, or MSC_DYNRES_MIN for both)
    float fakeA = 0, fakeB = 0;
    bool fake = false;
    int cycle = 0;                // MSC_DYNRES_CYCLE=N (desktop stress test): the next level every N frames, through all of them
    int cycleFrames = 0;
    int level = kDynNative;
    int normalLevel = kDynNative; // the level before a Mega Strike began
    bool mega = false;
    int megaQuiet = 0;
    // GPU time
    uint32_t seq = 0, ignoreUntil = 0;
    float ema = -1.0f;
    int over = 0, headroom = 0;
    int upHold = 45;              // measured frames of room before going up a level; doubled when a level gone up to is left again soon
    uint32_t lastUpSeq = 0, lastDownSeq = 0;
    // 1.1 (console test): a step down that does not lower the GPU time is undone. In a Mega Save on the console the picture went down to
    // 0.50 while the GPU time stayed at the frame budget (the haze, already at a quarter of the pixels, and the per-draw work do not shrink
    // with the picture), so the scene looked rough for nothing. probeAt: when to judge the last step down; noGainFloor: the lowest level
    // worth using until the scene changes (-1: none).
    uint32_t probeAt = 0;
    int probeFrom = -1;
    float probeEma = 0.0f;
    int noGainFloor = -1;
    int megaNoGain = -1;          // 1.1: the level a Mega Strike found no gain below (kept for the session's next Mega Strikes)
    int interval = 0;             // NpFpsDisplayInterval of the last decision
    float fakeRing[4] = {};
    uint32_t fakeSeq = 0;
    // without a GPU time: the Mega Strike's old steps by frame interval
    int frames = 0;
    double windowStart = 0;
    int changes = 0;
} g_dyn;
GpuTimeFn g_gpuTime = nullptr;
std::atomic<float> g_dynScale{1.0f};
std::atomic<float> g_gpuMinMs{0.0f};
std::atomic<int> g_megaActive{0};    // a Mega Strike scene is on (VgxMegaActive: the frame-rate controller keeps it at 30)
std::atomic<int> g_megaStep{0};      // 0 off, else 1 + how far a Mega Strike went down (VgxMegaStep: the particle thinning)
std::atomic<uint32_t> g_megaHint{0}; // set by the game while a Mega Strike scene is drawn (VgxMegaSceneHint)

void DynInit() {
    DynRes& d = g_dyn;
    d.inited = true;
    if (const char* e = std::getenv("MSC_DYNRES"); e && (std::strcmp(e, "off") == 0 || std::strcmp(e, "0") == 0)) d.on = false;
    if (const char* e = std::getenv("MSC_MEGA_RENDER"); e && *e) {
        if (std::strcmp(e, "off") == 0 || std::strcmp(e, "0") == 0) d.megaMode = -1;
        else if (std::sscanf(e, "%dx%d", &d.megaW, &d.megaH) == 2 && d.megaW >= 160 && d.megaH >= 120 && d.megaW <= 960 && d.megaH <= 544) d.megaMode = 1;
    }
    if (const char* e = std::getenv("MSC_DYNRES_MAX"); e && *e) {
        const float m = static_cast<float>(std::atof(e));
        d.minLevel = kDynNative;
        for (int l = 0; l < kDynNative; ++l)
            if (kDynLevels[l].s <= m + 0.001f) { d.minLevel = l; break; }
    }
    if (const char* e = std::getenv("MSC_DYNRES_MIN60"); e && *e) {
        const float m = static_cast<float>(std::atof(e));
        d.floor60 = kDynNative;
        for (int l = kDynNative; l <= kDynFloor + 2 && l < kDynCount; ++l)
            if (kDynLevels[l].s >= m - 0.001f) d.floor60 = l;
    }
    if (const char* e = std::getenv("MSC_DYNRES_MIN"); e && *e) {
        const float m = static_cast<float>(std::atof(e));
        d.floorLevel = kDynNative;
        for (int l = kDynNative; l <= kDynFloor + 2 && l < kDynCount; ++l)
            if (kDynLevels[l].s >= m - 0.001f) d.floorLevel = l;
    }
    if (std::getenv("MSC_DYNRES_MIN") && !std::getenv("MSC_DYNRES_MIN60")) d.floor60 = d.floorLevel;
    if (const char* e = std::getenv("MSC_DYNRES_FAKE"); e && *e) d.fake = std::sscanf(e, "%f,%f", &d.fakeA, &d.fakeB) == 2;
    if (const char* e = std::getenv("MSC_DYNRES_CYCLE"); e && *e) d.cycle = std::atoi(e);
    std::fprintf(stderr, "[vgx] dynamic resolution: %s; levels %.2f..%.2f per side in ordinary scenes, Mega Strikes %s; GPU time %s\n", d.on ? "on" : "off (MSC_DYNRES)",
            kDynLevels[d.floorLevel].s, kDynLevels[d.minLevel].s, d.megaMode < 0 ? "as ordinary scenes" : (d.megaMode == 1 ? "fixed size" : "down to 0.50"),
            d.fake ? "simulated (MSC_DYNRES_FAKE)" : (g_gpuTime ? "measured" : "not measured"));
}

// The render-target size for an EFB size at the level in force (CreateTarget, the RT scan's target-change check).
void DynTargetSize(int efbW, int efbH, int* w, int* h) {
    if (g_fixedW) { *w = g_fixedW; *h = g_fixedH; return; }   // MSC_VITA_RENDER, or a fixed Mega Strike size
    const float s = kDynLevels[g_dyn.level].s;
    if (s > 0.999f && s < 1.001f) { *w = efbW; *h = efbH; return; }
    int ww = (static_cast<int>(static_cast<float>(efbW) * s + 0.5f) + 7) & ~7, hh = (static_cast<int>(static_cast<float>(efbH) * s + 0.5f) + 7) & ~7;
    if (ww > 960) ww = 960;
    if (hh > 544) hh = 544;
    if (s < 1.0f) {
        if (ww > efbW) ww = efbW;
        if (hh > efbH) hh = efbH;
    }
    *w = ww < 16 ? 16 : ww;
    *h = hh < 16 ? 16 : hh;
}

double DynBudgetMs(int interval) { return (interval == 1 ? 1.0 : 2.0) * 1001.0 / 60.0; }   // (0: no policy yet, unlocked: the 30 budget)

int DynVramFreeKib(bool fresh);
// 1.1 (console review): a level that changes the EFB size allocates a new colour target, vitaGL's depth buffer and render target for
// it, and new pass objects, while the old ones are only freed later (the collector, TrimPool). So a size change waits 20 presents
// after the previous one (a Mega Strike stepping down every few frames had four sizes alive at once), needs the free GPU memory for
// the new targets plus 8 MiB, and is followed by a quick trim of the pool. `force`: the Mega Strike's begin and end (one each).
uint32_t g_lastResize = 0;
bool DynSetLevel(int l, const char* why, bool force = false) {
    DynRes& d = g_dyn;
    if (l == d.level) return false;
    {
        const float s0 = kDynLevels[d.level].s, s1 = kDynLevels[l].s;
        if (std::fabs(s0 - s1) > 0.001f && !g_fixedW) {
            if (!force && g_poolPresents - g_lastResize < 20) return false;
            const int kib = DynVramFreeKib(true);
            const long w = static_cast<long>(g_efbW * s1) + 32, h = static_cast<long>(g_efbH * s1) + 32;
            const long needKib = w * h * 4 * 2 / 1024 + 8192;   // colour + depth, and room for the pass objects
            if (kib >= 0 && kib < needKib && s1 >= s0) {
                VGX_LOG_N(20, "dynamic resolution: level %d -> %d skipped: %d KiB of GPU memory free, %ld needed\n", d.level, l, kib, needKib);
                return false;
            }
            g_lastResize = g_poolPresents;
            g_poolFastTrim = 3;
        }
    }
    if (d.ema > 0) d.ema = d.ema * DynCost(l) / DynCost(d.level);   // the expected time at the new level (until it is measured)
    if (d.changes < 200)   // (the console's log: the level changes of a session, up to 200)
        std::fprintf(stderr, "[vgx] dynamic resolution: frame %llu level %d -> %d (scale %.2f, passes %.2f): %s; GPU %.1f ms, budget %.1f ms\n",
                     static_cast<unsigned long long>(g_execFrame), d.level, l, kDynLevels[l].s, kDynLevels[l].p, why, d.ema, DynBudgetMs(d.interval));
    d.level = l;
    d.ignoreUntil = d.seq + 3;   // frames already queued at the old size
    d.over = d.headroom = 0;
    ++d.changes;
    return true;
}

// 1.1 (console crash, 23 Oct): free video memory, read every 30 frames. A larger picture needs new, larger targets; with the
// pool nearly full their creation failed and the GPU set-up crashed. Levels above the game's own size need 32 MiB free, any step
// up 16 MiB, and below 12 MiB a picture larger than the game's own size goes back to it. MSC_DYNRES_VRAM=KiB simulates it.
int DynVramFreeKib(bool fresh) {
    static int cached = -1, age = 1000;
    static const long fake = EnvInt("MSC_DYNRES_VRAM", -1);
    if (fake >= 0) return static_cast<int>(fake);
    if (++age >= 30 || fresh) {
        age = 0;
        cached = vgx_vram_free_kib ? vgx_vram_free_kib() : -1;
    }
    return cached;
}
bool DynRoomFor(int l) {
    const int kib = DynVramFreeKib(true);
    if (kib < 0) return true;
    return kib >= (l < kDynNative ? 32768 : 16384);
}

void DynamicResolution() {
    DynRes& d = g_dyn;
    if (!d.inited) DynInit();
    const bool hint = g_megaHint.exchange(0, std::memory_order_relaxed) != 0;
    const int prevLevel = d.level, prevFixedW = g_fixedW, prevFixedH = g_fixedH;
    if (g_userFixedW) {   // (a fixed size: no levels, but Mega Strikes are still held at 30)
        if (hint) { d.megaQuiet = 0; d.mega = true; }
        else if (d.mega && ++d.megaQuiet > 20) d.mega = false;
        g_megaActive.store(d.mega ? 1 : 0, std::memory_order_relaxed);
        return;
    }
    const int interval = NpFpsDisplayInterval ? NpFpsDisplayInterval() : 2;
    const double budget = DynBudgetMs(interval);
    // the Mega Strike scene
    bool megaChanged = false;
    if (hint) {
        d.megaQuiet = 0;
        if (!d.mega) { d.mega = true; megaChanged = true; }
    } else if (d.mega && ++d.megaQuiet > 20) {
        d.mega = false;
        megaChanged = true;
    }
    g_megaActive.store(d.mega ? 1 : 0, std::memory_order_relaxed);
    const bool megaLevels = d.mega && d.megaMode == 0;
    // 1.1: at 60 fps ordinary scenes may go one step lower (0.78 per side, sharpened); at 30 they stop at 0.86. The frame-rate
    // controller is told the GPU time at the 60 floor, so it tries 60 only where that level would fit.
    const int floor30 = !d.on ? kDynNative : (megaLevels ? kDynLast : d.floorLevel);
    const int floor60 = !d.on ? kDynNative : (megaLevels ? kDynLast : std::max(d.floorLevel, d.floor60));
    const int floor = interval == 1 ? floor60 : floor30;
    const int best = (!d.on || d.mega) ? kDynNative : d.minLevel;
    if (megaChanged) {
        if (d.mega) {
            d.normalLevel = d.level;
            // With the effects drawn at the lower resolution (FX) the scene itself starts at the game's own size, the large passes at half;
            // without them (no depth texture, MSC_FX_LOWRES=0) the whole scene starts at 70 % as before.
            // With the effects at the lower resolution the Mega Strike starts with the scene at the game's own size (also from the
            // ordinary floor of a match); without them at 70 % as before.
            const bool fx = FxDiv() > 0 && FxDepthAvailable();
            const int start = fx ? 5 : kDynMegaStart;
            if (d.on && megaLevels && (fx ? d.level != start : d.level < start)) DynSetLevel(start, "a Mega Strike began", true);
            // 1.1: what an earlier Mega Strike learned (a smaller picture did not help) holds for the next ones: no step down and back
            d.noGainFloor = d.megaNoGain;
            d.probeAt = 0;
            d.frames = -10;
            d.windowStart = Now();
        } else {
            d.noGainFloor = -1;
            d.probeAt = 0;
            // 1.1: back to the level of the play before it, at the cut back to play. (It kept the Mega Strike's better level when that was
            // better, and the GPU then went over its budget a few seconds into play: a size change in the middle of play, which on the
            // console stalls the picture for up to 190 ms.)
            int back = d.normalLevel;
            if (back > d.floorLevel) back = d.floorLevel;
            if (back < d.minLevel) back = d.minLevel;
            DynSetLevel(back, "the Mega Strike is over", true);
        }
    }
    // the GPU time of the newest finished frame
    uint32_t seq = 0;
    float ms = 0;
    bool have = false;
    if (d.fake) {
        // two frames late, as the console's notifications are
        d.fakeRing[d.fakeSeq % 4] = d.fakeA + d.fakeB * DynCost(d.level) * (d.mega ? 4.0f : 1.0f);
        ++d.fakeSeq;
        if (d.fakeSeq > 2) { seq = d.fakeSeq - 2; ms = d.fakeRing[seq % 4]; have = true; }
    } else if (g_gpuTime) {
        have = g_gpuTime(&seq, &ms);
    }
    if (d.cycle > 0) {
        if (++d.cycleFrames >= d.cycle) {
            d.cycleFrames = 0;
            DynSetLevel((d.level + 1) % kDynCount, "MSC_DYNRES_CYCLE");
        }
    } else if (!d.on) {
        g_gpuMinMs.store(0.0f, std::memory_order_relaxed);
    } else if (have && seq != d.seq) {
        d.seq = seq;
        if (seq > d.ignoreUntil && ms > 0.05f) {
            d.ema = d.ema < 0 ? ms : 0.75f * d.ema + 0.25f * ms;
            const float cur = DynCost(d.level);
            auto pred = [&](int l) { return d.ema * DynCost(l) / cur; };
            g_gpuMinMs.store(pred(floor60), std::memory_order_relaxed);
            if (interval != d.interval) {
                // the frame rate changed: aim for the new budget at once
                d.interval = interval;
                d.over = d.headroom = 0;
            }
            if (ms > budget) ++d.over; else d.over = 0;
            if (d.upHold > 45 && d.seq - d.lastDownSeq > 1200) d.upHold = 45;   // stable for a while: tries come at the usual pace again
            if (d.noGainFloor >= 0 && !d.mega && d.seq - d.lastDownSeq > 900) d.noGainFloor = -1;   // (the scene has changed by then)
            const int vram = DynVramFreeKib();
            // judge the last step down once its frames are measured
            if (d.probeAt && d.seq >= d.probeAt) {
                d.probeAt = 0;
                // Only in a Mega Strike (where it was seen), and only when the time stayed about the same: a time that went UP is a scene change
                // (a replay, a cut) that a bigger picture would make worse, so the step stands.
                if (d.mega && d.probeFrom >= 0 && d.level > d.probeFrom && d.ema > 0.92f * d.probeEma && d.ema < 1.15f * d.probeEma) {
                    const int from = d.probeFrom;
                    if (DynSetLevel(from, "the smaller picture did not save GPU time", true)) {
                        d.noGainFloor = from;
                        d.megaNoGain = from;
                        d.lastDownSeq = d.seq;
                    }
                }
            }
            const int floorUsed = (d.noGainFloor >= 0 && d.noGainFloor < floor) ? d.noGainFloor : floor;
            if (d.level > floorUsed) DynSetLevel(floorUsed, "past this scene's floor");
            else if (d.level < best) DynSetLevel(best, "above this scene's best level");
            else if (d.level < kDynNative && vram >= 0 && vram < 12288) {
                if (DynSetLevel(kDynNative, "video memory is low", true)) {
                    // (as a level that did not hold: the next try up waits twice as long)
                    d.lastDownSeq = d.seq;
                    if (d.upHold < 720) d.upHold *= 2;
                }
            }
            else if (d.probeAt) {
                // (a step down is being judged: no further step until its frames are measured)
            } else if (d.ema > 0.90f * budget || d.over >= 3) {
                int l = d.level;
                while (l < floorUsed && pred(l) > 0.82f * budget) ++l;
                if (l == d.level && l < floorUsed && d.over >= 3) ++l;   // over the budget for three frames although predicted to fit: one more
                if (l != d.level) {
                    // a level gone up to that does not hold (the model does not know every saving, the effects at the lower resolution
                    // among them): wait twice as long before the next try, up to 16 times
                    if (d.lastUpSeq && d.seq - d.lastUpSeq < 180 && d.upHold < 720) d.upHold *= 2;
                    const int from = d.level;
                    const float emaBefore = d.ema;
                    if (DynSetLevel(l, "the GPU is over its budget")) {
                        d.lastDownSeq = d.seq;
                        if (d.mega) {
                            d.probeFrom = from;
                            d.probeEma = emaBefore;
                            d.probeAt = d.seq + 24;   // (3 frames still at the old size, then about 20 measured)
                        }
                    }
                } else {
                    d.over = 0;
                }
            } else if (d.level > best && pred(d.level - 1) < 0.72f * budget && DynRoomFor(d.level - 1)) {
                if (++d.headroom >= d.upHold) {
                    if (DynSetLevel(d.level - 1, "the GPU has room")) d.lastUpSeq = d.seq;
                }
            } else {
                d.headroom = 0;
            }
        }
    } else if (!have && d.on && megaLevels) {
        // no GPU time: the Mega Strike steps down while its frames come slower than 30 per second (20-frame windows)
        const double now = Now();
        if (++d.frames == 0) d.windowStart = now;
        else if (d.frames >= 20) {
            const double avgMs = (now - d.windowStart) / d.frames;
            const int next = (avgMs > 34.5) ? (d.level + 2 < kDynLast ? d.level + 2 : kDynLast) : d.level;
            if (next != d.level) { DynSetLevel(next, "Mega Strike frames slower than 30 per second"); d.frames = -10; }
            else { d.frames = 0; d.windowStart = now; }
        }
    }
    // a fixed Mega Strike size (MSC_MEGA_RENDER=WxH) replaces the levels for the scene
    const int fw = (d.mega && d.megaMode == 1) ? d.megaW : 0, fh = (d.mega && d.megaMode == 1) ? d.megaH : 0;
    const DynLevel& v = kDynLevels[d.level];
    g_passScale = d.on ? v.p : 1.0f;
    g_dynScale.store(fw ? static_cast<float>(fw) / static_cast<float>(g_efbW) : v.s, std::memory_order_relaxed);
    g_megaStep.store(d.mega ? (v.s <= 0.56f ? 3 : (v.s <= 0.62f ? 2 : 1)) : 0, std::memory_order_relaxed);
    g_fixedW = fw;
    g_fixedH = fh;
    int w = 0, h = 0;
    DynTargetSize(g_efbW, g_efbH, &w, &h);
    if (w == g_rtW && h == g_rtH) return;
    // The new target starts with the old one's picture, scaled: a frame begins on what the previous frame's display copy left in the EFB (its
    // clear colour and alpha, or the picture), and the game's effects read it (a target that started black made the whole scene of the first
    // frame at the new size dark). Depth starts cleared (1.0, what the display copy's clear leaves in practice).
    const GLuint oldColor = g_efbColor;
    const double resizeT0 = Now();
    // 1.1 (resize stalls): on the console most of a size change's cost is vitaGL making a GPU render target for the new size (14-42 ms
    // here, more in the frames after). The target of the size left is kept (EfbKeep), and a size used before is taken back as it was.
    bool created = false, keptOld = false;
    g_resizeReused = false;
    if (oldColor && g_efbFbo && EfbKeepOn()) {
        const EfbSlot cur{g_rtW, g_rtH, g_efbFbo, g_efbColor, g_efbDepthRb, g_efbDepthTex, g_poolPresents};
        EfbSlot hit{};
        if (EfbTake(w, h, hit)) {
            g_efbFbo = hit.fbo; g_efbColor = hit.color; g_efbDepthRb = hit.depthRb; g_efbDepthTex = hit.depthTex;
            g_rtW = hit.w; g_rtH = hit.h;
            ClearTargetDepth();
            created = true;
            ++g_efbReused;
            g_resizeReused = true;
        } else {
            g_efbFbo = 0; g_efbColor = 0; g_efbDepthRb = 0; g_efbDepthTex = 0;
            created = CreateTarget();
            if (!created) {   // (CreateTarget made the new objects: drop them, and back to the old ones)
                if (g_efbColor) { GLuint t = g_efbColor; G(glDeleteTextures(1, &t)); ForgetTex(t); }
                if (g_efbDepthTex) { GLuint t = g_efbDepthTex; G(glDeleteTextures(1, &t)); ForgetTex(t); }
                if (g_efbDepthRb) G(glDeleteRenderbuffers(1, &g_efbDepthRb));
                if (g_efbFbo) { if (g_inUseFbo == g_efbFbo) g_inUseFbo = ~0u; G(glDeleteFramebuffers(1, &g_efbFbo)); }
                g_efbFbo = cur.fbo; g_efbColor = cur.color; g_efbDepthRb = cur.depthRb; g_efbDepthTex = cur.depthTex;
                g_rtW = cur.w; g_rtH = cur.h;
                BindFbo(g_efbFbo);
            }
        }
        if (created) { EfbKeep(cur); keptOld = true; }
    } else {
        if (oldColor && g_efbFbo) G(glGenTextures(1, &g_efbColor));
        created = CreateTarget();
    }
    g_resizeCreateMs = static_cast<float>(Now() - resizeT0);
    if (!created && !keptOld) {
        // no memory for the new size: keep the old target and go back to the level of its size
        if (oldColor && g_efbColor != oldColor) {
            GLuint dead = g_efbColor;
            G(glDeleteTextures(1, &dead));
            ForgetTex(dead);
            g_efbColor = oldColor;
        }
        d.level = prevLevel;
        d.ignoreUntil = d.seq + 3;
        g_passScale = d.on ? kDynLevels[prevLevel].p : 1.0f;
        g_dynScale.store(kDynLevels[prevLevel].s, std::memory_order_relaxed);
        g_fixedW = prevFixedW;
        g_fixedH = prevFixedH;
        return;
    }
    if (oldColor && g_efbFbo && oldColor != g_efbColor) {
        BindFbo(g_efbFbo);
        SetViewport(0, 0, g_rtW, g_rtH);
        SetScissor(false);
        SetBlendOff();
        SetDepthTest(false);
        SetCull(CULL_NONE);
        SetDepthMask(false);
        SetColorMask(true, true);
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, kZero[4] = {0, 0, 0, 0};
        DrawQuad(oldColor, 0.0f, 0.0f, 1.0f, 1.0f, kIdentity, kZero);
        ++C.copyQuads;
        if (!keptOld) {
            GLuint dead = oldColor;
            G(glDeleteTextures(1, &dead));
            ++C.texDeletes;
            ForgetTex(dead);
        }
    }
    g_rsValid = false;
    // 1.1: what a size change costs (the console's log): the CPU time here and the next presents' spacing (DynResizeWatch)
    g_resizeCpuMs = static_cast<float>(Now() - resizeT0);
    g_resizeWatch = 5;   // (the first watch call is this frame's, right after: it only starts the clock)
}

// Logs the spacing of the four presents after a size change (called once per present).
void DynResizeWatch() {
    if (g_resizeWatch <= 0) return;
    static float gaps[4];
    const double now = Now();
    if (g_resizeWatch <= 4) gaps[4 - g_resizeWatch] = static_cast<float>(now - g_resizeLast);
    g_resizeLast = now;
    if (--g_resizeWatch == 0) {
        static int logged = 0;
        if (logged++ < 60)
            std::fprintf(stderr, "[vgx] size change to %dx%d: %.1f ms on the CPU (targets %.1f%s), next presents %.1f %.1f %.1f %.1f ms apart\n", g_rtW, g_rtH,
                         g_resizeCpuMs, g_resizeCreateMs, g_resizeReused ? ", kept target taken back" : "", gaps[0], gaps[1], gaps[2], gaps[3]);
    }
}
} // namespace

void SetGpuTimeSource(GpuTimeFn fn) { g_gpuTime = fn; }
float RenderScale() { return g_dynScale.load(std::memory_order_relaxed); }

void Execute(const CmdBuf& buf) { ExecuteEx(buf, ExecOpts{}); }

void ExecuteEx(const CmdBuf& buf, const ExecOpts& opts) {
    g_hidden = opts.hidden;
    g_execFrame = buf.frame;
    g_execLatencyTag=buf.latencyTag;
    const bool diag=NpDiagActive&&NpDiagActive();
    if(diag&&NpDiagGpuFrame)NpDiagGpuFrame(buf.latencyTag);
    if(diag)NpDiagEvent(NPD_BACKEND,opts.replay?1:0,buf.latencyTag,buf.frame,buf.draws.size(),buf.uploads.size(),buf.copyTex.size(),NpDiagInts(g_efbW,g_efbH));
    g_dbgExecCmd.store(0xFFFFFFF0u, std::memory_order_relaxed); // (before the stream upload)
    const DrawRec* const drawRecs = opts.draws ? opts.draws : buf.draws.data();
    const bool replay = opts.replay;
    // (MSC_VGX_VALIDATE checks a built frame once: not its interpolated replays.)
    g_validateFrame = g_validation && g_validation->Selected(buf.frame) && !replay;
    if (g_validateFrame) g_validationFrame = ValidationFrame{};
    // MSC_VGX_PROF=1: wall time per command type, glFinish after every copy (charges deferred
    // rasterisation to the copy; for the desktop). =2: no glFinish (the CPU side of the GL
    // calls only: what the render thread itself spends, which is the number to compare with
    // its budget on the console). =3: no per-command clock reads (the console's clock is a system call, about 6.7 us: 1/2 inflate all= by
    // 2 reads per command, see net=); the frame total, the stream upload and the call census only. All print every 100 frames.
    static const int profMode = static_cast<int>(EnvInt("MSC_VGX_PROF", 0));
    static const bool prof = profMode != 0;
    static const bool profCmd = profMode == 1 || profMode == 2;
    const double tFrame = prof ? Now() : 0.0;
    static const int64_t sceneLogFrame = EnvInt("MSC_VGX_SCENELOG", -1);
    g_sceneLog = sceneLogFrame >= 0 && static_cast<int64_t>(buf.frame) == sceneLogFrame;
    ++g_serial;
    EvictPrograms();
    g_stats.drawCallsThisFrame = 0;
    g_progsThisFrame = 0;
    // Attribute constants are given again in every frame (see the notes at the top): the
    // first draw sets its whole layout, constants included.
    S.attrConstSet = 0;
    S.layoutVbo = ~0u;
    if (!replay) { DynamicResolution(); DynResizeWatch(); }
    BindFbo(g_efbFbo);
    if (!replay || opts.restream) UploadStream(buf, opts.vtx ? opts.vtx : buf.vtx.data(), !replay);
    if (prof) g_profStream += Now() - tFrame;
    FxDecide(buf, drawRecs, g_dyn.mega, g_dyn.level);
    if (static const bool fxDebug = EnvInt("MSC_FX_DEBUG", 0) != 0; fxDebug && !replay) {
        unsigned flagged = 0, eligible = 0;
        for (const Cmd& c : buf.cmds)
            if (c.type == CMD_DRAW && drawRecs[c.arg].fx) { ++flagged; if (FxEligible(drawRecs[c.arg])) ++eligible; else if (flagged < 4) {
                const RenderState& r = drawRecs[c.arg].rs;
                std::fprintf(stderr, "[fx] frame %llu not eligible: blend %u eq %u src %u dst %u zwrite %u dstAlpha %u\n", static_cast<unsigned long long>(buf.frame), r.blend, r.blendEq, r.blendSrc, r.blendDst, r.depthWrite, r.dstAlphaPass); } }
        if (flagged && (buf.frame % 50) == 0)
            std::fprintf(stderr, "[fx] frame %llu: %u effect draws, %u eligible, buffer uses FX %d\n", static_cast<unsigned long long>(buf.frame), flagged, eligible, g_fxBuf ? 1 : 0);
    }
    if (RtMode() > 0) {
        // W5 RT: the buffer's EFB depth scan; then the previous buffer's display-copy clear (it begins this frame's main EFB scene)
        RtScan(buf, drawRecs);
        g_rt.simDepth = (buf.frame & 1) ? 0.0f : 0.37f;
        // (the previous buffer's display-copy clear, if one is pending, is made just before command g_rt.clearAt: see the loop)
    } else {
        g_rt.scanned = false;
    }
    // Shader work of this buffer (see SetProgressDisplay in vgx_cmd.h). Units are weighted by
    // what they cost on the console: a compile is hundreds of milliseconds, a link and a
    // patched variant are a few.
    g_sw = ShaderWork{};
#if !defined(__VGX_VITA__)
    g_progFrameNow = buf.frame;
#endif
    for (const Cmd& c : buf.cmds) g_sw.total += c.type == CMD_SHADER ? 10u : (c.type == CMD_PROGRAM ? 2u : (c.type == CMD_VARIANT ? 1u : 0u));
    uint32_t dbgIdx = 0;
    const DrawRec* prevDraw = nullptr; // the previous command if it was a draw
    // W5 GLB, MSC_VGX_GLMERGE=1: consecutive draws that ContinuesDraw() reports as 2 (the same state and uniform values, the next indices of the same buffer, triangle
    // lists) are issued as one glDrawElements: the draw is held until a command that is not a draw (or a draw that does not continue it) comes. Off by default.
    static const bool mergeDraws = EnvInt("MSC_VGX_GLMERGE", 0) != 0;
    static const bool pairCensus = EnvInt("MSC_VGX_PAIRDIAG", 0) != 0 || std::getenv("MSC_VGX_GLSTATS") != nullptr;
    DrawRec pend;
    bool havePend = false;
    int pendIdx = -1;
    auto flushPend = [&]() {
        if (!havePend) return;
        havePend = false;
        const int keep = g_rt.cmd;
        g_rt.cmd = pendIdx; // (W5 RT: the scene decisions are made for the command the draw came from)
        RtTimedDraw(buf, pend);
        g_rt.cmd = keep;
    };
    for (const Cmd& c : buf.cmds) {
        g_rt.cmd = static_cast<int>(&c - buf.cmds.data());
        g_dbgExecCmd.store((static_cast<uint32_t>(c.type) << 24) | (dbgIdx++ & 0xFFFFFFu), std::memory_order_relaxed);
        const double t0 = profCmd ? Now() : 0.0;
        const bool diagCommand=diag&&(c.type==CMD_COPY_TEX||c.type==CMD_COPY_DISP||c.type==CMD_TEX_UPLOAD||c.type==CMD_SHADER||c.type==CMD_PROGRAM||c.type==CMD_SET_TARGET||c.type==CMD_PASS_BEGIN);
        const uint64_t diagBegin=diagCommand?NpDiagNow():0;
        const uint32_t swUnits = g_sw.total ? (c.type == CMD_SHADER ? 10u : (c.type == CMD_PROGRAM ? 2u : (c.type == CMD_VARIANT ? 1u : 0u))) : 0u;
        const double swT0 = swUnits ? NowMs() : 0.0;
        if (c.type != CMD_DRAW) {
            prevDraw = nullptr;
            flushPend();
        }
        if (g_rt.havePending && g_rt.scanned &&
            ((g_rt.cmd == g_rt.clearAt && !g_rt.clearInCopy) || (g_rt.clearInCopy && g_rt.cmd == g_rt.clearAt + 1))) {
            // W5 RT: the previous buffer's display-copy clear, just before the first command that uses the EFB (a copy that was to make it
            // inside, before its own clear, and did not: before the next command)
            flushPend();
            prevDraw = nullptr;
            g_rt.havePending = false;
            ++C.rtDeferredClears;
            g_rt.beforeCmd = true;
            Clear(g_rt.pendingClear, g_rt.pendingRect);
            g_rt.beforeCmd = false;
        }
        // FX: a run of additive effect draws goes into the FX target; the first command that is not one of them adds it to the scene
        if (g_fxBuf || g_fxOpen) {
            const bool fxDraw = FxCmd(g_rt.cmd);
            if (g_fxOpen && !fxDraw && !FxNeutral(c.type)) {
                flushPend();
                prevDraw = nullptr;
                EndFx();
            }
            if (fxDraw) {
                if (!g_fxOpen) {
                    flushPend();
                    prevDraw = nullptr;
                    BeginFx();
                }
                if (g_fxOpen) {
                    if (drawRecs[c.arg].rs.alphaWrite) g_fxAlpha = true;
                    ++C.fxDraws;
                }
            }
        }
        // (a replay only draws, copies and clears)
        if (replay && c.type != CMD_DRAW && c.type != CMD_PASS_BEGIN && c.type != CMD_COPY_TEX && c.type != CMD_COPY_DISP && c.type != CMD_PRESENT) continue;
        switch (c.type) {
        case CMD_DRAW: {
            const DrawRec& dr = drawRecs[c.arg];
            if (mergeDraws && !g_validateFrame) {
                if (havePend && ContinuesDraw(pend, dr, buf.uni) == 2) {
                    pend.idxCount += dr.idxCount;
                    pend.vtxCount += dr.vtxCount;
                    ++C.mergedDraws;
                    break;
                }
                flushPend();
                pend = dr;
                pendIdx = g_rt.cmd;
                havePend = true;
                break;
            }
            if (prevDraw && pairCensus) {
                const int k = ContinuesDraw(*prevDraw, dr, buf.uni);
                if (k == 2) ++C.mergeable; else if (k == 1) ++C.sameStateGap;
                PairCensus(*prevDraw, dr, buf.uni);
            }
            prevDraw = &dr;
            RtTimedDraw(buf, dr);
            break;
        }
        case CMD_TEX_UPLOAD: Upload(buf.uploads[c.arg]); break;
        case CMD_TEX_FREE:
            if (c.arg < g_tex.size() && g_tex[c.arg].id) {
                DeleteTex(g_tex[c.arg]);
                --g_stats.textures;
            }
            break;
        case CMD_SHADER: {
            const ShaderSource* s = buf.shaders[c.arg];
            CompileShaderSlot(*s, true);
            if (s->fragment) ++g_stats.fragmentShaders; else ++g_stats.vertexShaders;
            break;
        }
        case CMD_BUF_UPLOAD: {
            const BufUpload& u = buf.bufUploads[c.arg];
            if (g_validation) g_validation->Upload(u);
            if (g_sbuf.size() <= u.bufId) g_sbuf.resize(u.bufId + 64);
            StaticBuf& sb = g_sbuf[u.bufId];
            if (!sb.vbo) { G(glGenBuffers(1, &sb.vbo)); G(glGenBuffers(1, &sb.ibo)); }
            BindArrayBuf(sb.vbo);
            G(glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(u.vtx.size()), u.vtx.data(), g_staticUsage));
            BindElemBuf(sb.ibo);
            G(glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(u.idx.size() * 2), u.idx.data(), g_staticUsage));
            C.staticUploads += 2;
            C.staticBytes += static_cast<uint32_t>(u.vtx.size() + u.idx.size() * 2);
            if (S.layoutVbo == sb.vbo) S.layoutVbo = ~0u; // new contents: pointers are set again
            break;
        }
        case CMD_BUF_FREE:
            if (g_validation) g_validation->Free(c.arg);
            if (c.arg < g_sbuf.size() && g_sbuf[c.arg].vbo) {
                StaticBuf& sb = g_sbuf[c.arg];
                G(glDeleteBuffers(1, &sb.vbo));
                G(glDeleteBuffers(1, &sb.ibo));
                // The names may be handed out again; a deleted buffer is also unbound by the GL.
                if (S.arrayBuf == sb.vbo) S.arrayBuf = ~0u;
                if (S.elemBuf == sb.ibo) S.elemBuf = ~0u;
                if (S.layoutVbo == sb.vbo) S.layoutVbo = ~0u;
                sb = StaticBuf{};
            }
            break;
        case CMD_PASS_BEGIN: BeginPass(buf.passes[c.arg]); break;
        case CMD_PROGRAM:
            if (WarmRoom(static_cast<uint16_t>(c.arg >> 16), static_cast<uint16_t>(c.arg & 0xFFFFu))) GetProgram(static_cast<uint16_t>(c.arg >> 16), static_cast<uint16_t>(c.arg & 0xFFFFu));
            break;
        case CMD_VARIANT: WarmVariant(buf.variants[c.arg]); break;
        case CMD_COPY_TEX: CopyTex(buf.copyTex[c.arg]); break;
        case CMD_COPY_DISP: CopyDisp(buf.copyDisp[c.arg]); break;
        case CMD_PRESENT:
            for (PoolFbo& f : g_pool) f.uses = 0;
            TrimPool();
            g_stats.programsUsedThisFrame = g_progsThisFrame;
            break;
        case CMD_SET_TARGET:
            g_efbW = buf.targets[c.arg].efbW;
            g_efbH = buf.targets[c.arg].efbH;
            CreateTarget();
            BindFbo(g_efbFbo);
            g_rsValid = false;
            break;
        default: break;
        }
        if (swUnits) {
            const double dt = NowMs() - swT0 + g_sw.simPending;
            g_sw.simPending = 0;
            g_sw.ms += dt;
            g_sw.sinceShow += dt;
            g_sw.done += swUnits;
            if (g_sw.ms >= 150.0 && g_sw.done < g_sw.total && g_sw.sinceShow >= 40.0) {
                if (RtProgressAllowed()) {
                    ShowProgress();
                    g_sw.sinceShow = 0;
                } else {
                    ++C.rtHeldProgress; // (W5 RT: shown after the open pass or the EFB scene that does not keep its depth)
                }
            }
        }
        if(diagCommand){
            uint64_t detail=0;
            if(c.type==CMD_COPY_TEX){const auto& t=buf.copyTex[c.arg];detail=NpDiagInts(t.dstW,t.dstH);}
            NpDiagEvent(NPD_COMMAND,c.type,buf.latencyTag,diagBegin,NpDiagNow(),c.arg,detail,NpDiagInts(g_efbW,g_efbH));
        }
        if (profCmd) {
            if (profMode == 1 && (c.type == CMD_COPY_TEX || c.type == CMD_COPY_DISP)) glFinish(); // charge deferred rasterisation here
            g_prof[c.type] += Now() - t0;
            ++g_profCount[c.type];
            ++g_profCmds;
        }
    }
    flushPend();
    if (g_fxOpen) EndFx();
    g_stats.shaderWorkMs += g_sw.ms;
    if(diag)NpDiagEvent(NPD_BACKEND_END,0,buf.latencyTag,buf.frame,g_sw.compiles,g_sw.links,g_evictions,g_evicted);
    if(diag&&NpDiagGpuFrame)NpDiagGpuFrame(0);
    // One line for every frame whose shader work would have been seen as a hitch (always
    // printed: on the console this is the record of what start-up and first use cost).
    if (g_sw.ms >= 100.0 || g_sw.compiles >= 8)
        std::fprintf(stderr, "[vgx] frame %llu: %u shader compiles (%u answered by the simulated cache), %u program links, %.0f ms of shader work, progress bar shown %u times\n",
                     static_cast<unsigned long long>(buf.frame), g_sw.compiles, g_sw.simHits, g_sw.links, g_sw.ms, g_sw.shown);
    if (prof) {
        g_profTotal += Now() - tFrame;
#define X(n) g_profSum[k++] += C.n;
        { int k = 0; VGX_GL_COUNTERS(X) }
#undef X
        if (++g_profFrames % 100 == 0) {
            static const char* const nm[CMD_TYPE_COUNT] = {"draw", "upload", "free", "shader", "copytex", "copydisp", "present", "target", "bufupload",
                                                           "buffree", "passbegin", "program", "variant"};
            std::fprintf(stderr, "[vgx] prof (ms/frame over 100 frames%s): all=%.2f stream=%.2f", profMode == 1 ? ", glFinish after copies" : (profMode == 3 ? ", PROF=3 frame totals only" : ""),
                         g_profTotal / 100.0, g_profStream / 100.0);
            if (profCmd) {
                // The clock reads are in all= (two per command, and the frame's): take them out.
                const double k = ClockCostMs();
                std::fprintf(stderr, " clock=%.2fus net=%.2f", k * 1000.0, (g_profTotal - (2.0 * static_cast<double>(g_profCmds) + 2.0 * 100.0) * k) / 100.0);
                g_profCmds = 0;
            } else {
                const double k = ClockCostMs();
                std::fprintf(stderr, " clock=%.2fus net=%.2f", k * 1000.0, (g_profTotal - 2.0 * 100.0 * k) / 100.0);
            }
            for (int i = 0; i < CMD_TYPE_COUNT; ++i) {
                if (g_profCount[i]) std::fprintf(stderr, " %s=%.2f(x%.1f)", nm[i], g_prof[i] / 100.0, g_profCount[i] / 100.0);
                g_prof[i] = 0;
                g_profCount[i] = 0;
            }
            {
                // The render thread's own interval (VRS): with the player on this thread (the default) the interval is
                // all= (this back end) + idle= (waiting for records) + decode and whatever blocks outside the back end;
                // with MSC_VGX_BACKEND_THREAD=1 idle= is the player thread's, not this one's.
                static double tLastPrint = 0.0;
                const double tNow = Now();
                const double idle = rec::TakePlayerIdleMs();
                if (tLastPrint > 0.0)
                    std::fprintf(stderr, " | thread: period=%.2f idle=%.2f rest=%.2f", (tNow - tLastPrint) / 100.0, idle / 100.0,
                                 (tNow - tLastPrint - idle - g_profTotal) / 100.0);
                // MSC_VGX_BACKEND_THREAD=1 only: slotwait= the player's wait for the back end to take the previous frame (the player is faster than
                // this thread), beidle= this thread's wait for the player's next frame (this thread is faster). Decode = period - idle - slotwait.
                double slotWait = 0.0, beIdle = 0.0;
                if (rec::TakePipeWaitMs(&slotWait, &beIdle) && tLastPrint > 0.0)
                    std::fprintf(stderr, " slotwait=%.2f beidle=%.2f", slotWait / 100.0, beIdle / 100.0);
                tLastPrint = tNow;
            }
            std::fprintf(stderr, "\n[vgx] per frame:");
            static const char* const cn[] = {
#define X(n) #n,
                VGX_GL_COUNTERS(X)
#undef X
            };
            for (size_t i = 0; i < sizeof cn / sizeof cn[0]; ++i) {
                if (g_profSum[i]) std::fprintf(stderr, " %s=%.1f", cn[i], static_cast<double>(g_profSum[i]) / 100.0);
                g_profSum[i] = 0;
            }
            std::fprintf(stderr, "\n");
#if defined(__VGX_VITA__) && defined(VITAGL_GLB)
            vgx_vita_log_glb_stats();
#endif
            g_profTotal = g_profStream = 0;
        }
    }
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) VGX_LOG_N(32, "GL error 0x%X after frame %llu command buffer\n", err, static_cast<unsigned long long>(buf.frame));
    WriteCounts(buf.frame);
    if (g_validation) {
        if (g_validateFrame) {
            const ValidationFrame& total = g_validationFrame;
            std::fprintf(stderr, "[validate] frame=%llu summary draws=%llu bad_draws=%llu unchecked_draws=%llu references=%llu vertex_bytes=%llu index_bytes=%llu uniform_bytes=%llu hash=%016llx shadow_bytes=%llu shadow_drops_total=%u detail_lines=%llu CPU-contract-only\n",
                         static_cast<unsigned long long>(buf.frame), static_cast<unsigned long long>(total.draws), static_cast<unsigned long long>(total.badDraws),
                         static_cast<unsigned long long>(total.uncheckedDraws), static_cast<unsigned long long>(total.references), static_cast<unsigned long long>(total.vertexBytes), static_cast<unsigned long long>(total.indexBytes),
                         static_cast<unsigned long long>(total.uniformBytes), static_cast<unsigned long long>(total.hash), static_cast<unsigned long long>(g_validation->ShadowBytes()),
                         g_validation->shadowDrops, static_cast<unsigned long long>((total.draws < 16 ? total.draws : 16) + total.extraLines));
            std::fflush(stderr);
        }
        g_validation->Finish(buf.frame);
        if (!g_validation->Enabled()) g_validation.reset();
    }
    g_validateFrame = false;
#ifdef VGX_GL_STUB
    ReportGlMem(buf.frame);
#endif
    C = Counts{};
    // The host presents (and swaps) between frames: whatever is drawn next starts a scene.
    g_inUseFbo = ~0u;
}

const std::vector<uint8_t>& LastDisplayCopy(int& width, int& height) {
    width = g_capW;
    height = g_capH;
    return g_captured;
}
void RequestCapture(bool enable) { g_capture = enable; }
void SetPresentHook(PresentHook hook) { g_presentHook = hook; }
void SetSceneHintHook(SceneHintHook hook) { g_hintHook = hook; }
void SetMicroClock(MicroClock clock) { g_microClock = clock; }
void GetDrawTimers(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = g_rtDrawTimers[i]; }
uint64_t ExecutingFrame() { return g_execFrame; }
uint64_t ExecutingLatencyTag() { return g_execLatencyTag; }
void SetProgressDisplay(unsigned framebuffer, int width, int height, ProgressShowHook show) {
    g_dispFbo = framebuffer;
    g_dispW = width;
    g_dispH = height;
    g_showHook = show;
}
void SetSkipDraws(bool skip) { g_skipDraws = skip; }
uint32_t DbgExecCmd() { return g_dbgExecCmd.load(std::memory_order_relaxed); }
bool CaptureRequested() { return g_capture; }
bool SkipDrawsRequested() { return g_skipDraws; }
Stats GetStats() { return g_stats; }

} // namespace vgx::backend

#ifdef VGX_GL_STUB
namespace vgx::host {
bool CreateContext() { return true; }
}
#endif

// The game's Mega Strike hint (see MegaResolution): called on the game thread every frame the Mega background is drawn.
extern "C" void VgxMegaSceneHint(int on) {
    if (on) vgx::backend::g_megaHint.store(1, std::memory_order_relaxed);
}
extern "C" int VgxMegaStep(void) { return vgx::backend::g_megaStep.load(std::memory_order_relaxed); }
// DYNRES: the render scale in force (the host's sharpening follows it) and the GPU time per frame predicted at this scene's smallest picture (the
// frame-rate controller, np_fps_ctl.h; 0: not known)
extern "C" float VgxRenderScale(void) { return vgx::backend::RenderScale(); }
extern "C" float VgxGpuMinMs(void) { return vgx::backend::g_gpuMinMs.load(std::memory_order_relaxed); }
extern "C" int VgxMegaActive(void) { return vgx::backend::g_megaActive.load(std::memory_order_relaxed); }
