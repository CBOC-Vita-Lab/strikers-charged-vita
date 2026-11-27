/*************************************************************************
 *                                                                       *
 * Open Dynamics Engine, Copyright (C) 2001-2003 Russell L. Smith.       *
 * All rights reserved.  Email: russ@q12.org   Web: www.q12.org          *
 *                                                                       *
 * This library is free software; you can redistribute it and/or         *
 * modify it under the terms of EITHER:                                  *
 *   (1) The GNU Lesser General Public License as published by the Free  *
 *       Software Foundation; either version 2.1 of the License, or (at  *
 *       your option) any later version. The text of the GNU Lesser      *
 *       General Public License is included with this library in the     *
 *       file LICENSE.TXT.                                               *
 *   (2) The BSD-style license that is included with this library in     *
 *       the file LICENSE-BSD.TXT.                                       *
 *                                                                       *
 * This library is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the files    *
 * LICENSE.TXT and LICENSE-BSD.TXT for more details.                     *
 *                                                                       *
 *************************************************************************/

/*

spaces

*/

#include <ode/common.h>
#include <ode/matrix.h>
#include <ode/collision_space.h>
#include <ode/collision.h>
#include "collision_kernel.h"

#include "collision_space_internal.h"
#include "np_broadphase.h"
#include "np/gsplit.h"   // MSC_GSPLIT counters
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

extern "C"
{
    double floor(double);
    double frexp(double, int*);
    double ldexp(double, int);
    void* malloc(size_t);
    void free(void*);
    void* memcpy(void*, const void*, size_t);
    void* memset(void*, int, size_t);
}

extern const float __float_max;
#define dInfinity __float_max

#ifdef _MSC_VER
#pragma warning(disable : 4291) // for VC++, no complaints about "no matching operator delete found"
#endif

unsigned npGeomEpoch;   // np_broadphase.h: counts every change of a space's list, a geom's enabled flag or a cached AABB

//****************************************************************************
// make the geom dirty by setting the GEOM_DIRTY and GEOM_BAD_AABB flags
// and moving it to the front of the space's list. all the parents of a
// dirty geom also become dirty.

static inline void geomMoved(dxGeom* geom)
{
    dAASSERT(geom);
    npGeomEpoch++;

    // from the bottom of the space heirarchy up, process all clean geoms
    // turning them into dirty geoms.
    dxSpace* parent = geom->parent_space;

    while (parent && (geom->gflags & GEOM_DIRTY) == 0)
    {
        CHECK_NOT_LOCKED(parent);
        geom->gflags |= GEOM_DIRTY | GEOM_AABB_BAD;
        parent->dirty(geom);
        geom = parent;
        parent = parent->parent_space;
    }

    // all the remaining dirty geoms must have their AABB_BAD flags set, to
    // ensure that their AABBs get recomputed
    while (geom)
    {
        geom->gflags |= GEOM_DIRTY | GEOM_AABB_BAD;
        CHECK_NOT_LOCKED(geom->parent_space);
        geom = geom->parent_space;
    }
}
void dGeomMoved(dxGeom* geom)
{
    geomMoved(geom);
}

#define GEOM_ENABLED(g) ((g)->gflags & GEOM_ENABLED)

//****************************************************************************
// dxSpace

dxSpace::dxSpace(dSpaceID _space)
    : dxGeom(_space, 0)
{
    count = 0;
    first = 0;
    cleanup = 1;
    current_index = 0;
    current_geom = 0;
    lock_count = 0;
}

dxSpace::~dxSpace()
{
    CHECK_NOT_LOCKED(this);
    if (cleanup)
    {
        // note that destroying each geom will call remove()
        dxGeom *g, *n;
        for (g = first; g; g = n)
        {
            n = g->next;
            dGeomDestroy(g);
        }
    }
    else
    {
        dxGeom *g, *n;
        for (g = first; g; g = n)
        {
            n = g->next;
            remove(g);
        }
    }
}

void dxSpace::computeAABB()
{
    if (first)
    {
        int i;
        dReal a[6];
        a[0] = 3.402823466e+38F;
        a[1] = -3.402823466e+38F;
        a[2] = 3.402823466e+38F;
        a[3] = -3.402823466e+38F;
        a[4] = 3.402823466e+38F;
        a[5] = -3.402823466e+38F;
        for (dxGeom* g = first; g; g = g->next)
        {
            g->recomputeAABB();
            for (i = 0; i < 6; i += 2)
                if (g->aabb[i] < a[i])
                    a[i] = g->aabb[i];
            for (i = 1; i < 6; i += 2)
                if (g->aabb[i] > a[i])
                    a[i] = g->aabb[i];
        }
        memcpy(aabb, a, 6 * sizeof(dReal));
    }
    else
    {
        dSetZero(aabb, 6);
    }
}

void dxSpace::setCleanup(int mode)
{
    cleanup = (mode != 0);
}

int dxSpace::getCleanup()
{
    return cleanup;
}

int dxSpace::query(dxGeom* geom)
{
    dAASSERT(geom);
    return (geom->parent_space == this);
}

int dxSpace::getNumGeoms()
{
    return count;
}

// the dirty geoms are numbered 0..k, the clean geoms are numbered k+1..count-1

