#include <revolution/types.h>

#include "NL/gl/glRenderList.h"

#include "NL/gl/glMatrix.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/platvmath.h"
#include "Game/MathHelpers.h"

#ifdef NP_NATIVE
#include "np/framesend.h"
#include "np/overlap.h"
#include "NL/glx/glxMemory.h"
#include "NL/glx/glxSwap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" long VgxAbOverride(const char* key, long dflt) __attribute__((weak));   // (renderer/src/vgx_state.cpp; weak: a build without the renderer links none)

// NP (two-stage pipeline): the packet the send job reads is a COPY in the frame arena (packet, stream descriptors, parameter block, skin matrices),
// made when the packet is attached (the sorters hold the copy) and made again from the original at the fork (GLRefreshPacketSnapshots): the serial
// program sends at the end of the frame and sees what the build left in the persistent packet (a parameter set after the attach, the matrix handle
// glModelSetMatrix put there last), the next frame's build rewrites all of it while the job still reads. The records are a list in the arena.
namespace
{
const unsigned long kSkinMatricesHash = 0xFB3B01EC;
struct SnapRec
{
    SnapRec* next;
    const glModelPacket* src;
    glModelPacket* dst;
    glModelStream* dstStreams;
    unsigned char* dstParams;
    unsigned char* dstSkin;
    unsigned long skinCap;
    unsigned short streamCap, paramCap, skinOffset, hasSkin;
};
SnapRec* sSnapHead;
SnapRec* sSnapTail;
// the records of the frame the job sends (set at the fork), and what the job's Draw calls wrote into the copies (applied after the join)
SnapRec* sJobHead;
struct WriteBack
{
    glTextureBinding* orig;
    unsigned char wrapS, wrapT;
};
const unsigned kMaxWriteBacks = 1024;
WriteBack sWriteBacks[kMaxWriteBacks];
volatile unsigned sNumWriteBacks;
unsigned long sWriteBacksLost, sWriteBacksMade;
const unsigned kLookupSize = 8192;   // open addressing, built by the job thread at the first lookup of a job
SnapRec* sLookup[kLookupSize];
bool sLookupBuilt;
bool sLookupFull;
unsigned long sSnapGeneration;   // the arena generation the list belongs to
unsigned long sSnapCount, sSnapFail, sSnapGrown;
int sSnapEnabled = -1;

inline unsigned long SkinOffsetOf(GLMaterialProgram* program, unsigned long paramSize)
{
    const GXMaterialParameter* info = program->GetParameters();
    for (unsigned long i = 0; i < program->parameterCount; ++i, ++info)
        if (info->hash == kSkinMatricesHash && info->offset + 8 <= paramSize)
            return info->offset;
    return 0xFFFF;
}

// gt27 (MSC_FS_SKINDEDUP=1, default 0): several packets of one skin mesh point at the same skin matrix buffer; the refresh at the fork copied it once per packet.
// With the switch the first copy of a refresh is kept in a small table (source pointer and size) and the later packets point at it: the same bytes (nothing writes the
// source between the copies: the refresh runs on the game thread with no job running), one memcpy. Counters in the [fs] skin-dedup line.
int SkinDedupOn()
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("MSC_FS_SKINDEDUP");
        on = (e != 0 && *e == '1') ? 1 : 0;
    }
    return on;
}
const unsigned kDedupSlots = 256;   // a power of two
struct DedupSlot
{
    unsigned long src, size, gen;
    void* dst;
};
DedupSlot sDedup[kDedupSlots];
bool sInRefresh = false;
unsigned long sDedupGen = 1, sSkinCopies, sSkinBytes, sSkinShared, sSkinSharedBytes, sDedupRefreshes;
int sSkinDedupBreak = -1;   // MSC_FS_SKINDEDUP_BREAK=1 (proof only): a shared copy gets one wrong byte, the frame hashes must change
inline void* DedupFind(unsigned long src, unsigned long size)
{
    DedupSlot& d = sDedup[((src >> 4) * 2654435761u) & (kDedupSlots - 1)];
    return (d.gen == sDedupGen && d.src == src && d.size == size) ? d.dst : 0;
}
inline void DedupPut(unsigned long src, unsigned long size, void* dst)
{
    DedupSlot& d = sDedup[((src >> 4) * 2654435761u) & (kDedupSlots - 1)];
    d.src = src;
    d.size = size;
    d.gen = sDedupGen;
    d.dst = dst;
}

