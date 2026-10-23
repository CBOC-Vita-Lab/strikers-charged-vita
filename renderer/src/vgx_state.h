// Front-end GX state: the hardware register file (BP / XF / CP) plus the
// bookkeeping the HLE-level API adds on top (texture objects, vertex arrays).
// API calls are encoded into register writes exactly like the SDK does, and
// command streams (display lists) are parsed into the same writes, so there
// is a single source of truth for both paths.
#pragma once
#include "vgx_cmd.h"
#include "vgx_shadergen.h"

namespace vgx {

struct VtxAttrFmt {
    uint8_t cnt = 1, type = 4, frac = 0; // GXCompCnt, GXCompType
};
struct ArrayRef {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint32_t stride = 0;
    bool le = false;
};

// Host-side layout of GXTexObj / GXTlutObj / GXLightObj. The public structs
// are opaque word arrays (96 / 48 / 64 bytes); the runtime only stores them.
struct TexObj {
    const void* data;
    uint16_t width, height;
    uint32_t format;
    uint8_t wrapS, wrapT, minFilter, magFilter;
    uint8_t mipmap, pad[3];
    float minLod, maxLod, lodBias;
    uint32_t tlut;
    void* userData;
    uint32_t dataVersion; // bumped by GXInitTexObj*/GXInitTexObjData: "contents may have changed"
    uint32_t magic;
};
struct TlutObj {
    const void* data;
    uint32_t format;
    uint16_t entries;
    uint16_t pad;
    uint32_t version;
};
struct LightObj {
    uint32_t pad[3];
    uint32_t color; // RGBA8
    float a[3], k[3], pos[3], dir[3];
};

struct TexSlot { // what GXLoadTexObj bound to a texmap
    TexObj obj{};
    bool valid = false;
    uint32_t loadSerial = 0;
};
struct TlutSlot {
    TlutObj obj{};
    bool valid = false;
};

// How one vertex format's command-stream bytes map onto the decoded layout.
struct AttrPlan {
    uint8_t attr, mode, type, cnt, frac;
    uint8_t srcSize;  // bytes consumed from the stream
    uint8_t elemSize; // bytes of one array element (indexed)
    uint8_t outOff;
    uint8_t comps;
};
struct Plan {
    bool valid = false;
    int n = 0;
    AttrPlan a[21];
    uint32_t streamSize = 0;
    uint32_t layoutMask = 0;
    VtxLayout layout;
    int callsPerVertex = 0; // immediate mode
};
struct PlanCache { Plan p[8]; };
// Plans are cached per vertex format until the descriptor / formats change
// (`dirty`). `recordStats` must only be set on the player's thread.
const Plan& GetPlanIn(PlanCache& cache, bool& dirty, const uint8_t* vcd, const VtxAttrFmt (*vat)[26], int fmt, bool recordStats);

struct State {
    // ---- BP
    uint32_t bp[256] = {};
    uint32_t bpMask = 0x00FFFFFF;
    int tevReg[4][4] = {};  // PREV, REG0..2 as r,g,b,a (s11)
    int tevKonst[4][4] = {};
    // ---- XF
    float xfPos[64][4] = {};  // 0x0000..0x00FF
    float xfNrm[32][3] = {};  // 0x0400..0x045F
    float xfPost[64][4] = {}; // 0x0500..0x05FF
    uint32_t xfLight[8][16] = {}; // 0x0600..0x067F
    uint32_t xfReg[0x58] = {};    // 0x1000..0x1057
    // ---- CP
    uint8_t vcd[26] = {};
    VtxAttrFmt vat[8][26] = {};
    ArrayRef arrays[26] = {};
    uint32_t matIdxA = 0x3CF3CF00u | 0, matIdxB = 0x00F3CF3Cu; // identity texture matrices, PNMTX0
    // ---- API-level state
    TexSlot tex[8];
    TlutSlot tlut[20];
    uint16_t manualScale[8][2] = {};
    uint8_t manualScaleOn = 0;
    EfbRect dispCopySrc{0, 0, 640, 480};
    EfbRect texCopySrc{0, 0, 640, 480};
    uint16_t dispCopyDstW = 640, dispCopyDstH = 480;
    uint16_t texCopyDstW = 0, texCopyDstH = 0;
    uint32_t texCopyFmt = 6;
    bool texCopyHalf = false;
    uint16_t efbW = 640, efbH = 528; // logical EFB size (VIConfigure)
    // ---- dirty tracking (coarse: gates rebuilding keys / blocks, which are
    // then compared against the previous ones before anything is emitted)
    bool bpDirty = true;      // a BP register the TEV uniform block is built from
    bool bpKeyDirty = true;   // a BP register the fragment key is built from (then the render state and the TEV block are rebuilt too)
    bool bpRsDirty = true;    // a BP register only the render state is built from (scissor, depth mode, line width)
    bool xfRegDirty = true;   // an XF register not covered by the flags below: everything
    bool xfKeyDirty = true;   // channel control, texgens: vertex / fragment key
    bool xfViewDirty = true;  // viewport: render state
    bool xfProjDirty = true;  // projection
    bool xfColorDirty = true; // ambient / material colours: light block
    bool xfMtxDirty = true;   // matrix memory, matrix indices
    bool xfLightDirty = true; // light memory
    bool layoutDirty = true;  // VCD / VAT
    bool texDirty = true;     // texture bindings
    uint8_t texSlotDirty = 0xFF; // which texmaps' bindings (or what they resolve to) may have changed
};
extern State g;

// ---- register writes (vgx_state.cpp)
void WriteBP(uint32_t regAndValue);            // bits 24..31 = register
inline void WriteBP(uint8_t reg, uint32_t v) { WriteBP((static_cast<uint32_t>(reg) << 24) | (v & 0x00FFFFFFu)); }
void SetBPField(uint8_t reg, int shift, int count, uint32_t value); // read-modify-write helper for the API
void WriteXF(uint32_t addr, uint32_t count, const uint32_t* hostWords);
void WriteXFBE(uint32_t addr, uint32_t count, const uint8_t* beWords);
inline void WriteXFReg(uint32_t addr, uint32_t v) { WriteXF(addr, 1, &v); }
void WriteCP(uint8_t reg, uint32_t value);
void SetVcd(int attr, int type);
void SetVat(int fmt, int attr, int cnt, int type, int frac);
extern uint64_t g_coPlanarOn;                   // genMode bit 19 (Z-freeze) switched on, counted for MSC_VGX_ZFTRACE
void StoreBP(uint8_t reg, uint32_t value);      // final register value (the recorder applied the mask already)
void InitState(State& s);                      // power-on values
void ResetState();

// ---- geometry (vgx_state.cpp)
void ProcessStream(const uint8_t* data, uint32_t nbytes);
void CallList(const uint8_t* list, uint32_t nbytes); // a display list in guest RAM: through the geometry cache (vgx_dlcache.cpp)
void DlCacheReport(FILE* f);
void DrawStream(uint8_t prim, int fmt, const uint8_t* data, uint32_t count, uint32_t availBytes);
// Byte ranges of the vertex arrays (attributes POS..TEX7) a decode read.
struct ArraySpans {
    uint32_t mask = 0; // bit per attribute number
    uint32_t lo[12] = {}, hi[12] = {};
};
const Plan& CurrentPlan(int fmt); // the player's plan for a vertex format under the current descriptor
void DecodeVertices(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, ArraySpans* spans);
uint32_t StreamVertexSize(int fmt);
// W5 SKIN (vgx_ext.h): can draws of this plan carry the skin state (their position is indexed), and the in-place widening of decoded vertices to the
// skinned layout (p.layoutMask | VL_SKIN; the buffer must hold count vertices of the wider layout). Uses g_skin.
bool SkinPosIndexed(const Plan& p);
void SkinExpand(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* v);
void SkinNoteMissed(uint32_t count);
void DrawDecoded(uint8_t prim, uint32_t layoutMask, const uint8_t* verts, uint32_t count); // vertices already in the decoded layout

// ---- draw resolution and command buffer (vgx_draw.cpp)
// Reserves room for decoded vertices; EmitDraw turns them into a DrawRec with
// the current state. Vertex data is written between the two calls.
uint8_t* BeginVertices(uint32_t layoutMask, uint32_t count);
void EmitDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count);
// A primitive whose vertices and indices already sit in a static buffer.
// vtxFirst: what the buffer's indices for this primitive were offset by (see "vertex addressing" in vgx_draw.cpp).
void EmitStaticDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t bufId, uint32_t vtxByteOffset, uint32_t idxOffset,
                    uint32_t vtxFirst);
