#ifndef NL_GL_GLRENDERLIST_H
#define NL_GL_GLRENDERLIST_H

#include "NL/gl/glMemory.h"
#include "NL/gl/glPacketCallback.h"
#include "NL/gl/glView.h"
#include "NL/nlAVLTree.h"

class GLPacketSorter
{
public:
    static void* operator new(size_t size)
    {
        return glFrameAlloc(size, GLM_Header);
    }

    virtual const glModelPacket* First() = 0;
    virtual const glModelPacket* Next() = 0;
    virtual void AttachPacket(GLView*, const glModelPacket*) = 0;
};

/* NP (two-stage pipeline): the packet the send job reads. A copy in the frame arena (packet, parameter block, skin matrices) made when the packet is
 * attached, because the build of the next frame rewrites the persistent packet (matrix handle, parameters, skin matrices) while the job still reads it.
 * The sort keys keep using the original packet's address (same order as the serial program). Identity when the pipeline is off. */
#ifdef NP_NATIVE
const glModelPacket* GLSnapshotPacket(const glModelPacket* packet);
void GLRefreshPacketSnapshots();
unsigned long GLSnapshotFailures();   /* snapshots (or skin copies) the frame arena had no room for: the send of that frame must not overlap the next build */
void GLApplyDrawWriteBacks();   /* after a join: what the job's Draw calls wrote into packet copies, written into the persistent packets */
#else
static inline const glModelPacket* GLSnapshotPacket(const glModelPacket* packet) { return packet; }
#endif

typedef unsigned long long GLPacketSortKey;
typedef AVLTreeEntry<GLPacketSortKey, const glModelPacket*> GLPacketTreeEntry;

template <typename T>
class GLFrameAllocator
{
public:
    T* Allocate()
    {
        return (T*)glFrameAlloc(sizeof(T), GLM_Header);
    }

    void Allocate(T*& out)
    {
        out = Allocate();
    }

    void Delete(T* entry)
    {
        entry->~T();
    }
};

class GLPacketTree
    : public AVLTreeBase<GLPacketSortKey, const glModelPacket*,
          GLFrameAllocator<GLPacketTreeEntry>,
          DefaultKeyCompare<GLPacketSortKey> >
{
};

#ifdef NP_NATIVE
/* MSC_PKT_SORT (glRenderList.cpp): a sorter's packets as a list in the frame arena, sorted when the send walks it, in place of a balanced-tree
 * insert per attach on the game thread. The same packets in the same order as the tree (the keys hold the packet's address). */
struct NpPktSortEnt
{
    GLPacketSortKey key;
    unsigned long seq;   /* attach order (the sort is stable: of two equal keys the tree keeps the first insert, the copy the first entry) */
    const glModelPacket* value;
};
struct NpPktChunk;
#endif

class GLTreePacketSorter : public GLPacketSorter
{
public:
#ifdef NP_NATIVE
    GLTreePacketSorter();
#endif
    virtual const glModelPacket* First();
    virtual const glModelPacket* Next();
    virtual void AttachPacket(GLView*, const glModelPacket*);
    virtual unsigned long GetSortKey(GLView*, const glModelPacket*) = 0;

    GLPacketTree m_Tree;
    nlAVLTreeIterator<GLPacketSortKey, const glModelPacket*,
        DefaultKeyCompare<GLPacketSortKey> >
        m_Iterator;
#ifdef NP_NATIVE
    NpPktChunk* m_NpHead;       /* MSC_PKT_SORT: the attached packets, in attach order */
    NpPktChunk* m_NpTail;
    unsigned long m_NpCount;    /* entries in the chunks (attach order numbers them) */
    unsigned long m_NpPos;      /* the walk, in the sorted copy */
    bool m_NpFlat;              /* the list is in use (the switch on, and the frame arena had room every time) */
    void* m_NpScratch;          /* the walking thread's sorted copy (glRenderList.cpp NpPktScratch), found without thread-local access */
    void NpToTree();
    bool NpSortedCopy();
#endif
};

class GLTexturePacketSorter : public GLTreePacketSorter
{
public:
    virtual unsigned long GetSortKey(GLView*, const glModelPacket*);
};

class GLTransformedDepthPacketSorter : public GLTreePacketSorter
{
public:
    virtual unsigned long GetSortKey(GLView*, const glModelPacket*);
};

class GLTransformedMatrixDepthPacketSorter : public GLTreePacketSorter
{
public:
    GLTransformedMatrixDepthPacketSorter()
        : m_Sequence(0)
    {
    }

    virtual unsigned long GetSortKey(GLView*, const glModelPacket*);

    unsigned long m_Sequence;
};

class GLListPacketSorter : public GLPacketSorter
{
public:
    GLListPacketSorter()
        : m_Head(0)
        , m_Tail(0)
        , m_Current(0)
    {
    }

    virtual const glModelPacket* First()
    {
        m_Current = m_Head;
        return Next();
    }

    virtual const glModelPacket* Next()
    {
        if (m_Current == 0)
            return 0;
        const glModelPacket* packet = m_Current->entry;
        m_Current = m_Current->next;
        return packet;
    }

protected:
    u8 m_Allocator;
    ListEntry<const glModelPacket*>* m_Head;
    ListEntry<const glModelPacket*>* m_Tail;
    ListEntry<const glModelPacket*>* m_Current;
};

class GLUnsortedPacketSorter : public GLListPacketSorter
{
public:
    virtual void AttachPacket(GLView*, const glModelPacket* packet)
    {
        ListEntry<const glModelPacket*>* entry = (ListEntry<const glModelPacket*>*)glFrameAlloc(
            sizeof(ListEntry<const glModelPacket*>), GLM_Header);
        entry->entry = GLSnapshotPacket(packet);
        entry->next = 0;
        nlListAddEnd(&m_Head, &m_Tail, entry);
    }
};

class GLReversePacketSorter : public GLListPacketSorter
{
public:
    virtual void AttachPacket(GLView*, const glModelPacket* packet)
    {
        ListEntry<const glModelPacket*>* entry = (ListEntry<const glModelPacket*>*)glFrameAlloc(
            sizeof(ListEntry<const glModelPacket*>), GLM_Header);
        entry->next = 0;
        entry->entry = GLSnapshotPacket(packet);
        nlListAddStart(&m_Head, entry, &m_Tail);
    }
};

#endif // NL_GL_GLRENDERLIST_H