// copy the skin matrix buffer the parameter block points at (the buffer is rewritten in place every frame)
inline void CopySkin(SnapRec* r)
{
    unsigned long* buffer = (unsigned long*)(r->dstParams + r->skinOffset);
    if (buffer[0] == 0 || buffer[1] == 0)
        return;
    const bool dedup = sInRefresh && SkinDedupOn() != 0;   // (only in the refresh at the fork: at the attach a later packet may see a buffer rewritten since)
    if (dedup)
    {
        if (void* shared = DedupFind(buffer[0], buffer[1]))
        {
            if (sSkinDedupBreak < 0) { const char* e = getenv("MSC_FS_SKINDEDUP_BREAK"); sSkinDedupBreak = (e != 0 && *e == '1') ? 1 : 0; }
            if (sSkinDedupBreak == 1) ((unsigned char*)shared)[0] ^= 0x01;
            buffer[0] = (unsigned long)shared;
            ++sSkinShared;
            sSkinSharedBytes += buffer[1];
            return;
        }
        ++sSkinCopies;
        sSkinBytes += buffer[1];
    }
    const unsigned long srcPtr = buffer[0];
    if (buffer[1] <= r->skinCap && r->dstSkin != 0)
    {
        memcpy(r->dstSkin, (const void*)buffer[0], buffer[1]);
        buffer[0] = (unsigned long)r->dstSkin;
        if (dedup) DedupPut(srcPtr, buffer[1], r->dstSkin);
        return;
    }
    void* grown = glplatFrameAllocTry(buffer[1], GLM_Header);
    if (grown != 0)
    {
        memcpy(grown, (const void*)buffer[0], buffer[1]);
        buffer[0] = (unsigned long)grown;
        ++sSnapGrown;
        if (dedup) DedupPut(srcPtr, buffer[1], grown);
    }
    else
        ++sSnapFail;
}

inline void Fill(SnapRec* r)
{
    const glModelPacket* src = r->src;
    glModelPacket* dst = r->dst;
    *dst = *src;
    if (r->dstStreams != 0 && src->numStreams <= r->streamCap)
    {
        memcpy(r->dstStreams, src->streams, src->numStreams * sizeof(glModelStream));
        dst->streams = r->dstStreams;
    }
    GLMaterialProgram* program = (GLMaterialProgram*)src->materialProgram;
    if (r->dstParams != 0 && program != 0 && src->materialParameters != 0 && program->parameterDataSize <= r->paramCap)
    {
        memcpy(r->dstParams, src->materialParameters, program->parameterDataSize);
        dst->materialParameters = r->dstParams;
        if (r->hasSkin)
            CopySkin(r);
    }
}

// MSC_FS_SNAPREFRESH=0: the copies stay as the attach made them (a switch for the equality bisection)
inline bool SnapRefresh()
{
    static const bool refresh = [] { const char* e = getenv("MSC_FS_SNAPREFRESH"); return !(e != 0 && e[0] == '0'); }();
    return refresh;
}
} // namespace

unsigned long GLSnapshotFailures() { return sSnapFail; }