dxGeom* dxSpace::getGeom(int i)
{
    dUASSERT(i >= 0 && i < count, "index out of range");
    if (current_geom && current_index == i - 1)
    {
        current_geom = current_geom->next;
        current_index = i;
        return current_geom;
    }
    else
    {
        dxGeom* g = first;
        for (int j = 0; j < i; j++)
        {
            if (g)
                g = g->next;
            else
                return 0;
        }
        current_geom = g;
        current_index = i;
        return g;
    }
}

void dxSpace::add(dxGeom* geom)
{
    CHECK_NOT_LOCKED(this);
    dAASSERT(geom);
    dUASSERT(geom->parent_space == 0 && geom->next == 0,
        "geom is already in a space");
    npGeomEpoch++;

    // add
    geom->parent_space = this;
    geom->spaceAdd(&first);
    count++;

    // enumerator has been invalidated
    current_geom = 0;

    // new geoms are added to the front of the list and are always
    // considered to be dirty. as a consequence, this space and all its
    // parents are dirty too.
    geom->gflags |= GEOM_DIRTY | GEOM_AABB_BAD;
    geomMoved(this);
}

void dxSpace::remove(dxGeom* geom)
{
    CHECK_NOT_LOCKED(this);
    dAASSERT(geom);
    dUASSERT(geom->parent_space == this, "object is not in this space");
    npGeomEpoch++;

    // remove
    geom->spaceRemove();
    count--;

    // safeguard
    geom->next = 0;
    geom->tome = 0;
    geom->parent_space = 0;

    // enumerator has been invalidated
    current_geom = 0;

    // the bounding box of this space (and that of all the parents) may have
    // changed as a consequence of the removal.
    geomMoved(this);
}

void dxSpace::dirty(dxGeom* geom)
{
    geom->spaceRemove();
    geom->spaceAdd(&first);
}

//****************************************************************************
// simple space - reports all n^2 object intersections

struct dxSimpleSpace : public dxSpace
{
    dxSimpleSpace(dSpaceID _space);
    ~dxSimpleSpace();
    void cleanGeoms();
    void collide(void* data, dNearCallback* callback);
    void collide2(void* data, dxGeom* geom, dNearCallback* callback);
};

dxSimpleSpace::dxSimpleSpace(dSpaceID _space)
    : dxSpace(_space)
{
    type = dSimpleSpaceClass;
}

void dxSimpleSpace::cleanGeoms()
{
    // compute the AABBs of all dirty geoms, and clear the dirty flags
    lock_count++;
    for (dxGeom* g = first; g && (g->gflags & GEOM_DIRTY); g = g->next)
    {
        if (IS_SPACE(g))
        {
            ((dxSpace*)g)->cleanGeoms();
        }
        g->recomputeAABB();
        g->gflags &= (~(GEOM_DIRTY | GEOM_AABB_BAD));
    }
    lock_count--;
}

// MSC_ODE_BROADPHASE (np_broadphase.h): 0 = the plain loop, 2 = the plain loop plus a check of every skip decision of the call against the float test (one log line
// each 600 calls: pairs, skips, wrong skips, calls that left the flat walk).
static int NpBroadphaseMode()
{
    static int mode = -1;
    if (mode < 0)
    {
        const char* e = getenv("MSC_ODE_BROADPHASE");
        mode = (e && e[0] == '0') ? 0 : (e && e[0] == '2') ? 2 : 1;
    }
    return mode;
}

// MSC_ODE_BPFULL (default 0 = off; 1 on, needs MSC_ODE_BROADPHASE != 0): the flat walk also skips the pairs collideAABBs returns from before it calls anything because of the
// geoms' Z extents, their category / collide bits or a shared body (the 884 collideAABBs calls of a step that make 71 callbacks). Pure tests of the geoms' own fields:
// the survivors are the same pairs in the same order. 0: only the X / Y test. 2: as 1, and a run of three or more consecutive geoms on one body (a character's bone geoms)
// is stepped over in one test of the run's union box (np_broadphase.h NpBpSegs); exact, the same survivors in the same order. Only where unsigned long is 32 bits (the console, host32).
static int NpBpFullMode()
{
    static int mode = -1;
    if (mode < 0)
    {
        const char* e = getenv("MSC_ODE_BPFULL");
        mode = (sizeof(unsigned long) == 4 && e && e[0] == '1') ? 1 : (sizeof(unsigned long) == 4 && e && e[0] == '2') ? 2 : 0;
    }
    return mode;
}

enum { NP_BP_MIN = 8, NP_BP_MAX = 512 };

static unsigned NpBpBits(dReal v)
{
    unsigned bits;
    memcpy(&bits, &v, 4);
    return bits;
}