bool AbsoluteIndices(); // indices count from the start of a buffer (default) / of the draw (MSC_VGX_ABSIDX=0)
// Before EmitDraw / EmitStaticDraw: the highest position-matrix row the primitive's vertices
// name (0 when they carry no matrix index). Not said: the full matrix palette is used.
void SetPendingPosMtxMax(uint32_t row);
uint32_t LastDecodedPosMtxMax(); // of the last DecodeVertices call
// Number of indices a GX primitive of `count` vertices turns into; writes them (relative to `base`) when out != null.
uint32_t PrimIndices(uint8_t gxPrim, uint32_t count, uint32_t base, uint16_t* out);
uint32_t SoftXformLimit(); // primitives up to this many vertices may be transformed on the CPU (and merged)
bool LastDrawNeedsArena(); // the draw EmitDraw resolved last was transformed on the CPU, or its vertices feed the Z-freeze reference (MSC_VGX_PROMOSMALL)
// W5 SKIN soft instancing (MSC_VGX_SOFTINST): the display-list cache offers a segment before copying it into the arena; true = taken (or culled).
bool SoftInstEmit(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, const uint8_t* v);
bool SoftInstOn();
extern bool g_streamCensusList; // MSC_VGX_STREAMCENSUS: true while the display-list cache replays a list into the stream (vgx_dlcache.cpp sets it)
// MSC_VGX_HASH=2 needs the bytes of a static buffer to hash what a draw refers to (vgx_dlcache.cpp).
bool StaticBufferData(uint32_t bufId, const uint8_t*& vtx, const uint16_t*& idx);
void QueueCopyTex(const void* dest, bool clear);
void QueueCopyDisp(bool clear);
// Ends the frame on the player's side: closes the command buffer and hands it
// to the back end. `direct`: the caller is the render thread (the back end is
// run in place); otherwise the buffer goes through SubmitToBackend.
// `onDecoded` is called between the two, when nothing of the frame is left to
// be read from guest memory.
void FrontPresent(bool direct, void (*onDecoded)(), uint64_t latencyTag = 0);
void DlCacheDiagnostic(uint64_t tag);
// When set, a finished frame's command buffer is handed to this function instead of being
// executed (or submitted) by FrontPresent. It may swap the buffer for an empty one and return
// before the frame has been drawn (vgx_play.cpp: back end on its own thread).
void SetFrameSink(void (*sink)(CmdBuf& buf));
void QueueSetTarget();
CmdBuf& Pending();
uint64_t CurrentFrame();
bool TraceFrame(); // MSC_VGX_TRACE=<frame>