const glModelPacket* GLSnapshotPacket(const glModelPacket* src)
{
    if (!NpFsPipe())
        return src;
    if (sSnapEnabled < 0)
    {
        const char* e = getenv("MSC_FS_SNAP");
        sSnapEnabled = (e != 0 && e[0] == '0') ? 0 : 1;
    }
    if (!sSnapEnabled)
        return src;
    if (sSnapGeneration != glplatFrameGeneration())   // the arena was reset since: the records are gone
    {
        sSnapHead = sSnapTail = 0;
        sSnapGeneration = glplatFrameGeneration();
    }
    GLMaterialProgram* program = (GLMaterialProgram*)src->materialProgram;
    const unsigned long paramSize = (program != 0 && src->materialParameters != 0) ? program->parameterDataSize : 0;
    const unsigned long nStreams = src->numStreams;
    const unsigned long streamBytes = nStreams * sizeof(glModelStream);
    const unsigned long total = sizeof(SnapRec) + sizeof(glModelPacket) + ((streamBytes + 7) & ~7ul) + ((paramSize + 7) & ~7ul);
    // test switch: MSC_FS_SNAPFAIL=n makes every n-th snapshot "not fit" (the fallback: the original is attached, the frame is joined at the fork)
    static long failEvery = -1;
    static unsigned long tries = 0;
    if (failEvery < 0)
    {
        const char* e = getenv("MSC_FS_SNAPFAIL");
        failEvery = (e != 0 && e[0] != 0) ? atol(e) : 0;
    }
    unsigned char* mem = (failEvery > 0 && (++tries % (unsigned long)failEvery) == 0) ? 0 : (unsigned char*)glplatFrameAllocTry(total, GLM_Header);
    if (mem == 0)
    {
        ++sSnapFail;
        return src;
    }
    SnapRec* r = (SnapRec*)mem;
    mem += sizeof(SnapRec);
    r->src = src;
    r->dst = (glModelPacket*)mem;
    mem += sizeof(glModelPacket);
    r->dstStreams = 0;
    if (streamBytes != 0)
    {
        r->dstStreams = (glModelStream*)mem;
        mem += (streamBytes + 7) & ~7ul;
    }
    r->streamCap = (unsigned short)nStreams;
    r->dstParams = 0;
    r->paramCap = 0;
    r->hasSkin = 0;
    r->skinOffset = 0;
    r->dstSkin = 0;
    r->skinCap = 0;
    if (paramSize != 0)
    {
        r->dstParams = mem;
        r->paramCap = (unsigned short)paramSize;
        const unsigned long off = SkinOffsetOf(program, paramSize);
        if (off != 0xFFFF)
        {
            r->hasSkin = 1;
            r->skinOffset = (unsigned short)off;
            const unsigned long* b = (const unsigned long*)((const unsigned char*)src->materialParameters + off);
            if (b[0] != 0 && b[1] != 0)
            {
                r->dstSkin = (unsigned char*)glplatFrameAllocTry(b[1], GLM_Header);
                r->skinCap = r->dstSkin != 0 ? b[1] : 0;
            }
        }
    }
    // MSC_FS_SNAPONCE (np/overlap.h): the refresh at the fork writes everything Fill writes here, from the same source, so only the packet itself is taken now, with
    // its pointers to the persistent streams and parameters. Nothing reads a copy before that refresh: the sort keys come from `src`, the sorters only keep the pointer, and both
    // ways of sending a frame refresh first (glplatSendFrameForked, glx_SendViews).
    if (NpFsSnapOnce() && SnapRefresh())
    {
        *r->dst = *src;
        NpOverlapSnapSkipped(streamBytes + paramSize + r->skinCap);
    }
    else
    {
        Fill(r);
    }
    r->next = 0;
    if (sSnapTail != 0)
        sSnapTail->next = r;
    else
        sSnapHead = r;
    sSnapTail = r;
    ++sSnapCount;
    return r->dst;
}