// The enabled geoms of the list, in list order, into g[] and the four key arrays. 0 = this call takes the plain loop.
static int NpBpSnapshot(dxGeom* first, dxGeom** g, int* lox, int* hix, int* loy, int* hiy, int capacity, const NpBpExtra* x)
{
    int n = 0;
    for (dxGeom* p = first; p; p = p->next)
    {
        if (p->gflags & (GEOM_DIRTY | GEOM_AABB_BAD))
            return 0;
        if (GEOM_ENABLED(p))
        {
            if (n == capacity)
                return 0;
            const unsigned b0 = NpBpBits(p->aabb[0]), b1 = NpBpBits(p->aabb[1]), b2 = NpBpBits(p->aabb[2]), b3 = NpBpBits(p->aabb[3]);
            if (!(NpBpKeyable(b0) & NpBpKeyable(b1) & NpBpKeyable(b2) & NpBpKeyable(b3)))
                return 0;
            if (x)
            {
                const unsigned b4 = NpBpBits(p->aabb[4]), b5 = NpBpBits(p->aabb[5]);
                if (!(NpBpKeyable(b4) & NpBpKeyable(b5)))
                    return 0;
                x->loz[n] = NpBpKey(b4);
                x->hiz[n] = NpBpKey(b5);
                x->cat[n] = (unsigned)p->category_bits;
                x->col[n] = (unsigned)p->collide_bits;
                x->body[n] = (unsigned)(uintptr_t)p->body;
                static int brk = -1;   // MSC_ODE_BPFULL_BREAK=1: proof mutant, the flat copy of every 3rd geom's category bits is zeroed (must change the trace and be counted by mode 2); =2: the run mutant (NpBpBuildSegs)
                if (brk < 0)
                {
                    const char* e = getenv("MSC_ODE_BPFULL_BREAK");
                    brk = (e && e[0] == '1') ? 1 : (e && e[0] == '2') ? 2 : 0;
                }
                if (brk == 1 && n % 3 == 0)
                {
                    x->cat[n] = 0;
                    x->col[n] = 0;
                }
            }
            g[n] = p;
            lox[n] = NpBpKey(b0);
            hix[n] = NpBpKey(b1);
            loy[n] = NpBpKey(b2);
            hiy[n] = NpBpKey(b3);
            n++;
        }
    }
    NpBpPad(lox, hix, loy, hiy, n);
    if (x)
        NpBpPadExtra(*x, n);
    return n;
}

// The plain walk, entered at any pair: g2 = 0 starts at the top of g1's turn, otherwise the walk goes on after the pair (g1, g2), reading the list as it is now,
// which is what the plain loop would do after a callback that moved, added, removed or switched a geom. The enabled test of g1 was made when its turn began.
static void NpPlainWalk(dxGeom* g1, dxGeom* g2, void* data, dNearCallback* callback)
{
    if (g1 && g2)
    {
        for (g2 = g2->next; g2; g2 = g2->next)
        {
            if (GEOM_ENABLED(g2))
            {
                collideAABBs(g1, g2, data, callback);
            }
        }
        g1 = g1->next;
    }
    for (; g1; g1 = g1->next)
    {
        if (GEOM_ENABLED(g1))
        {
            for (g2 = g1->next; g2; g2 = g2->next)
            {
                if (GEOM_ENABLED(g2))
                {
                    collideAABBs(g1, g2, data, callback);
                }
            }
        }
    }
}

// MSC_ODE_BROADPHASE=2. Decides nothing: compares each decision with the float test and counts.
static unsigned npBpLeft;

static unsigned long long sBpRunSkipped;

static void NpBpVerify(dxGeom** g, const int* lox, const int* hix, const int* loy, const int* hiy, int n, const NpBpExtra* x, NpBpSegs* S)
{
    static unsigned calls, wrong;
    static unsigned long long pairs, skips;
    calls++;
    for (int i = 0; i < n; i++)
    {
        int next = S ? NpBpNextSeg(lox, hix, loy, hiy, *x, *S, i, i + 1, n) : x ? NpBpNextFull(lox, hix, loy, hiy, *x, i, i + 1, n) : NpBpNext(lox, hix, loy, hiy, i, i + 1, n);
        for (int j = i + 1; j < n; j++)
        {
            const dReal* a = g[i]->aabb;
            const dReal* b = g[j]->aabb;
            const int skip = j != next;
            int floatReject = a[0] > b[1] || a[1] < b[0] || a[2] > b[3] || a[3] < b[2];
            if (x)   // the first three early outs of collideAABBs, from the geoms themselves, then the Z bounds
                floatReject = (g[i]->body == g[j]->body && g[i]->body) || (((g[i]->category_bits & g[j]->collide_bits) || (g[j]->category_bits & g[i]->collide_bits)) == 0) || floatReject
                              || a[4] > b[5] || a[5] < b[4];
            pairs++;
            if (skip)
                skips++;
            else
                next = S ? NpBpNextSeg(lox, hix, loy, hiy, *x, *S, i, j + 1, n) : x ? NpBpNextFull(lox, hix, loy, hiy, *x, i, j + 1, n) : NpBpNext(lox, hix, loy, hiy, i, j + 1, n);
            if (skip != floatReject)
                wrong++;
        }
    }
    if (calls % 600 == 0)
    {
        printf("ode broadphase: calls=%u pairs=%llu skipped=%llu wrong=%u left=%u runskipped=%llu\n", calls, pairs, skips, wrong, npBpLeft, S ? S->skipped : 0ull);
        fflush(stdout);
    }
}

// MSC_ODE_BPFULL / MSC_ODE_BROADPHASE action counters (a line at exit): spaces walked flat, geom pairs in them, pairs handed on to collideAABBs, calls that left the flat walk
static unsigned long long sBpSpaces, sBpPairs, sBpSurvivors, sBpPlain;
static void NpSim26BpReport(void);   // MSC_SIM26_BP (below)
static void NpBpReport(void)
{
    fprintf(stderr, "[bpfull] flat spaces %llu, pairs %llu, passed to collideAABBs %llu (avoided %llu), plain walks %llu, geoms of runs stepped over %llu, full=%d\n", sBpSpaces, sBpPairs, sBpSurvivors,
        sBpPairs - sBpSurvivors, sBpPlain, sBpRunSkipped, NpBpFullMode());
    NpSim26BpReport();
}

