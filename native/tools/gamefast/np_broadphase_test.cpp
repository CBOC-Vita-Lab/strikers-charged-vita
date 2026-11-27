// MSC_ODE_BROADPHASE: the real collision_space.cpp over generated spaces. Prints one hash of the callback sequence per seed; run_broadphase_test.py runs this with the switch
// at 0, 1 and 2 under qemu-arm (NEON path and C path) and requires identical output. Besides ordinary spaces the generator makes:
// positions at NaN, +-inf, denormals, -0 and exact touches, disabled geoms, sphere/box/plane mixes, and callbacks that move, disable, enable, add and remove geoms.
#include <ode/common.h>
#include <ode/collision.h>
#include <ode/collision_space.h>
#include <ode/objects.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include "../../src/ode/collision_kernel.h"
#include "../../src/ode/objects.h"
#include "../../src/ode/np_broadphase.h"

extern "C" void dGeomMoved(dxGeom*);

// the game's maths library, for the few calls the geoms make (plane normalisation)
float nlSqrt(float x, bool) { return sqrtf(x); }
float nlRecipSqrt(float x, bool) { return 1.0f / sqrtf(x); }

static uint32_t rngState;
static uint32_t Rng()
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}
static float Frand(float lo, float hi) { return lo + (hi - lo) * (float)(Rng() & 0xFFFFFF) / 16777216.0f; }
static float Bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

enum { MAXG = 700 };
static dGeomID geoms[MAXG];
static int ngeoms;
static dSpaceID space;
static uint32_t hash;
static int mutateOdds;
static int pairs;
static int bad;

static void Mix(uint32_t v) { hash = (hash ^ v) * 16777619u; }

static float Coord(int style)
{
    switch (style)
    {
    case 1:   // pitch: lots of near misses, lots of equal edges
        return (float)(int)Frand(-20, 20) * 0.5f;
    case 2:
    {
        // the awkward values
        static const uint32_t v[] = { 0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x80800000, 0x3F800000, 0xBF800000 };
        return Bits(v[Rng() % 10]);
    }
    case 3:
    {
        static const uint32_t v[] = { 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F7FFFFF, 0xFF7FFFFF, 0xFFC00001 };
        return (Rng() & 7) == 0 ? Bits(v[Rng() % 6]) : Frand(-5, 5);
    }
    default:
        return Frand(-40, 40);
    }
}

static dGeomID MakeGeom(int style)
{
    dGeomID g;
    uint32_t kind = Rng() % 8;
    if (kind < 5)
        g = dCreateSphere(0, style == 2 ? 0.0f : style == 1 ? (float)(1 + (int)(Rng() % 4)) * 0.5f : Frand(0.1f, 3.0f));
    else if (kind < 7 && style == 1)   // exact sizes on the half-unit grid: edges of different geoms land exactly on each other
        g = dCreateBox(0, (float)(1 + (int)(Rng() % 6)), (float)(1 + (int)(Rng() % 6)), (float)(1 + (int)(Rng() % 6)));
    else if (kind < 7)
        g = dCreateBox(0, Frand(0.1f, 6.0f), Frand(0.1f, 6.0f), Frand(0.1f, 6.0f));
    else
        g = dCreatePlane(0, 0, 0, 1, Frand(-2, 2));   // infinite AABB
    if (kind < 7)
        dGeomSetPosition(g, Coord(style), Coord(style), Coord(style));
    if ((Rng() & 7) == 0)
        dGeomDisable(g);
    // category / collide bits: mostly open, sometimes narrow (MSC_ODE_BPFULL tests them), and now and then a body shared by several geoms (the same-body early out)
    static const unsigned long cats[] = { ~0ul, ~0ul, ~0ul, 1ul, 2ul, 4ul, 0x40ul, 0xFFul, 0ul };
    static const unsigned long cols[] = { ~0ul, ~0ul, ~0ul, 2ul, 3ul, 0x5Ful, 0xAFul, 0x1F05Ful, 0ul };
    dGeomSetCategoryBits(g, Rng() & 3 ? ~0ul : cats[Rng() % 9]);
    dGeomSetCollideBits(g, Rng() & 3 ? ~0ul : cols[Rng() % 9]);
    if (kind < 7 && (Rng() & 3) == 0)
    {
        // a body shared by several geoms: a stand-in with an identity pose (the walk only reads the pointer; dBodyCreate would need the game's allocator)
        static dxBody fake[8];
        const int b = (int)(Rng() % 8);
        if (fake[b].R[0] == 0)
            fake[b].R[0] = fake[b].R[5] = fake[b].R[10] = 1;
        ((dxGeom*)g)->body = &fake[b];
    }
    return g;
}