// at the end of the build (the fork, or the plain send): bring every copy up to what the persistent packets hold now, and start a new list
void GLRefreshPacketSnapshots()
{
    if (sSnapGeneration != glplatFrameGeneration())
    {
        sSnapHead = sSnapTail = 0;
        sSnapGeneration = glplatFrameGeneration();
    }
    const bool refresh = SnapRefresh();
    GLApplyDrawWriteBacks();   // (no job runs here: the join of the last one is done)
    sJobHead = sSnapHead;      // the job of this frame finds the copies it draws (GLDrawSetWrap)
    sLookupBuilt = false;
    static const bool dbg = getenv("MSC_FS_SNAPDBG") != 0;
    if (dbg && !NpFsSnapOnce())   // the equality bisection: what changed in a persistent packet between the attach and the fork
    {
        static int shown = 0;
        unsigned long n = 0, nd = 0;
        for (SnapRec* r = sSnapHead; r != 0; r = r->next, ++n)
        {
            GLMaterialProgram* program = (GLMaterialProgram*)r->src->materialProgram;
            if (program == 0 || r->dstParams == 0 || r->src->materialParameters == 0)
                continue;
            const unsigned long sz = program->parameterDataSize <= r->paramCap ? program->parameterDataSize : r->paramCap;
            if (memcmp(r->dstParams, r->src->materialParameters, sz) != 0)
            {
                ++nd;
                if (shown < 60)
                {
                    ++shown;
                    const unsigned char* a = r->dstParams;
                    const unsigned char* b = (const unsigned char*)r->src->materialParameters;
                    unsigned long k = 0;
                    while (k < sz && a[k] == b[k])
                        ++k;
                    fprintf(stderr, "[snapdbg] frame %ld gen %lu rec %lu prog %08lx size %lu first diff at %lu: attach %02x%02x%02x%02x%02x%02x%02x%02x fork %02x%02x%02x%02x%02x%02x%02x%02x\n", (long)glxGetFrameCount(), glplatFrameGeneration(), n, (unsigned long)program->programHash, sz, k,
                        a[k & ~7ul], a[(k & ~7ul) + 1], a[(k & ~7ul) + 2], a[(k & ~7ul) + 3], a[(k & ~7ul) + 4], a[(k & ~7ul) + 5], a[(k & ~7ul) + 6], a[(k & ~7ul) + 7],
                        b[k & ~7ul], b[(k & ~7ul) + 1], b[(k & ~7ul) + 2], b[(k & ~7ul) + 3], b[(k & ~7ul) + 4], b[(k & ~7ul) + 5], b[(k & ~7ul) + 6], b[(k & ~7ul) + 7]);
                }
            }
        }
        if (nd != 0)
            fprintf(stderr, "[snapdbg] frame %ld gen %lu: %lu of %lu parameter blocks changed between the attach and the fork\n", (long)glxGetFrameCount(), glplatFrameGeneration(), nd, n);
    }
    ++sDedupGen;   // (gt27: a new table for this refresh; copies of an earlier frame are never shared)
    sInRefresh = true;
    static const int sPrefetchEnv = [] { const char* e = getenv("MSC_FS_REFRESH_PREFETCH"); return (e != 0 && *e == '1') ? 1 : 0; }();
    // wave 15 (MSC_AB=MSC_FS_REFRESH_PREFETCH:0,1, vgx_ab.h): the in-run A/B harness answers the value while it flips this switch; unset: the environment's, as before.
    const int sPrefetch = VgxAbOverride ? static_cast<int>(VgxAbOverride("MSC_FS_REFRESH_PREFETCH", sPrefetchEnv)) != 0 : sPrefetchEnv;
    if (refresh && sPrefetch)
    {
        // gt27 (MSC_FS_REFRESH_PREFETCH=1, default 0): the refresh walks ~380 records a picture whose sources are cold (the packet, its stream list, its parameter block, the program):
        // the record two ahead and the next record's packet, streams, parameters and program are prefetched while this one is copied. Hints only: the bytes copied are the same.
        if (sSnapHead != 0)
        {
            __builtin_prefetch(sSnapHead->src);
            if (sSnapHead->next != 0) __builtin_prefetch(sSnapHead->next);
        }
        for (SnapRec* r = sSnapHead; r != 0; r = r->next)
        {
            SnapRec* n1 = r->next;
            if (n1 != 0)
            {
                SnapRec* n2 = n1->next;
                if (n2 != 0)
                {
                    __builtin_prefetch(n2);
                    __builtin_prefetch(n2->src);
                }
                const glModelPacket* ns = n1->src;
                __builtin_prefetch(ns->materialParameters);
                __builtin_prefetch(ns->streams);
                __builtin_prefetch(ns->materialProgram);
                __builtin_prefetch(n1->dst, 1);
            }
            Fill(r);
        }
    }
    else if (refresh)
        for (SnapRec* r = sSnapHead; r != 0; r = r->next)
            Fill(r);
    sInRefresh = false;
    if (SkinDedupOn() && ++sDedupRefreshes % 1800 == 0)
        fprintf(stderr, "[fs] skin-dedup (MSC_FS_SKINDEDUP=1): %lu refreshes, skin buffers copied %lu (%lu KiB), shared with an earlier packet's copy %lu (%lu KiB not copied)\n",
            sDedupRefreshes, sSkinCopies, sSkinBytes / 1024, sSkinShared, sSkinSharedBytes / 1024);
    sSnapHead = sSnapTail = 0;
}

