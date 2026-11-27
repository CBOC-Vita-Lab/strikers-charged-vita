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
    bool texCopyClearOnly = false;   // nxr4b (MSC_GLOW_L0OFF=2): the copy being queued is marked clear-only by the game
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
    // Hash of the registers the shader keys are built from (KeyHashOf in vgx_state.cpp), kept up to date by the register writers
    // (MSC_VGX_RSCACHE, default on; not maintained when it is 0).
    uint64_t keyHash = 0;
    // MSC_VGX_MTXSPLIT (fe26): which matrix rows the writers changed since the last resolution (the position rows, the post-transform rows, the normal rows), so that BuildMtx
    // rebuilds only the blocks those rows can reach. mtxFull: something changed that the masks do not say (a reset, a post-transform index): every block is rebuilt.
    uint64_t mtxPos = ~0ull, mtxPost = ~0ull;
    uint32_t mtxNrm = ~0u;
    bool mtxFull = true;
};
extern State g;
// Rows [word0 / unit, (word0 + words - 1) / unit] of a matrix memory as a mask (at most 64 rows).
inline uint64_t MtxRows(uint32_t word0, uint32_t words, uint32_t unit) {
    const uint32_t r0 = word0 / unit, r1 = (word0 + words - 1) / unit;
    const uint32_t n = r1 - r0 + 1;
    return (n >= 64 ? ~0ull : ((1ull << n) - 1)) << r0;
}
inline void MtxMarkPos(uint32_t word0, uint32_t words) { g.mtxPos |= MtxRows(word0, words, 4); }
inline void MtxMarkPost(uint32_t word0, uint32_t words) { g.mtxPost |= MtxRows(word0, words, 4); }
inline void MtxMarkNrm(uint32_t word0, uint32_t words) { g.mtxNrm |= static_cast<uint32_t>(MtxRows(word0, words, 3)); }
inline void MtxClear() { g.mtxPos = g.mtxPost = 0; g.mtxNrm = 0; g.mtxFull = false; }
// MSC_VGX_EXTMEMO: moves whenever a write changes what LightFactId() / TevRegsWide() read (the light memory, XF registers 9 and 0x0E..0x11, the TEV colour registers, a reset).
extern uint32_t g_extGen;

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
// MSC_VGX_RSCACHE (vgx_state.cpp, vgx_draw.cpp): 0 off, 1 on, 2 verify (every resolution is also made the old way and compared). Read once, again after GxdInit().
extern int g_rsCacheMode;
int RsCacheModeSlow();
inline int RsCacheMode() { return __builtin_expect(g_rsCacheMode >= 0, 1) ? g_rsCacheMode : RsCacheModeSlow(); }
uint64_t KeyHashOf(const State& s); // State::keyHash worked out from scratch
void ResetState();