// MSC_SIM26_BP (wave 12, sim26; needs MSC_ODE_BPFULL=2): 0 off (default), 1 the rows of a character's bone run walk the run's candidate list (np_broadphase.h
// NpBpRunCands; exact: the same survivors in the same order), 2 verify: the list is built and its survivors compared with the walk's for every run row, the
// calls come from the walk (count in the [sim26bp] line: wrong must be 0). MSC_SIM26_BP_BREAK=1 / 2: proof mutants (the last / every candidate dropped).
static int NpSim26BpMode()
{
    static int mode = -1;
    if (mode < 0)
    {
        const char* e = getenv("MSC_SIM26_BP");
        mode = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
    }
    return mode;
}
static int NpSim26BpBreak()
{
    static int brk = -1;
    if (brk < 0)
    {
        const char* e = getenv("MSC_SIM26_BP_BREAK");
        brk = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
    }
    return brk;
}
static unsigned long long sS26Lists, sS26Rows, sS26Cands, sS26Calls, sS26RunSkips, sS26Checked, sS26Wrong;
static void NpSim26BpReport(void)
{
    if (NpSim26BpMode() == 0)
        return;
    fprintf(stderr, "[sim26bp] mode=%d break=%d run lists %llu (candidates %llu, later runs stepped over %llu), run rows %llu, their calls %llu, verify rows %llu wrong %llu\n", NpSim26BpMode(),
        NpSim26BpBreak(), sS26Lists, sS26Cands, sS26RunSkips, sS26Rows, sS26Calls, sS26Checked, sS26Wrong);
}