namespace
{
inline unsigned LookupSlot(const glModelPacket* p)
{
    return (unsigned)(((unsigned long)p >> 4) * 2654435761u) & (kLookupSize - 1);
}
SnapRec* FindCopy(const glModelPacket* packet)
{
    if (!sLookupBuilt)   // (the job thread, the first lookup of this job)
    {
        memset(sLookup, 0, sizeof(sLookup));
        sLookupFull = false;
        unsigned n = 0;
        for (SnapRec* r = sJobHead; r != 0; r = r->next)
        {
            if (++n > kLookupSize / 2)
            {
                sLookupFull = true;
                break;
            }
            unsigned i = LookupSlot(r->dst);
            while (sLookup[i] != 0)
                i = (i + 1) & (kLookupSize - 1);
            sLookup[i] = r;
        }
        sLookupBuilt = true;
    }
    if (!sLookupFull)
    {
        for (unsigned i = LookupSlot(packet); sLookup[i] != 0; i = (i + 1) & (kLookupSize - 1))
            if (sLookup[i]->dst == packet)
                return sLookup[i];
        return 0;
    }
    for (SnapRec* r = sJobHead; r != 0; r = r->next)
        if (r->dst == packet)
            return r;
    return 0;
}
} // namespace

void GLDrawSetWrap(const glModelPacket* packet, glTextureBinding* binding, bool wrapS, bool wrapT)
{
    if (wrapS)
        binding->SetWrapS(true);
    if (wrapT)
        binding->SetWrapT(true);
    if (sJobHead == 0 || !NpFsPipe())
        return;
    SnapRec* r = FindCopy(packet);
    if (r == 0 || r->dstParams == 0 || r->src->materialParameters == 0)
        return;   // (the packet is the persistent one itself: the store went there)
    const unsigned char* b = (const unsigned char*)binding;
    if (b < r->dstParams || b + sizeof(glTextureBinding) > r->dstParams + r->paramCap)
        return;
    glTextureBinding* orig = (glTextureBinding*)((unsigned char*)r->src->materialParameters + (b - r->dstParams));
    const unsigned n = sNumWriteBacks;
    for (unsigned i = n > 8 ? n - 8 : 0; i < n; ++i)   // the same store again (the same packet drawn every frame)
        if (sWriteBacks[i].orig == orig && sWriteBacks[i].wrapS == (unsigned char)wrapS && sWriteBacks[i].wrapT == (unsigned char)wrapT)
            return;
    if (n >= kMaxWriteBacks)
    {
        ++sWriteBacksLost;
        return;
    }
    sWriteBacks[n].orig = orig;
    sWriteBacks[n].wrapS = wrapS;
    sWriteBacks[n].wrapT = wrapT;
    sNumWriteBacks = n + 1;
}

void GLApplyDrawWriteBacks()
{
    const unsigned n = sNumWriteBacks;
    for (unsigned i = 0; i < n; ++i)
    {
        if (sWriteBacks[i].wrapS)
            sWriteBacks[i].orig->SetWrapS(true);
        if (sWriteBacks[i].wrapT)
            sWriteBacks[i].orig->SetWrapT(true);
    }
    sWriteBacksMade += n;
    sNumWriteBacks = 0;
}

extern "C" void NpGlSnapStats(void)
{
    if (sSnapCount != 0 || sSnapFail != 0)
        fprintf(stderr, "[fs]   packet snapshots: %lu made, %lu skin buffers grown at the fork, %lu not made (frame arena full); draw write-backs (wrap flags set by Draw): %lu made, %lu lost\n", sSnapCount, sSnapGrown, sSnapFail, sWriteBacksMade, sWriteBacksLost);
}
#endif

unsigned long GLTexturePacketSorter::GetSortKey(
    GLView*, const glModelPacket* pPacket)
{
    return ((GLMaterialProgram*)pPacket->materialProgram)->programHash;
}

unsigned long GLTransformedDepthPacketSorter::GetSortKey(
    GLView* view, const glModelPacket* pPacket)
{
    nlMatrix4 packetMatrix;
    glGetMatrix(pPacket->matrix, packetMatrix);

    nlVector3 pos = packetMatrix.GetTranslation();
    nlMultPosVectorMatrix(pos, *view->m_Interface->GetViewMatrix());
    pos.z = nlMinEquals(0.0f, pos.z);

    return (unsigned long)(int)(-pos.z * 2147483648.0f);
}