static int IdOf(dGeomID g)
{
    for (int i = 0; i < ngeoms; i++)
        if (geoms[i] == g)
            return i;
    return -1;
}

static void Near(void*, dGeomID a, dGeomID b)
{
    Mix((uint32_t)IdOf(a));
    Mix((uint32_t)IdOf(b));
    pairs++;
    if (mutateOdds == 0 || Rng() % mutateOdds != 0)
        return;
    // ODE forbids these in a locked space only by an assertion that is off in the game's build: what matters is that both paths then walk the same way
    dGeomID t = geoms[Rng() % ngeoms];
    switch (Rng() % 6)
    {
    case 0:
        if (dGeomGetClass(t) != dPlaneClass)
            dGeomSetPosition(t, Frand(-40, 40), Frand(-40, 40), Frand(-5, 5));
        break;
    case 1:
        dGeomDisable(t);
        break;
    case 2:
        dGeomEnable(t);
        break;
    case 3:
        if (ngeoms < MAXG)
        {
            dGeomID g = MakeGeom(0);
            geoms[ngeoms++] = g;
            dSpaceAdd(space, g);
        }
        break;
    case 4:
        if (t != a && t != b && dSpaceQuery(space, t))   // a real game never frees mid-walk: taking a geom out (not one of the pair) is the hardest legal case
        {
            if (dGeomGetClass(t) != dPlaneClass)
                dSpaceRemove(space, t);
        }
        break;
    default:
        dGeomMoved(t);
        break;
    }
}

// The keys against float compares: every pair of a list of edge values and their neighbours, then random pairs that share an exponent often.
static int CheckKeys()
{
    static const uint32_t base[] = { 0x00000000, 0x00800000, 0x00800001, 0x3F7FFFFF, 0x3F800000, 0x3F800001, 0x7F7FFFFF, 0x7F800000 };
    uint32_t v[64];
    int n = 0;
    for (int i = 0; i < 8; i++)
    {
        v[n++] = base[i];
        v[n++] = base[i] | 0x80000000u;
    }
    int bad = 0;
    long tested = 0;
    for (long k = 0; k < 40000000; k++)
    {
        uint32_t a = k < n * n ? v[k / n] : Rng(), b = k < n * n ? v[k % n] : ((k & 1) ? Rng() : (a ^ (Rng() & 0x8000FFFF)));
        if (!NpBpKeyable(a) || !NpBpKeyable(b))
            continue;
        float fa = Bits(a), fb = Bits(b);
        int ka = NpBpKey(a), kb = NpBpKey(b);
        tested++;
        if ((fa > fb) != (ka > kb) || (fa < fb) != (ka < kb))
            bad++;
    }
    printf("ode keys: compared %ld wrong %d\n", tested, bad);
    return bad;
}

int main(int argc, char** argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 400;
    rngState = 12345;
    if (CheckKeys() != 0)
        return 1;
    for (int seed = 1; seed <= seeds; seed++)
    {
        rngState = (uint32_t)seed * 2654435761u + 1;
        hash = 2166136261u;
        pairs = 0;
        space = dSimpleSpaceCreate(0);
        ngeoms = (seed % 5 == 0) ? 3 + (int)(Rng() % 6) : 8 + (int)(Rng() % 200);
        if (seed % 37 == 0)
            ngeoms = 600;   // above NP_BP_MAX for a few of the moved calls once geoms are added
        int style = seed % 4;
        mutateOdds = (seed % 3 == 0) ? 0 : (seed % 3 == 1) ? 3 : 40;
        for (int i = 0; i < ngeoms; i++)
        {
            geoms[i] = MakeGeom(Rng() % 3 == 0 ? (int)(Rng() % 4) : style);
            dSpaceAdd(space, geoms[i]);
        }
        // several ticks, some geoms moved between them (the dirty-list order matters)
        for (int tick = 0; tick < 6; tick++)
        {
            dSpaceCollide(space, 0, Near);
            for (int k = 0; k < 10; k++)
            {
                dGeomID t = geoms[Rng() % ngeoms];
                if (dGeomGetClass(t) != dPlaneClass)
                    dGeomSetPosition(t, Coord(style), Coord(style), Frand(-3, 3));
            }
            Mix(0xFFFFFFFFu);
        }
        printf("%d %08x %d\n", seed, hash, pairs);
        for (int i = 0; i < ngeoms; i++)
        {
            if (dSpaceQuery(space, geoms[i]))
                dSpaceRemove(space, geoms[i]);
            dGeomDestroy(geoms[i]);
        }
        dSpaceDestroy(space);
    }
    (void)bad;
    return 0;
}