void dxSimpleSpace::collide(void* data, dNearCallback* callback)
{
    dAASSERT(callback);

    lock_count++;
    cleanGeoms();
    NP_GSC(NPGC_GEOMS, count);
    NP_GSC(NPGC_PAIRS, (unsigned)count * (unsigned)(count > 0 ? count - 1 : 0) / 2u);

    const int mode = NpBroadphaseMode();
    if (mode != 0 && count >= NP_BP_MIN && count <= NP_BP_MAX)
    {
        const int cap = count;
        dxGeom** g = (dxGeom**)ALLOCA(cap * sizeof(dxGeom*));
        int* keys = (int*)ALLOCA(4 * (cap + NP_BP_PAD) * sizeof(int));
        int* lox = keys;
        int* hix = lox + cap + NP_BP_PAD;
        int* loy = hix + cap + NP_BP_PAD;
        int* hiy = loy + cap + NP_BP_PAD;
        const int full = NpBpFullMode();
        NpBpExtra ex = { 0, 0, 0, 0, 0 };
        int* ints = 0;
        if (full)
        {
            ints = (int*)ALLOCA(5 * (cap + NP_BP_PAD) * sizeof(int));
            ex.loz = ints;
            ex.hiz = ex.loz + cap + NP_BP_PAD;
            ex.cat = (unsigned*)(ex.hiz + cap + NP_BP_PAD);
            ex.col = ex.cat + cap + NP_BP_PAD;
            ex.body = ex.col + cap + NP_BP_PAD;
        }
        const int n = NpBpSnapshot(first, g, lox, hix, loy, hiy, cap, full ? &ex : 0);
        if (n != 0)
        {
            // BPFULL=2: the runs of one body (a character's bone geoms), only when the list has one
            NpBpSegs sg;
            NpBpSegs* segs = 0;
            if (full == 2)
            {
                const int m = cap + 1;
                int* si = (int*)ALLOCA(8 * m * sizeof(int));
                unsigned* su = (unsigned*)ALLOCA(2 * m * sizeof(unsigned));
                unsigned char* sr = (unsigned char*)ALLOCA(m);
                sg.end = si;
                sg.segOf = si + m;
                sg.ulox = si + 2 * m; sg.uhix = si + 3 * m; sg.uloy = si + 4 * m; sg.uhiy = si + 5 * m; sg.uloz = si + 6 * m; sg.uhiz = si + 7 * m;
                sg.ucat = su; sg.ucol = su + m;
                sg.run = sr;
                sg.skipped = 0;
                static int mut = -1;
                if (mut < 0)
                {
                    const char* e = getenv("MSC_ODE_BPFULL_BREAK");
                    mut = (e && e[0] == '2') ? 1 : 0;
                }
                NpBpBuildSegs(sg, lox, hix, loy, hiy, ex, n, mut);
                if (sg.hasRun)
                    segs = &sg;
            }
            if (mode == 2)
            {
                NpBpVerify(g, lox, hix, loy, hiy, n, full ? &ex : 0, segs);
                const unsigned before = npGeomEpoch;
                NpPlainWalk(first, 0, data, callback);
                npBpLeft += npGeomEpoch != before;
                lock_count--;
                return;
            }
            const unsigned epoch = npGeomEpoch;
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                atexit(NpBpReport);
            }
            sBpSpaces++;
            if (sBpSpaces == 1 || sBpSpaces % 1500 == 0)
                NpBpReport();   // (the harness may leave by _exit)
            sBpPairs += (unsigned long long)n * (unsigned long long)(n - 1) / 2u;
            // MSC_SIM26_BP: the rows of a run walk the run's candidate list (np_broadphase.h NpBpRunCands)
            const int rowsMode = segs ? NpSim26BpMode() : 0;
            int* cand = rowsMode ? (int*)ALLOCA((n + 1) * sizeof(int)) : 0;
            int* ref = rowsMode == 2 ? (int*)ALLOCA((n + 1) * sizeof(int)) : 0;
            int candSeg = -1, candCount = 0;
            for (int i = 0; i < n; i++)
            {
                if (rowsMode)
                {
                    const int s = segs->segOf[i];
                    if (segs->run[s])
                    {
                        if (s != candSeg)
                        {
                            candSeg = s;
                            candCount = NpBpRunCands(lox, hix, loy, hiy, ex, *segs, s, ex.body[i], segs->end[s], n, cand, NpSim26BpBreak(), &sS26RunSkips);
                            sS26Lists++;
                            sS26Cands += (unsigned)candCount;
                        }
                        sS26Rows++;
                        if (rowsMode == 2)
                        {
                            // verify: the survivors of the old walk, compared with those of the list; the calls are made from the old walk
                            int nr = 0;
                            for (int j = NpBpNextSeg(lox, hix, loy, hiy, ex, *segs, i, i + 1, n); j < n; j = NpBpNextSeg(lox, hix, loy, hiy, ex, *segs, i, j + 1, n))
                                ref[nr++] = j;
                            int k2 = 0, ok = 1;
                            for (int k = 0; k < candCount; k++)
                            {
                                const int j = cand[k];
                                if (j <= i || NpBpRejectFull(lox, hix, loy, hiy, ex, i, j))
                                    continue;
                                if (k2 >= nr || ref[k2] != j)
                                    ok = 0;
                                k2++;
                            }
                            if (k2 != nr)
                                ok = 0;
                            sS26Checked++;
                            sS26Wrong += ok ? 0u : 1u;
                            for (int k = 0; k < nr; k++)
                            {
                                sBpSurvivors++;
                                collideAABBs(g[i], g[ref[k]], data, callback);
                                if (npGeomEpoch != epoch)
                                {
                                    sBpPlain++;
                                    npBpLeft++;
                                    NpPlainWalk(g[i], g[ref[k]], data, callback);
                                    lock_count--;
                                    return;
                                }
                            }
                            continue;
                        }
                        for (int k = 0; k < candCount; k++)
                        {
                            const int j = cand[k];
                            if (NpBpRejectFull(lox, hix, loy, hiy, ex, i, j))
                                continue;
                            sS26Calls++;
                            sBpSurvivors++;
                            collideAABBs(g[i], g[j], data, callback);
                            if (npGeomEpoch != epoch)
                            {
                                sBpPlain++;
                                npBpLeft++;
                                NpPlainWalk(g[i], g[j], data, callback);
                                lock_count--;
                                return;
                            }
                        }
                        continue;
                    }
                }
                for (int j = segs ? NpBpNextSeg(lox, hix, loy, hiy, ex, *segs, i, i + 1, n) : full ? NpBpNextFull(lox, hix, loy, hiy, ex, i, i + 1, n) : NpBpNext(lox, hix, loy, hiy, i, i + 1, n); j < n;
                     j = segs ? NpBpNextSeg(lox, hix, loy, hiy, ex, *segs, i, j + 1, n) : full ? NpBpNextFull(lox, hix, loy, hiy, ex, i, j + 1, n) : NpBpNext(lox, hix, loy, hiy, i, j + 1, n))
                {
                    sBpSurvivors++;
                    collideAABBs(g[i], g[j], data, callback);
                    if (npGeomEpoch != epoch)
                    {
                        sBpPlain++;
                        if (segs) sBpRunSkipped += segs->skipped;
                        // the callback changed the list, an enabled flag or an AABB: the arrays say nothing about the rest
                        npBpLeft++;
                        NpPlainWalk(g[i], g[j], data, callback);
                        lock_count--;
                        return;
                    }
                }
            }
            if (segs) sBpRunSkipped += segs->skipped;
            lock_count--;
            return;
        }
    }

    // intersect all bounding boxes
    NpPlainWalk(first, 0, data, callback);

    lock_count--;
}

void dxSimpleSpace::collide2(void* data, dxGeom* geom,
    dNearCallback* callback)
{
    dAASSERT(geom && callback);

    lock_count++;
    cleanGeoms();
    geom->recomputeAABB();

    // intersect bounding boxes
    for (dxGeom* g = first; g; g = g->next)
    {
        if (GEOM_ENABLED(g))
        {
            collideAABBs(g, geom, data, callback);
        }
    }

    lock_count--;
}

//****************************************************************************
// utility stuff for hash table space

// kind of silly, but oh well...
#ifndef MAXINT
#define MAXINT ((int)((((unsigned int)(-1)) << 1) >> 1))
#endif

// prime[i] is the largest prime smaller than 2^i
#define NUM_PRIMES 31
static long int prime[NUM_PRIMES] = { 1L, 2L, 3L, 7L, 13L, 31L, 61L, 127L, 251L, 509L, 1021L, 2039L, 4093L, 8191L, 16381L, 32749L, 65521L, 131071L, 262139L, 524287L, 1048573L, 2097143L, 4194301L, 8388593L, 16777213L, 33554393L, 67108859L, 134217689L, 268435399L, 536870909L, 1073741789L };