// ---- textures (vgx_texcache.cpp)
// Resolves what is bound on a texmap to a back-end texture id, decoding and
// queueing an upload when the guest data is new or changed.
bool ResolveTexture(int texMap, TexBind& out, uint16_t& width, uint16_t& height);
// Shader warm-up list (vgx_draw.cpp): every shader key and program pair that is ever needed is
// appended to this file, and the next run generates and links all of them in its first frame
// instead of in the middle of play. Call before the first frame (or MSC_VGX_SHADERLIST=<path>).
void SetShaderListFile(const char* path);
void PassCheckReport(FILE* f);           // result of MSC_VGX_PASSCHECK=1, for the statistics report
void TexCacheEndFrame();                 // frees least recently used textures when over the budget
void SetTextureBudget(uint64_t bytes);   // RGBA8 bytes of guest textures to keep; 0 = unlimited (also MSC_VGX_TEXBUDGET=<MiB>)
uint32_t AcquireCopyTexture(const void* dest, uint16_t w, uint16_t h, uint32_t fmt);
uint32_t AllocTextureId(); // a back-end texture id that belongs to no guest texture (copy atlases)
void DestroyCopyTexture(const void* dest);
void InvalidateAllTextures();
void TexCacheReport(FILE* f);
void CopyCheckTexReport(); // MSC_VGX_COPYCHECK=1
void SetGuestWriteGenerationHook(uint64_t (*fn)(const void*, size_t));
// Optional: called on the player's thread for every texture upload (texture, GX format, frame).
void SetTextureDumpHook(void (*fn)(const TexUpload& up, uint32_t gxFormat, uint64_t frame));

