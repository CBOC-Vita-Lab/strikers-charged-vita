/* Broad-phase prefilter of dxSimpleSpace::collide (MSC_ODE_BROADPHASE; MSC_ODE_BPFULL adds the Z extent, the category / collide bits and the same-body test, all of collideAABBs' side-effect-free early outs). The pair walk of a simple space is the hottest loop of the game thread by the PGO list:
 * about 7,000 pairs a physics tick, of which about 75 reach the callback, each pair chasing two dxGeom pointers and doing up to six VFP compares.
 * The X and Y extents of the enabled geoms are copied into flat arrays and turned into integers that order exactly as the floats do, so the skip decision is
 * integer compares: no float arithmetic, nothing NEON's flush-to-zero or the FPSCR can change. A pair skipped here is one collideAABBs would have rejected at
 * its bounds test, with no side effect; every other pair still goes through the real collideAABBs. Test: native/tools/gamefast/np_broadphase_test.cpp. */
#ifndef NP_BROADPHASE_H
#define NP_BROADPHASE_H

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(NP_BP_NO_NEON)
#include <arm_neon.h>
#endif

#define NP_BP_PAD 3 /* readable entries after the last geom: a scan reads four at a time */

/* Finite or infinite, normal or a zero of either sign: the integer key orders these exactly as a float compare does under every FPSCR. NaN is left out (every compare
 * with it is false) and so are denormals (compared as zero when FPSCR.FZ is set): a space holding one takes the plain loop for that call. */
static inline int NpBpKeyable(unsigned bits)
{
    unsigned a = bits & 0x7FFFFFFFu;
    return a == 0 || (a >= 0x00800000u && a <= 0x7F800000u);
}

/* Sign-magnitude to two's complement. -0 and +0 both give 0, as they compare equal. */
static inline int NpBpKey(unsigned bits)
{
    int m = (int)(bits & 0x7FFFFFFFu);
    int s = (int)bits >> 31;
    return (m ^ s) - s;
}

/* Fill entries n .. n+NP_BP_PAD-1 so that no geom can pass against them. */
static inline void NpBpPad(int* lox, int* hix, int* loy, int* hiy, int n)
{
    for (int k = 0; k < NP_BP_PAD; k++)
    {
        lox[n + k] = 0x7FFFFFFF;
        hix[n + k] = (int)0x80000000u;
        loy[n + k] = 0x7FFFFFFF;
        hiy[n + k] = (int)0x80000000u;
    }
}

/* The rest of collideAABBs' early outs, in flat arrays beside the four keys (MSC_ODE_BPFULL): Z extent keys, category and collide bits, body identity (0 = none). */
struct NpBpExtra
{
    int* loz;
    int* hiz;
    unsigned* cat;
    unsigned* col;
    unsigned* body;
};

static inline void NpBpPadExtra(const NpBpExtra& x, int n)
{
    for (int k = 0; k < NP_BP_PAD; k++)
    {
        x.loz[n + k] = 0x7FFFFFFF;
        x.hiz[n + k] = (int)0x80000000u;
        x.cat[n + k] = 0;
        x.col[n + k] = 0;
        x.body[n + k] = 0;
    }
}

/* collideAABBs: bounds1[0] > bounds2[1] || bounds1[1] < bounds2[0] || bounds1[2] > bounds2[3] || bounds1[3] < bounds2[2] */
static inline int NpBpReject(int ilox, int ihix, int iloy, int ihiy, int jlox, int jhix, int jloy, int jhiy)
{
    return ilox > jhix || ihix < jlox || iloy > jhiy || ihiy < jloy;
}

/* collideAABBs up to (not including) the geoms' own AABBTest: same body, category / collide bits, the six bounds. */
static inline int NpBpRejectFull(const int* lox, const int* hix, const int* loy, const int* hiy, const NpBpExtra& x, int i, int j)
{
    if (x.body[i] == x.body[j] && x.body[i])
        return 1;
    if (((x.cat[i] & x.col[j]) || (x.cat[j] & x.col[i])) == 0)
        return 1;
    return NpBpReject(lox[i], hix[i], loy[i], hiy[i], lox[j], hix[j], loy[j], hiy[j]) || x.loz[i] > x.hiz[j] || x.hiz[i] < x.loz[j];
}