// an axis aligned bounding box in the hash table
struct dxAABB
{
    dxAABB* next;   // next in the list of all AABBs
    int level;      // the level this is stored in (cell size = 2^level)
    int dbounds[6]; // AABB bounds, discretized to cell size
    dxGeom* geom;   // corresponding geometry object (AABB stored here)
    int index;      // index of this AABB, starting from 0
};

// a hash table node that represents an AABB that intersects a particular cell
// at a particular level
struct Node
{
    Node* next;   // next node in hash table collision list, 0 if none
    int x, y, z;  // cell position in space, discretized to cell size
    dxAABB* aabb; // axis aligned bounding box that intersects this cell
};

// return the `level' of an AABB. the AABB will be put into cells at this
// level - the cell size will be 2^level. the level is chosen to be the
// smallest value such that the AABB occupies no more than 8 cells, regardless
// of its placement. this means that:
//	size/2 < q <= size
// where q is the maximum AABB dimension.

static int findLevel(dReal bounds[6])
{
    if (bounds[0] <= -dInfinity || bounds[1] >= dInfinity || bounds[2] <= -dInfinity || bounds[3] >= dInfinity || bounds[4] <= -dInfinity || bounds[5] >= dInfinity)
    {
        return MAXINT;
    }

    // compute q
    dReal q, q2;
    q = bounds[1] - bounds[0];  // x bounds
    q2 = bounds[3] - bounds[2]; // y bounds
    if (q2 > q)
        q = q2;
    q2 = bounds[5] - bounds[4]; // z bounds
    if (q2 > q)
        q = q2;

    // find level such that 0.5 * 2^level < q <= 2^level
    int level;
    frexp(q, &level); // q = (0.5 .. 1.0) * 2^level (definition of frexp)
    return level;
}

// find a virtual memory address for a cell at the given level and x,y,z
// position.
// @@@ currently this is not very sophisticated, e.g. the scaling
// factors could be better designed to avoid collisions, and they should
// probably depend on the hash table physical size.

static unsigned long getVirtualAddress(int level, int x, int y, int z)
{
    return level * 1000 + x * 100 + y * 10 + z;
}

//****************************************************************************
// hash space

struct dxHashSpace : public dxSpace
{
    int global_minlevel; // smallest hash table level to put AABBs in
    int global_maxlevel; // objects that need a level larger than this will be
                         // put in a "big objects" list instead of a hash table

    dxHashSpace(dSpaceID _space);
    void setLevels(int minlevel, int maxlevel);
    void getLevels(int* minlevel, int* maxlevel);
    void cleanGeoms();
    void collide(void* data, dNearCallback* callback);
    void collide2(void* data, dxGeom* geom, dNearCallback* callback);
};

dxHashSpace::dxHashSpace(dSpaceID _space)
    : dxSpace(_space)
{
    type = dHashSpaceClass;
    global_minlevel = -3;
    global_maxlevel = 10;
}

void dxHashSpace::setLevels(int minlevel, int maxlevel)
{
    dAASSERT(minlevel <= maxlevel);
    global_minlevel = minlevel;
    global_maxlevel = maxlevel;
}

void dxHashSpace::getLevels(int* minlevel, int* maxlevel)
{
    if (minlevel)
        *minlevel = global_minlevel;
    if (maxlevel)
        *maxlevel = global_maxlevel;
}

void dxHashSpace::cleanGeoms()
{
    // compute the AABBs of all dirty geoms, and clear the dirty flags
    lock_count++;
    for (dxGeom* g = first; g && (g->gflags & GEOM_DIRTY); g = g->next)
    {
        if (IS_SPACE(g))
        {
            ((dxSpace*)g)->cleanGeoms();
        }
        g->recomputeAABB();
        g->gflags &= (~(GEOM_DIRTY | GEOM_AABB_BAD));
    }
    lock_count--;
}