unsigned long GLTransformedMatrixDepthPacketSorter::GetSortKey(
    GLView* view, const glModelPacket* pPacket)
{
    nlMatrix4 packetMatrix;
    nlVector3 out;
    glGetMatrix(pPacket->matrix, packetMatrix);
    nlMultPosVectorMatrix(out, *(nlVector3*)&packetMatrix.e2[3][0],
        *view->m_Interface->GetViewMatrix());
    return ((unsigned long)(int)(-out.z * 100.0f) << 12)
        | (m_Sequence++ & 0xFFF);
}

#ifdef NP_NATIVE
// MSC_PKT_SORT=0|1 (default 1). Every packet the game attaches to a view went into a balanced tree keyed by (sort key << 32 | packet address): an
// insert per packet on the game thread (a descent of compares through a function pointer, rotations, a 32-byte node from the frame arena), then
// the tree's in-order walk at the send. With 1 the sorter appends (key, attach order, packet) to chunks in the frame arena (8, 16, 32 ... up to
// 256 entries of 16 bytes: no more arena than the tree's nodes), and First(), on the thread that sends (the frame-send worker in core plan C),
// copies them into a buffer of that thread and sorts it. The order is the tree's: the keys are unique except for a packet attached twice to one
// sorter, where the tree keeps the first insert, and so does the copy (equal keys sort by attach order, later repeats are dropped). Exact by
// construction: the same packets reach the send in the same order (check: the armhf harness's per-frame vgx hash, MSC_PKT_SORT=0 against 1).
// When the arena has no room for a chunk, the sorter puts what it holds into the tree, in attach order (the tree the plain code would have
// built), and goes on as before. Rendering only: no simulation state.
#include "np/pktsort.h"
struct NpPktChunk
{
    NpPktChunk* next;
    unsigned long n, cap, pad;
    NpPktSortEnt e[1];
};
namespace
{
int sNpPktSort = -1;   // (a plain word: read once, then the same value on every thread; no guarded static on the attach path)
bool NpPktSortOn()
{
    if (sNpPktSort < 0)
    {
        const char* e = getenv("MSC_PKT_SORT");
        sNpPktSort = (e != 0 && e[0] == '0') ? 0 : 1;
    }
    return sNpPktSort != 0;
}
// The sorted copy: one per thread that sends (only one sorter is walked at a time; owner says whose copy it is).
struct NpPktScratch
{
    NpPktSortEnt* buf;   // 2 x cap entries
    NpPktSortEnt* e;     // the sorted copy (one of the two halves)
    unsigned long n, cap;
    const GLTreePacketSorter* owner;
};
__thread NpPktScratch tNpScratch;
} // namespace

#ifdef NP_NATIVE
extern "C" volatile int gNpSimtCensus __attribute__((weak));   // block17 (src/Game/NpSimRun17.cpp)
extern "C" void NpSimtNoteGxRecord(void);
#endif
GLTreePacketSorter::GLTreePacketSorter()
    : m_NpHead(0)
    , m_NpTail(0)
    , m_NpCount(0)
    , m_NpPos(0)
    , m_NpFlat(NpPktSortOn())
    , m_NpScratch(0)
{
}

void GLTreePacketSorter::NpToTree()
{
    m_NpFlat = false;
    for (NpPktChunk* c = m_NpHead; c != 0; c = c->next)
        for (unsigned long i = 0; i < c->n; ++i)
            m_Tree.Add(c->e[i].key, c->e[i].value);
    m_NpHead = m_NpTail = 0;
    m_NpCount = 0;
}

bool GLTreePacketSorter::NpSortedCopy()
{
    NpPktScratch& s = tNpScratch;
    s.owner = 0;
    if (m_NpCount > s.cap)
    {
        unsigned long cap = s.cap != 0 ? s.cap : 256;
        while (cap < m_NpCount)
            cap *= 2;
        NpPktSortEnt* const grown = (NpPktSortEnt*)malloc(2 * cap * sizeof(NpPktSortEnt));   // (two halves: the merge sort's)
        if (grown == 0)
            return false;
        free(s.buf);
        s.buf = grown;
        s.cap = cap;
    }
    unsigned long n = 0;
    for (NpPktChunk* c = m_NpHead; c != 0; c = c->next)
    {
        memcpy(s.buf + n, c->e, c->n * sizeof(NpPktSortEnt));
        n += c->n;
    }
    NpPktSortEnt* const e = NpPktStableSort(s.buf, s.buf + s.cap, n);
    s.e = e;
    s.n = NpPktDropRepeats(e, n);   // (a repeat: the tree kept the first insert)
    s.owner = this;
    m_NpScratch = &s;
    return true;
}
#endif