/* The first j >= from that geom i is not rejected against, n if there is none. Ascending, so the survivors come in the order of the plain loop. */
static inline int NpBpNext(const int* lox, const int* hix, const int* loy, const int* hiy, int i, int from, int n)
{
    int j = from;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(NP_BP_NO_NEON)
    const int32x4_t ilox = vdupq_n_s32(lox[i]);
    const int32x4_t ihix = vdupq_n_s32(hix[i]);
    const int32x4_t iloy = vdupq_n_s32(loy[i]);
    const int32x4_t ihiy = vdupq_n_s32(hiy[i]);
    for (; j < n; j += 4)
    {
        uint32x4_t r = vcgtq_s32(ilox, vld1q_s32(hix + j));
        r = vorrq_u32(r, vcltq_s32(ihix, vld1q_s32(lox + j)));
        r = vorrq_u32(r, vcgtq_s32(iloy, vld1q_s32(hiy + j)));
        r = vorrq_u32(r, vcltq_s32(ihiy, vld1q_s32(loy + j)));
        /* lanes are all-ones or all-zeros, so each narrows to 16 bits of the same; a padding entry always rejects, so a survivor is below n */
        uint64_t m = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(r)), 0);
        if (m != ~(uint64_t)0)
        {
            int k = 0;
            while ((m & 0xFFFFu) != 0)
            {
                m >>= 16;
                k++;
            }
            return j + k;
        }
    }
    return n;
#else
    for (; j < n; j++)
    {
        if (!NpBpReject(lox[i], hix[i], loy[i], hiy[i], lox[j], hix[j], loy[j], hiy[j]))
            return j;
    }
    return n;
#endif
}

/* The same with the full test. Ascending, so the survivors come in the order of the plain loop; every skipped pair is one collideAABBs returns from before it calls anything. */
static inline int NpBpNextFull(const int* lox, const int* hix, const int* loy, const int* hiy, const NpBpExtra& x, int i, int from, int n)
{
    int j = from;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(NP_BP_NO_NEON)
    const int32x4_t ilox = vdupq_n_s32(lox[i]);
    const int32x4_t ihix = vdupq_n_s32(hix[i]);
    const int32x4_t iloy = vdupq_n_s32(loy[i]);
    const int32x4_t ihiy = vdupq_n_s32(hiy[i]);
    const int32x4_t iloz = vdupq_n_s32(x.loz[i]);
    const int32x4_t ihiz = vdupq_n_s32(x.hiz[i]);
    const uint32x4_t icat = vdupq_n_u32(x.cat[i]);
    const uint32x4_t icol = vdupq_n_u32(x.col[i]);
    const uint32x4_t ibody = vdupq_n_u32(x.body[i]);
    const uint32x4_t ibodyNonZero = vdupq_n_u32(x.body[i] ? 0xFFFFFFFFu : 0u);
    for (; j < n; j += 4)
    {
        uint32x4_t r = vcgtq_s32(ilox, vld1q_s32(hix + j));
        r = vorrq_u32(r, vcltq_s32(ihix, vld1q_s32(lox + j)));
        r = vorrq_u32(r, vcgtq_s32(iloy, vld1q_s32(hiy + j)));
        r = vorrq_u32(r, vcltq_s32(ihiy, vld1q_s32(loy + j)));
        r = vorrq_u32(r, vcgtq_s32(iloz, vld1q_s32(x.hiz + j)));
        r = vorrq_u32(r, vcltq_s32(ihiz, vld1q_s32(x.loz + j)));
        /* category: passes when (cat_i & col_j) or (cat_j & col_i) is not zero; vtst gives all-ones for "not zero" */
        const uint32x4_t pass = vorrq_u32(vtstq_u32(icat, vld1q_u32(x.col + j)), vtstq_u32(vld1q_u32(x.cat + j), icol));
        r = vorrq_u32(r, vmvnq_u32(pass));
        /* same body: equal to a non-zero body */
        r = vorrq_u32(r, vandq_u32(vceqq_u32(ibody, vld1q_u32(x.body + j)), ibodyNonZero));
        uint64_t m = vget_lane_u64(vreinterpret_u64_u16(vmovn_u32(r)), 0);
        if (m != ~(uint64_t)0)
        {
            int k = 0;
            while ((m & 0xFFFFu) != 0)
            {
                m >>= 16;
                k++;
            }
            return j + k;
        }
    }
    return n;
#else
    for (; j < n; j++)
    {
        if (!NpBpRejectFull(lox, hix, loy, hiy, x, i, j))
            return j;
    }
    return n;
#endif
}