void dxHashSpace::collide(void* data, dNearCallback* callback)
{
    dAASSERT(this && callback);
    dxGeom* geom;
    dxAABB* aabb;
    int i, maxlevel;

    // 0 or 1 geoms can't collide with anything
    if (count < 2)
        return;

    lock_count++;
    cleanGeoms();

    // create a list of auxiliary information for all geom axis aligned bounding
    // boxes. set the level for all AABBs. put AABBs larger than the space's
    // global_maxlevel in the big_boxes list, check everything else against
    // that list at the end. for AABBs that are not too big, record the maximum
    // level that we need.

    int n = 0;              // number of AABBs in main list
    dxAABB* first_aabb = 0; // list of AABBs in hash table
    dxAABB* big_boxes = 0;  // list of AABBs too big for hash table
    maxlevel = global_minlevel - 1;
    for (geom = first; geom; geom = geom->next)
    {
        if (!GEOM_ENABLED(geom))
        {
            continue;
        }
        dxAABB* aabb = (dxAABB*)ALLOCA(sizeof(dxAABB));
        aabb->geom = geom;
        // compute level, but prevent cells from getting too small
        int level = findLevel(geom->aabb);
        if (level < global_minlevel)
            level = global_minlevel;
        if (level <= global_maxlevel)
        {
            // aabb goes in main list
            aabb->next = first_aabb;
            first_aabb = aabb;
            aabb->level = level;
            if (level > maxlevel)
                maxlevel = level;
            // cellsize = 2^level
            dReal cellsize = (dReal)ldexp(1.0, level);
            // discretize AABB position to cell size
            for (i = 0; i < 6; i++)
                aabb->dbounds[i] = (int)
                    floor(geom->aabb[i] / cellsize);
            // set AABB index
            aabb->index = n;
            n++;
        }
        else
        {
            // aabb is too big, put it in the big_boxes list. we don't care about
            // setting level, dbounds, index, or the maxlevel
            aabb->next = big_boxes;
            big_boxes = aabb;
        }
    }

    // for `n' objects, an n*n array of bits is used to record if those objects
    // have been intersection-tested against each other yet. this array can
    // grow large with high n, but oh well...
    int tested_rowsize = (n + 7) >> 3; // number of bytes needed for n bits
    unsigned char* tested = (unsigned char*)malloc(n * tested_rowsize);
    memset(tested, 0, n * tested_rowsize);

    // create a hash table to store all AABBs. each AABB may take up to 8 cells.
    // we use chaining to resolve collisions, but we use a relatively large table
    // to reduce the chance of collisions.

    // compute hash table size sz to be a prime > 8*n
    for (i = 0; i < NUM_PRIMES; i++)
    {
        if (prime[i] >= (8 * n))
            break;
    }
    if (i >= NUM_PRIMES)
        i = NUM_PRIMES - 1; // probably pointless
    int sz = prime[i];

    // allocate and initialize hash table node pointers
    Node** table = (Node**)ALLOCA(sizeof(Node*) * sz);
    for (i = 0; i < sz; i++)
        table[i] = 0;

    // add each AABB to the hash table (may need to add it to up to 8 cells)
    for (aabb = first_aabb; aabb; aabb = aabb->next)
    {
        int* dbounds = aabb->dbounds;
        for (int xi = dbounds[0]; xi <= dbounds[1]; xi++)
        {
            for (int yi = dbounds[2]; yi <= dbounds[3]; yi++)
            {
                for (int zi = dbounds[4]; zi <= dbounds[5]; zi++)
                {
                    // get the hash index
                    unsigned long hi = getVirtualAddress(aabb->level, xi, yi, zi) % sz;
                    // add a new node to the hash table
                    Node* node = (Node*)malloc(sizeof(Node));
                    node->x = xi;
                    node->y = yi;
                    node->z = zi;
                    node->aabb = aabb;
                    node->next = table[hi];
                    table[hi] = node;
                }
            }
        }
    }

    // now that all AABBs are loaded into the hash table, we do the actual
    // collision detection. for all AABBs, check for other AABBs in the
    // same cells for collisions, and then check for other AABBs in all
    // intersecting higher level cells.

    int db[6]; // discrete bounds at current level
    for (aabb = first_aabb; aabb; aabb = aabb->next)
    {
        // we are searching for collisions with aabb
        for (i = 0; i < 6; i++)
            db[i] = aabb->dbounds[i];
        for (int level = aabb->level; level <= maxlevel; level++)
        {
            for (int xi = db[0]; xi <= db[1]; xi++)
            {
                for (int yi = db[2]; yi <= db[3]; yi++)
                {
                    for (int zi = db[4]; zi <= db[5]; zi++)
                    {
                        // get the hash index
                        unsigned long hi = getVirtualAddress(level, xi, yi, zi) % sz;
                        // search all nodes at this index
                        Node* node;
                        for (node = table[hi]; node; node = node->next)
                        {
                            // node points to an AABB that may intersect aabb
                            if (node->aabb == aabb)
                                continue;
                            if (node->aabb->level == level && node->x == xi && node->y == yi && node->z == zi)
                            {
                                // see if aabb and node->aabb have already been tested
                                // against each other
                                unsigned char mask;
                                if (aabb->index <= node->aabb->index)
                                {
                                    i = (aabb->index * tested_rowsize) + (node->aabb->index >> 3);
                                    mask = 1 << (node->aabb->index & 7);
                                }
                                else
                                {
                                    i = (node->aabb->index * tested_rowsize) + (aabb->index >> 3);
                                    mask = 1 << (aabb->index & 7);
                                }
                                dIASSERT(i >= 0 && i < (tested_rowsize * n));
                                if ((tested[i] & mask) == 0)
                                {
                                    collideAABBs(aabb->geom, node->aabb->geom, data, callback);
                                }
                                tested[i] |= mask;
                            }
                        }
                    }
                }
            }
            // get the discrete bounds for the next level up
            for (i = 0; i < 6; i++)
                db[i] >>= 1;
        }
    }

    // every AABB in the normal list must now be intersected against every
    // AABB in the big_boxes list. so let's hope there are not too many objects
    // in the big_boxes list.
    for (aabb = first_aabb; aabb; aabb = aabb->next)
    {
        for (dxAABB* aabb2 = big_boxes; aabb2; aabb2 = aabb2->next)
        {
            collideAABBs(aabb->geom, aabb2->geom, data, callback);
        }
    }

    // intersected all AABBs in the big_boxes list together
    for (aabb = big_boxes; aabb; aabb = aabb->next)
    {
        for (dxAABB* aabb2 = aabb->next; aabb2; aabb2 = aabb2->next)
        {
            collideAABBs(aabb->geom, aabb2->geom, data, callback);
        }
    }

    lock_count--;
}

void dxHashSpace::collide2(void* data, dxGeom* geom,
    dNearCallback* callback)
{
    dAASSERT(geom && callback);

    // this could take advantage of the hash structure to avoid
    // O(n2) complexity, but it does not yet.

    lock_count++;
    cleanGeoms();
    geom->recomputeAABB();

    // intersect bounding boxes
    for (dxGeom* g = first; g; g = g->next)
    {
        collideAABBs(g, geom, data, callback);
    }

    lock_count--;
}