// ---- statistics (vgx_stats.cpp)
namespace stats {
void TexFormat(uint32_t fmt, uint16_t w, uint16_t h, bool mip);
void CopyFormat(uint32_t fmt, bool disp);
// The per-draw bookkeeping below (feature usage, distinct textures / programs per frame) is
// only done while Detailed() is on: the host layer turns it on when a statistics report or the
// per-frame CSV is asked for. It costs about 6% of the player's time otherwise.
void SetDetailed(bool on);
extern bool g_detailed;
inline bool Detailed() { return g_detailed; }
void Draw(const FsKey& fs, const VsKey& vs, uint32_t layoutMask, uint32_t verts);
void VertexFormat(int attr, int type, const VtxAttrFmt& f);
void Note(const char* what); // one-off feature flag, e.g. "indirect"
void ShaderCreated(bool fragment, uint64_t frame);
void FrameEnd(uint64_t frame,uint64_t tag=0);
void CloseFrameLog();
void Dump(const char* path);
// Per-frame counters (one CSV line per presented frame, MSC_VGX_FRAMESTATS=<file>).
struct Frame {
    uint32_t prims = 0;        // GX primitives submitted (GXBegin / draw opcodes)
    uint32_t primsDL = 0;      // of those, from display lists / raw streams
    uint32_t vertsDL = 0;      // vertices of those
    uint32_t verts = 0;        // vertices decoded
    uint32_t tris = 0;         // triangles produced
    uint32_t drawCalls = 0;    // draw records after merging (= GL draw calls)
    uint32_t streams = 0;      // command streams (display lists, XF packets) parsed
    uint32_t streamBytes = 0;
    uint32_t texUploads = 0, texUploadBytes = 0; // texture bytes handed to the back end
    uint32_t texHashes = 0, texHashBytes = 0;   // re-validations of cached textures by content hash
    uint32_t texUntracked = 0;                  // of those: data outside tracked guest RAM (hashed on every bind)
    uint32_t texEvicted = 0, texEvictBytes = 0; // freed to stay inside the texture budget
    uint32_t copies = 0;       // EFB-to-texture copies
    uint32_t copyPixels = 0;   // destination texels of those
    uint32_t deadCopies = 0;   // of the copies: never sampled before being overwritten (only their clear is executed)
    uint32_t passes = 0;       // of the copies: end of an isolated offscreen pass (source is the pass target)
    uint32_t passDraws = 0;    // draws inside isolated passes
    uint32_t passesDirect = 0; // of the passes: can be drawn straight into the copy's destination texture
    uint32_t fills = 0;        // of the copies: source rectangle known to be a constant colour
    uint32_t fillsRedundant = 0; // of the fills: the texture already holds exactly that (nothing is executed but the clear)
    uint32_t efbReads = 0;     // of the copies: the remaining ones, which really read the EFB
    uint32_t clearsRedundant = 0; // clears (of copies) not executed: the rectangle already holds exactly those values
    uint32_t atlasPasses = 0;  // of the direct passes: drawn into a cell of a copy atlas
    uint32_t atlasRemaps = 0;  // draws whose texture coordinates were rewritten to sample an atlas cell
    uint32_t atlasMerged = 0;  // draw records merged into their predecessor once they sampled the same atlas
    uint32_t atlasEvicted = 0; // pictures taken back out of an atlas (sampled in a way that cannot be redirected)
    uint32_t atlasSorted = 0;  // runs of consecutive atlas passes put into page order
    // What a back end with one render target per framebuffer object does with the frame:
    uint32_t efbScenes = 0;    // stretches of drawing / clearing in the EFB between uses of other targets
    uint32_t offScenes = 0;    // uses of other targets: offscreen passes, copy quads, fills
    uint32_t shadersVs = 0, shadersFs = 0; // generated this frame
    uint32_t vtxBytes = 0, idxBytes = 0, uniBytes = 0;
    uint32_t softVerts = 0;    // vertices transformed on the CPU
    uint32_t dlHit = 0, dlBuild = 0, dlRehash = 0, dlBypass = 0; // display-list cache (vgx_dlcache.cpp)
    uint32_t staticDraws = 0, staticVerts = 0; // draws from static buffers
    uint32_t bufUploads = 0, bufUploadBytes = 0;
    uint64_t hash = 0;         // MSC_VGX_HASH=1: hash of the frame's command buffer(s)
};
Frame& Cur();
void TexBound(uint32_t texId, uint32_t bytes); // distinct textures referenced by this frame's draws
void ProgramUsed(uint16_t vsId, uint16_t fsId);
void TexLive(int32_t deltaCount, int64_t deltaBytes); // textures alive in the cache
}

} // namespace vgx
