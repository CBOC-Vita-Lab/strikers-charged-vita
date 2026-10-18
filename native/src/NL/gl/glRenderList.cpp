#include <revolution/types.h>

#include "NL/gl/glRenderList.h"

#include "NL/gl/glMatrix.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/platvmath.h"
#include "Game/MathHelpers.h"

#ifdef NP_NATIVE
#include "np/framesend.h"
#include "NL/glx/glxMemory.h"
#include "NL/glx/glxSwap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// copy the skin matrix buffer the parameter block points at (the buffer is rewritten in place every frame)
inline void CopySkin(SnapRec* r)
{
    unsigned long* buffer = (unsigned long*)(r->dstParams + r->skinOffset);
    if (buffer[0] == 0 || buffer[1] == 0)
        return;
    if (buffer[1] <= r->skinCap && r->dstSkin != 0)
    {
        memcpy(r->dstSkin, (const void*)buffer[0], buffer[1]);
        buffer[0] = (unsigned long)r->dstSkin;
        return;
    }
    void* grown = glplatFrameAllocTry(buffer[1], GLM_Header);
    if (grown != 0)
    {
        memcpy(grown, (const void*)buffer[0], buffer[1]);
        buffer[0] = (unsigned long)grown;
        ++sSnapGrown;
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
    Fill(r);
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
    static const bool refresh = [] { const char* e = getenv("MSC_FS_SNAPREFRESH"); return !(e != 0 && e[0] == '0'); }();   // (a switch for the equality bisection)
    GLApplyDrawWriteBacks();   // (no job runs here: the join of the last one is done)
    sJobHead = sSnapHead;      // the job of this frame finds the copies it draws (GLDrawSetWrap)
    sLookupBuilt = false;
    static const bool dbg = getenv("MSC_FS_SNAPDBG") != 0;
    if (dbg)   // the equality bisection: what changed in a persistent packet between the attach and the fork
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
    if (refresh)
        for (SnapRec* r = sSnapHead; r != 0; r = r->next)
            Fill(r);
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

const glModelPacket* GLTreePacketSorter::First()
{
    m_Iterator.Initialize(m_Tree.m_Root);
    return Next();
}

const glModelPacket* GLTreePacketSorter::Next()
{
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