const glModelPacket* GLTreePacketSorter::First()
{
#ifdef NP_NATIVE
    if (m_NpFlat)
    {
        if (!NpSortedCopy())
            NpToTree();   // (no memory for the copy: the tree, as the plain code)
        else
        {
            m_NpPos = 0;
            return Next();
        }
    }
#endif
    m_Iterator.Initialize(m_Tree.m_Root);
    return Next();
}

const glModelPacket* GLTreePacketSorter::Next()
{
#ifdef NP_NATIVE
    if (m_NpFlat)
    {
        // 1.3 armcpu9: the walk's own copy is found through m_NpScratch (set by NpSortedCopy on the thread that walks; one sorter is walked by one
        // thread). tNpScratch is emulated TLS on the Vita: each access is a call chain of about 90 instructions and a barrier, and Next runs per packet.
        NpPktScratch* const w = static_cast<NpPktScratch*>(m_NpScratch);
        if (w != 0 && w->owner == this)
            return m_NpPos < w->n ? w->e[m_NpPos++].value : 0;
        // (another sorter's walk in between, which the send never does: the same copy again)
        if (tNpScratch.owner != this && !NpSortedCopy())
            return 0;
        return m_NpPos < tNpScratch.n ? tNpScratch.e[m_NpPos++].value : 0;
    }
#endif
    if (m_Iterator.m_NumStackEntries == 0)
        return 0;

    const glModelPacket* pPacket =
        m_Iterator.m_Stack[m_Iterator.m_NumStackEntries - 1]->value;
    m_Iterator.Next();
    return pPacket;
}

void GLTreePacketSorter::AttachPacket(
    GLView* view, const glModelPacket* pPacket)
{
    GLPacketSortKey sortKey =
        ((GLPacketSortKey)GetSortKey(view, pPacket) << 32)
        | (unsigned long)pPacket;
#ifdef NP_NATIVE
    if (&gNpSimtCensus != 0 && gNpSimtCensus != 0) NpSimtNoteGxRecord();   // block17 (MSC_SIMT=1): a packet recorded on np_sim would be a draw call made by the step
    if (m_NpFlat)
    {
        const glModelPacket* const snap = GLSnapshotPacket(pPacket);
        NpPktChunk* c = m_NpTail;
        if (c == 0 || c->n == c->cap)
        {
            const unsigned long cap = c == 0 ? 8 : (c->cap < 256 ? c->cap * 2 : 256);
            NpPktChunk* const fresh = (NpPktChunk*)glplatFrameAllocTry(sizeof(NpPktChunk) + (cap - 1) * sizeof(NpPktSortEnt), GLM_Header);
            if (fresh == 0)
            {
                NpToTree();
                m_Tree.Add(sortKey, snap);
                return;
            }
            fresh->next = 0;
            fresh->n = 0;
            fresh->cap = cap;
            if (c != 0)
                c->next = fresh;
            else
                m_NpHead = fresh;
            m_NpTail = c = fresh;
        }
        NpPktSortEnt& e = c->e[c->n++];
        e.key = sortKey;
        e.seq = m_NpCount++;
        e.value = snap;
        return;
    }
#endif
    m_Tree.Add(sortKey, GLSnapshotPacket(pPacket));
}

PacketCallbackManager::PacketCallbackManager(
    GLView* view, GLViewPacketCallback cb)
    : m_View(view)
    , m_Cb(cb)
    , m_LastRaster((unsigned long)-1)
    , m_LastMatrix((unsigned long)-1)
    , m_LastProgram(0)
{
}

void PacketCallbackManager::DoCallback(
    const glModelPacket* p, unsigned int count)
{
    unsigned long flags = 0;

    if (m_LastRaster != p->rasterState)
    {
        flags |= 2;
        m_LastRaster = p->rasterState;
    }

    if (m_LastMatrix != p->matrix)
    {
        flags |= 4;
        m_LastMatrix = p->matrix;
    }

    if (p->materialProgram != m_LastProgram)
    {
        flags |= 8;
        m_LastProgram = p->materialProgram;
    }

    unsigned long stage = flags | 0x80;
    while (count != 0)
    {
        m_Cb(m_View, stage, p);
        --count;
    }
}