/* MSC_ODE_BPFULL=2: runs. The geoms of one character (the 11 bone geoms, all on one body) are walked in a row, so the list is cut into segments: a run is
 * three or more consecutive enabled geoms on the same non-zero body, anything between runs is one plain segment. A run keeps the union of its members'
 * keys (min of the lows, max of the highs, the OR of the category and of the collide bits). A geom i that is on the run's body, or whose boxes the union
 * box misses on any axis, or whose category / collide bits can pass against no member (both ANDs with the ORs are zero), is rejected by collideAABBs
 * against every member of the run, so the whole run is stepped over. Everything else walks the members with the usual test. All integer compares on the
 * same keys as the member test: a member is skipped only when its own test would reject it, the survivors come in the order of the plain loop. */
struct NpBpSegs
{
    int count;
    int hasRun;
    int* end;            /* [seg] one past its last geom */
    unsigned char* run;  /* [seg] 1 = a run */
    int* segOf;          /* [geom] its segment */
    int* ulox; int* uhix; int* uloy; int* uhiy; int* uloz; int* uhiz;   /* [seg] union keys (runs only) */
    unsigned* ucat; unsigned* ucol;
    unsigned long long skipped;   /* geoms stepped over by a run skip (counter) */
};

#define NP_BP_MINRUN 3

/* The arrays are sized n + 1 by the caller. mutate: proof mutant, every second run's union box is far too small (the run is skipped for every geom). */
static inline void NpBpBuildSegs(NpBpSegs& S, const int* lox, const int* hix, const int* loy, const int* hiy, const NpBpExtra& x, int n, int mutate)
{
    int c = 0, plainStart = -1, runs = 0;
    int i = 0;
    S.hasRun = 0;
    while (i < n)
    {
        int j = i + 1;
        if (x.body[i])
        {
            while (j < n && x.body[j] == x.body[i])
                j++;
        }
        if (x.body[i] && j - i >= NP_BP_MINRUN)
        {
            if (plainStart >= 0)
            {
                S.end[c] = i;
                S.run[c] = 0;
                for (int k = plainStart; k < i; k++)
                    S.segOf[k] = c;
                c++;
                plainStart = -1;
            }
            int a = lox[i], b = hix[i], d = loy[i], e = hiy[i], f = x.loz[i], g = x.hiz[i];
            unsigned oc = 0, ol = 0;
            for (int k = i; k < j; k++)
            {
                if (lox[k] < a) a = lox[k];
                if (hix[k] > b) b = hix[k];
                if (loy[k] < d) d = loy[k];
                if (hiy[k] > e) e = hiy[k];
                if (x.loz[k] < f) f = x.loz[k];
                if (x.hiz[k] > g) g = x.hiz[k];
                oc |= x.cat[k];
                ol |= x.col[k];
                S.segOf[k] = c;
            }
            if (mutate && (runs & 1) == 0)
                b = (int)0x80000000u;
            runs++;
            S.end[c] = j;
            S.run[c] = 1;
            S.ulox[c] = a; S.uhix[c] = b; S.uloy[c] = d; S.uhiy[c] = e; S.uloz[c] = f; S.uhiz[c] = g;
            S.ucat[c] = oc; S.ucol[c] = ol;
            c++;
            S.hasRun = 1;
            i = j;
        }
        else
        {
            if (plainStart < 0)
                plainStart = i;
            i++;
        }
    }
    if (plainStart >= 0)
    {
        S.end[c] = n;
        S.run[c] = 0;
        for (int k = plainStart; k < n; k++)
            S.segOf[k] = c;
        c++;
    }
    S.count = c;
}