// ---- geometry (vgx_state.cpp)
void ProcessStream(const uint8_t* data, uint32_t nbytes);
void CallList(const uint8_t* list, uint32_t nbytes); // a display list in guest RAM: through the geometry cache (vgx_dlcache.cpp)
void DlCacheReport(FILE* f);
void FrontEndReport(FILE* f); // vgx_draw.cpp: state resolution counters
void DrawStream(uint8_t prim, int fmt, const uint8_t* data, uint32_t count, uint32_t availBytes);
// Byte ranges of the vertex arrays (attributes POS..TEX7) a decode read.
struct ArraySpans {
    uint32_t mask = 0; // bit per attribute number
    uint32_t lo[12] = {}, hi[12] = {};
    bool incomplete = false; // 1.2: some index addressed an element outside its array (the decode used zeros for it; the spans do not cover it)
};
const Plan& CurrentPlan(int fmt); // the player's plan for a vertex format under the current descriptor
// outStride: the vertices are written at that stride instead of the plan's own (MSC_VGX_SKINDIRECT: the stride of the skinned layout, whose offsets are the plan's own).
// onlyAttrs (MSC_VGX_DLREFRESH, vgx_dlcache.cpp): when not 0, a bit per attribute number: only those attributes are decoded again into vertices that a full decode
// of the same plan, stream and stride filled before (no defaults are written, nothing else of a vertex is touched, the matrix-row maximum is left alone).
void DecodeVertices(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, ArraySpans* spans, uint32_t outStride = 0, uint32_t onlyAttrs = 0);
uint32_t StreamVertexSize(int fmt);
// ---- MSC_VGX_FEPAR (vgx_fepar.inc, included by vgx_draw.cpp): vertex decode of plain stream draws on a worker thread
// DecodeVerticesSnap: DecodeVertices over a copy of the array bindings (`arrs`, the 26 entries of State::arrays), for the worker: no census, no spans, no global written.
void DecodeVerticesSnap(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, const ArrayRef* arrs, uint32_t* mxOut = nullptr);
uint32_t StreamMatrixMax(const Plan& p, const uint8_t* src, uint32_t count); // (MSC_VGX_FEPAR_MTX) the matrix-index maximum the decode would report, without decoding
bool PlanHasMatrixIndex(const Plan& p);
bool FeparMtxOn();
uint32_t FeparStagedMtxMax(); // the maximum FeparStage scanned for the draw it staged
bool DecodeDeferrable(const Plan& p); // the worker may decode this plan's vertices (no matrix-index attribute, no diagnostic mode that shares scratch state)
// DrawStream: the draw's decode is left to EmitDraw (true), which runs it inline when it must or hands it to the worker. False: nothing staged, decode now.
bool FeparStage(const Plan& p, const uint8_t* src, uint32_t count);
void FeparChunkJump(); // the player hands back a record chunk: the jobs that read the one before it are finished
void FeparNoteInline(const Plan& p, uint32_t count, bool skin); // (census) a stream draw the player decodes itself, by reason
void FeparNoteArrayCopy(); // an array copied into a record chunk (a job of the next chunk may read it)
void FeparJoin();     // every job handed to the worker is finished (a counter read when none is out)
int FeparMode();      // MSC_VGX_FEPAR: 0 off, 1 on, 2 verify
void FeparReport(FILE* f);
void FeparPicture(uint64_t frame); // once per picture, after the join: the periodic counter line
void SetFeparThreadInit(void (*fn)()); // the host's placement of the worker thread (core, priority), run on that thread when it starts
void SetFeparRepin(void (*fn)());        // (core3x15) run on the worker, between two jobs, when its placement on the system core was retired (it must go back to the plan's core)
void SetFeparReplace(bool (*fn)());      // (core60, MSC_VGX_FEPAR_KEEP bit 4) run on the worker to put it back on the system core (true: placed); the host checks the core is open
int FeparKeepBits();                     // (core60) MSC_VGX_FEPAR_KEEP, 0 off
void FeparPlacedOnCore3(bool on);        // (core3x15) called by the host on the worker thread when it put it on the system core (the worker then parks at once)
// W5 SKIN (vgx_ext.h): can draws of this plan carry the skin state (their position is indexed), and the in-place widening of decoded vertices to the
// skinned layout (p.layoutMask | VL_SKIN; the buffer must hold count vertices of the wider layout). Uses g_skin.
bool SkinPosIndexed(const Plan& p);
void DecodeCensusFrame(uint64_t frame); // MSC_VGX_DECODECENSUS
void SkinExpand(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* v);
// MSC_VGX_SKINDIRECT: the vertices were decoded at the skinned layout's stride already (DecodeVertices outStride): only the bone rows and weights are written.
bool SkinDirect();
void SkinFill(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* v);
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
// MSC_VGX_CULL: the bounding box of the next EmitStaticDraw's vertices as stored (min xyz, max xyz; the positions in the layout's first 12 bytes), or null
// when the display-list cache has none (the draw is skinned, has non-finite positions, ...). Set by the cache around the call.
extern const float* g_staticSegBounds;
// 1.3 MSC_VGX_PALMERGE (vgx_draw.cpp "palette runs"): set by the cache around EmitStaticDraw: the segment's kept decoded vertices (null: not kept) and
// the serial of its buffer id; StaticSegVerts finds them again while the buffer is the same; PalMergeForget drops the combined buffers made from a
// buffer that is being released; AllocStaticBufferIdFresh gives an id that was not freed in this frame.
bool PalCompactOn();          // MSC_VGX_PALCOMPACT
long PalMergeMode();          // in effect: 0 off, 1 on, 2 auto (armed)
bool SkinMergeOn();           // MSC_VGX_SKINMERGE: runs of GPU-skinned static draws (the lists keep their decoded vertices too)
uint64_t SkinMergeCapBytes(); // MSC_VGX_SKINMERGE_MB: kept vertices of skinned lists
uint64_t PalMergeCapBytes(); // MSC_VGX_PALMERGE_MB: kept vertices (combined buffers: half of it)
extern const uint8_t* g_staticSegVerts;
extern uint32_t g_staticSegPalMask; // MSC_VGX_PALCOMPACT: the matrices (row / 3, bit each) the segment's vertices name, 0: not known
extern uint32_t g_staticSegSerial;
const uint8_t* StaticSegVerts(uint32_t bufId, uint32_t serial, uint32_t byteOff, uint32_t bytes);
void PalMergeForget(uint32_t bufId);
uint32_t AllocStaticBufferIdFresh();
uint64_t PalKeptBytes();
// 1.3 MSC_VGX_SBATCH (vgx_draw.cpp "static batches"): the cache keeps the decoded vertices of a rigid list promoted while a background class is drawn
// (SBatchKeepWanted, up to SBatchKeepCapBytes) and finds them with StaticSegVerts / g_staticSegVerts; SBatchForget drops the batches made from a
// buffer that is being released.
bool SBatchKeepWanted();
uint64_t SBatchKeepCapBytes();
uint64_t SBatchKeptBytes();
void SBatchForget(uint32_t bufId);
void SBatchTrimKept(uint64_t idle);
void SBatchNoted(uint32_t bufId);
uint8_t SBatchWhyNotKept(uint32_t bufId);
void SBatchWant(uint32_t bufId);
uint64_t SBatchRekept();
bool FillBoxesWanted(); // MSC_VGX_SHQUAD != 0
bool CullBoundsWanted(); // MSC_VGX_CULL != 0: the cache records the boxes
uint32_t LastDecodedPosMtxMax(); // of the last DecodeVertices call
// Number of indices a GX primitive of `count` vertices turns into; writes them (relative to `base`) when out != null.
uint32_t PrimIndices(uint8_t gxPrim, uint32_t count, uint32_t base, uint16_t* out);
uint32_t SoftXformLimit(); // primitives up to this many vertices may be transformed on the CPU (and merged)
bool LastDrawNeedsArena(); // the draw EmitDraw resolved last was transformed on the CPU, or its vertices feed the Z-freeze reference (MSC_VGX_PROMOSMALL)
// MSC_VGX_PROMOSOLO (vgx_dlcache.cpp): what became of the last EmitDraw, and whether it was transformed on the CPU with nothing else asking for the arena.
enum EmitOutcome : uint8_t { kEmitDropped, kEmitNew, kEmitJoined, kEmitBatch };
EmitOutcome LastEmitOutcome();
bool LastDrawSoftOnly();
void DlNoteRecord(bool joined); // (EmitDraw) a draw record was made, or a draw joined the record before it
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
bool DlKeepActive();          // 1.3 MSC_DL_KEEP is on
uint64_t DlKeepSaved();       // lists found again that the 600-frame rule would have freed
// 1.3 (wave 8) per-match switch line: what the front end's switches have done so far (cumulative, never reset by a diagnostic)
void GetDrawSwitchCounts(DrawSwitchCounts& out);
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
void TexTouch(const uint32_t* w, uint32_t n);
void FirstUseNote(const char* text);   // 1.3 first-use manifest: a name for what was just made or uploaded (vgx_texcache.cpp)
uint64_t TexUploadSerial();   // 1.3 MSC_TEX_PRETOUCH: EXT_TOUCH_TEX (vgx_texcache.cpp)
// Shader warm-up list (vgx_draw.cpp): every shader key and program pair that is ever needed is
// appended to this file, and the next run generates and links all of them in its first frame
// instead of in the middle of play. Call before the first frame (or MSC_VGX_SHADERLIST=<path>).
void SetShaderListFile(const char* path);
const char* ShaderListPath();            // the warm-up list's path as set (env or SetShaderListFile), "" if none (w9 cuts)
void PassCheckReport(FILE* f);           // result of MSC_VGX_PASSCHECK=1, for the statistics report
void TexCacheLost(uint32_t texId);       // 1.2 (MSC_VGX_UPLOAD_RETRY): the back end had no memory for this texture: its cache entry is dropped after a while, so the next use uploads it again
void TexCacheEndFrame();                 // frees least recently used textures when over the budget
void SetTextureBudget(uint64_t bytes);   // RGBA8 bytes of guest textures to keep; 0 = unlimited (also MSC_VGX_TEXBUDGET=<MiB>)
uint32_t AcquireCopyTexture(const void* dest, uint16_t w, uint16_t h, uint32_t fmt);
uint32_t AllocTextureId(); // a back-end texture id that belongs to no guest texture (copy atlases)
void DestroyCopyTexture(const void* dest);
void InvalidateAllTextures();
// MSC_VGX_TEXSAME=0|1|2 (fe26; code default 0, Vita plan 1): a texture bind of the object the slot holds, with no guest write notification since the slot's last bind, leaves the
// bindings clean (vgx_play.cpp R_TEX), so ResolveState's early exit is taken for the draw. =2: the bindings are dirtied as before and each resolution is compared with the
// slot's binding from before ("texsame: MISMATCH"). The counters feed the front-end report.
int TexSameMode();
uint64_t TexSameGen(const TexObj& o); // the write generation of the object's data range (all mip levels), ~0 when unknown or a colour-indexed format
uint64_t TexSameStamp();             // the copy-set and cache-erase generations a binding is valid under
bool TexSameOk(); // the inputs of a resolution that a write notification does not cover are absent (no HD replacement hook, no texture verifier, guest RAM tracked)
struct TexSameCounters { uint64_t skipped = 0, dirtied = 0, checked = 0, bad = 0; uint32_t verifySlots = 0, pending = 0, bindDirt = 0; };   // (mode 2: slots to compare; slots with a bind of another object not resolved yet; slots dirtied by a bind record)
extern TexSameCounters g_texSame;
void TexCacheReport(FILE* f);
void CopyCheckTexReport(); // MSC_VGX_COPYCHECK=1
void SetGuestWriteGenerationHook(uint64_t (*fn)(const void*, size_t));
// Optional: called on the player's thread for every texture upload (texture, GX format, frame).
void SetTextureDumpHook(void (*fn)(const TexUpload& up, uint32_t gxFormat, uint64_t frame));
// 1.1: optional replacement of a guest texture when it is uploaded (vgx_hdtex.cpp installs one, from the Galaxy port): `fn` may
// fill `up` (width, height, format, levels, authored) with another image of the texture and return true. Asked only on upload,
// never for EFB copies. The guest's dimensions stay what the draw state and the TEV see; the replacement carries its own mip
// chain, and its binds get the mip filter `mipFilter` (FILT_LIN_MIP_*).
struct TexReplaceQuery {
    const uint8_t* data;  // level 0 in guest memory
    uint32_t fmt;         // GX texture format
    uint16_t width, height;
    const uint8_t* tlut;  // palette of a CI texture (null otherwise)
    uint32_t tlutBytes;
    bool mipClamp;        // backend::Caps::mipClamp: no compressed level below one 4x4 block
    uint64_t liveBytes;   // bytes of replacement textures the back end holds now
};
// `fn` returns 0 (not replaced), 1 (replaced: `up` filled) or 2 (the replacement is being read: *wait names it, and the cache
// uploads the texture again once `ready(*wait)` says it is in).
void SetTextureReplaceHook(int (*fn)(const TexReplaceQuery& q, TexUpload& up, uint32_t* wait), uint8_t mipFilter, bool (*ready)(uint32_t wait));
uint64_t ReplacedTextureBytes();                 // bytes of replacement textures alive in the back end
void SetReplacedTextureBudget(uint64_t bytes);   // past half of it, replacements unused for 600 frames are freed

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
    uint32_t passesDown = 0;   // of the passes: 2:1 filter steps drawn at half scale straight into the copy's destination (MSC_VGX_PASSDOWN)
    uint32_t fills = 0;        // of the copies: source rectangle known to be a constant colour
    uint32_t fillsRedundant = 0; // of the fills: the texture already holds exactly that (nothing is executed but the clear)
    uint32_t efbReads = 0;     // of the copies: the remaining ones, which really read the EFB
    uint32_t clearsRedundant = 0; // clears (of copies) not executed: the rectangle already holds exactly those values
    uint32_t atlasPasses = 0;  // of the direct passes: drawn into a cell of a copy atlas
    uint32_t atlasRemaps = 0;  // draws whose texture coordinates were rewritten to sample an atlas cell
    uint32_t atlasMerged = 0;  // draw records merged into their predecessor once they sampled the same atlas
    uint32_t atlasEvicted = 0; // pictures taken back out of an atlas (sampled in a way that cannot be redirected)
    uint32_t atlasSorted = 0;  // runs of consecutive atlas passes put into page order
    uint32_t shadowsDown = 0;  // MSC_VGX_SHADOWDOWN: shadow pictures drawn at half size straight into their cells
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