//****************************************************************************
// space functions

dxSpace* dSimpleSpaceCreate(dxSpace* space)
{
    return new dxSimpleSpace(space);
}

dxSpace* dHashSpaceCreate(dxSpace* space)
{
    return new dxHashSpace(space);
}

void dHashSpaceSetLevels(dxSpace* space, int minlevel, int maxlevel)
{
    dAASSERT(space);
    dUASSERT(minlevel <= maxlevel, "must have minlevel <= maxlevel");
    dUASSERT(space->type == dHashSpaceClass, "argument must be a hash space");
    dxHashSpace* hspace = (dxHashSpace*)space;
    hspace->setLevels(minlevel, maxlevel);
}

void dHashSpaceGetLevels(dxSpace* space, int* minlevel, int* maxlevel)
{
    dAASSERT(space);
    dUASSERT(space->type == dHashSpaceClass, "argument must be a hash space");
    dxHashSpace* hspace = (dxHashSpace*)space;
    hspace->getLevels(minlevel, maxlevel);
}

void dSpaceDestroy(dxSpace* space)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    dGeomDestroy(space);
}

void dSpaceSetCleanup(dxSpace* space, int mode)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    space->setCleanup(mode);
}

int dSpaceGetCleanup(dxSpace* space)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    return space->getCleanup();
}

void dSpaceAdd(dxSpace* space, dxGeom* g)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    CHECK_NOT_LOCKED(space);
    space->add(g);
}

void dSpaceRemove(dxSpace* space, dxGeom* g)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    CHECK_NOT_LOCKED(space);
    space->remove(g);
}

int dSpaceQuery(dxSpace* space, dxGeom* g)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    return space->query(g);
}

void dSpaceClean(dxSpace* space)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");

    space->cleanGeoms();
}

int dSpaceGetNumGeoms(dxSpace* space)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    return space->getNumGeoms();
}

dGeomID dSpaceGetGeom(dxSpace* space, int i)
{
    dAASSERT(space);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    return space->getGeom(i);
}

void dSpaceCollide(dxSpace* space, void* data, dNearCallback* callback)
{
    dAASSERT(space && callback);
    dUASSERT(dGeomIsSpace(space), "argument not a space");
    space->collide(data, callback);
}

void dSpaceCollide2(dxGeom* g1, dxGeom* g2, void* data,
    dNearCallback* callback)
{
    dAASSERT(g1 && g2 && callback);
    dxSpace *s1, *s2;

    // see if either geom is a space
    if (IS_SPACE(g1))
        s1 = (dxSpace*)g1;
    else
        s1 = 0;
    if (IS_SPACE(g2))
        s2 = (dxSpace*)g2;
    else
        s2 = 0;

    // handle the four space/geom cases
    if (s1)
    {
        if (s2)
        {
            // g1 and g2 are spaces.
            if (s1 == s2)
            {
                // collide a space with itself --> interior collision
                s1->collide(data, callback);
            }
            else
            {
                // iterate through the space that has the fewest geoms, calling
                // collide2 in the other space for each one.
                if (s1->count < s2->count)
                {
                    for (dxGeom* g = s1->first; g; g = g->next)
                    {
                        s2->collide2(data, g, callback);
                    }
                }
                else
                {
                    for (dxGeom* g = s2->first; g; g = g->next)
                    {
                        s1->collide2(data, g, callback);
                    }
                }
            }
        }
        else
        {
            // g1 is a space, g2 is a geom
            s1->collide2(data, g2, callback);
        }
    }
    else
    {
        if (s2)
        {
            // g1 is a geom, g2 is a space
            s2->collide2(data, g1, callback);
        }
        else
        {
            // g1 and g2 are geoms, call the callback directly
            callback(data, g1, g2);
        }
    }
}

dxSimpleSpace::~dxSimpleSpace()
{
}

static void collideAABBs(dxGeom* g1, dxGeom* g2, void* data,
    dNearCallback* callback)
{
    dIASSERT((g1->gflags & GEOM_AABB_BAD) == 0);
    dIASSERT((g2->gflags & GEOM_AABB_BAD) == 0);

    NP_GSC(NPGC_AABB, 1);
    // no contacts if both geoms on the same body, and the body is not 0
    if (g1->body == g2->body && g1->body)
        return;

    // test if the category and collide bitfields match
    if (((g1->category_bits & g2->collide_bits)
            || (g2->category_bits & g1->collide_bits))
        == 0)
    {
        return;
    }

    // if the bounding boxes are disjoint then don't do anything
    dReal* bounds1 = g1->aabb;
    dReal* bounds2 = g2->aabb;
    if (bounds1[0] > bounds2[1] || bounds1[1] < bounds2[0]
        || bounds1[2] > bounds2[3] || bounds1[3] < bounds2[2]
        || bounds1[4] > bounds2[5] || bounds1[5] < bounds2[4])
    {
        return;
    }

    // check if either object is able to prove that it doesn't intersect the
    // AABB of the other
    if (g1->AABBTest(g2, bounds2) == 0)
        return;
    if (g2->AABBTest(g1, bounds1) == 0)
        return;

    // the objects might actually intersect - call the space callback function
    callback(data, g1, g2);
}