/* NpBpNextFull over the segments: the first j >= from that geom i is not rejected against, n if there is none. */
static inline int NpBpNextSeg(const int* lox, const int* hix, const int* loy, const int* hiy, const NpBpExtra& x, NpBpSegs& S, int i, int from, int n)
{
    int j = from;
    while (j < n)
    {
        const int s = S.segOf[j];
        const int e = S.end[s];
        if (S.run[s])
        {
            if ((x.body[i] == x.body[j] && x.body[i])
                || lox[i] > S.uhix[s] || hix[i] < S.ulox[s] || loy[i] > S.uhiy[s] || hiy[i] < S.uloy[s] || x.loz[i] > S.uhiz[s] || x.hiz[i] < S.uloz[s]
                || ((x.cat[i] & S.ucol[s]) == 0 && (S.ucat[s] & x.col[i]) == 0))
            {
                S.skipped += (unsigned)(e - j);
                j = e;
                continue;
            }
        }
        const int r = NpBpNextFull(lox, hix, loy, hiy, x, i, j, e);
        if (r < e)
            return r;
        j = e;
    }
    return n;
}

/* MSC_SIM26_BP (wave 12, sim26): the rows of a run. NpBpNextSeg tests every geom i of a run (a character's bones) against every later segment on its own;
 * the 11 rows of one character repeat nearly the same walk. Here the run's union (keys, ORed bits, its body) is tested ONCE against every geom after the run
 * (and against the union of every later run first), giving the run's candidate list; the rows of the run then test only those candidates with the member
 * test NpBpRejectFull. A geom j left out of the list is rejected by every member i of the run: j on the run's body (same non-zero body), or the union box
 * misses j's box on an axis (the union holds every member's box, so the member misses it too), or both ANDs of the ORed bits with j's bits are zero
 * (each member's bits are a subset). The geoms inside the run after i are on i's body. So each row's survivors are the same j in the same ascending order
 * as NpBpNextSeg / the plain loop; nothing but integer compares on the same keys. */
static inline int NpBpRejectUnionGeom(const int* lox, const int* hix, const int* loy, const int* hiy, const NpBpExtra& x, const NpBpSegs& S, int s, unsigned sbody, int j)
{
    return (x.body[j] == sbody) || S.ulox[s] > hix[j] || S.uhix[s] < lox[j] || S.uloy[s] > hiy[j] || S.uhiy[s] < loy[j] || S.uloz[s] > x.hiz[j] || S.uhiz[s] < x.loz[j]
           || ((S.ucat[s] & x.col[j]) == 0 && (x.cat[j] & S.ucol[s]) == 0);
}

static inline int NpBpRejectUnionUnion(const NpBpSegs& S, int s, int t)
{
    return S.ulox[s] > S.uhix[t] || S.uhix[s] < S.ulox[t] || S.uloy[s] > S.uhiy[t] || S.uhiy[s] < S.uloy[t] || S.uloz[s] > S.uhiz[t] || S.uhiz[s] < S.uloz[t]
           || ((S.ucat[s] & S.ucol[t]) == 0 && (S.ucat[t] & S.ucol[s]) == 0);
}

/* The candidates of run segment s (its geoms end at runEnd; sbody = its body) into cand[], ascending; returns their count. mutate (proof mutants of
 * MSC_SIM26_BP_BREAK): 1 drops the last candidate, 2 drops them all. Counts the union-union steps over a later run in *runSkips. */
static inline int NpBpRunCands(const int* lox, const int* hix, const int* loy, const int* hiy, const NpBpExtra& x, const NpBpSegs& S, int s, unsigned sbody, int runEnd, int n, int* cand,
    int mutate, unsigned long long* runSkips)
{
    int m = 0;
    int j = runEnd;
    while (j < n)
    {
        const int t = S.segOf[j];
        const int e = S.end[t];
        if (S.run[t] && NpBpRejectUnionUnion(S, s, t))
        {
            *runSkips += 1;
            j = e;
            continue;
        }
        for (; j < e; j++)
        {
            if (!NpBpRejectUnionGeom(lox, hix, loy, hiy, x, S, s, sbody, j))
                cand[m++] = j;
        }
    }
    if (mutate == 1 && m > 0)
        m--;
    else if (mutate == 2)
        m = 0;
    return m;
}

#endif
