/* np/stadiumcut.h: MSC_STADIUM_CUT, the per-stadium BACKGROUND cut table (wave 10, helper dumpgpu10). Default OFF. Render-only: every bit drops or thins something the player's
 * eye sees as scenery (heat shimmer, haze, small far decoration, crowd refresh rate), never a player, the ball, their shadows or the HUD, and nothing the simulation reads.
 *
 *   MSC_STADIUM_CUT=0|off      (default) nothing is cut.
 *   MSC_STADIUM_CUT=table      each stadium takes its row of kNpStadiumCutTable (below), the rows bg30b's MSC_BG30_CUTS can share (NpStadiumCutTableMask).
 *   MSC_STADIUM_CUT=<mask>     one bitmask (decimal or 0x..) for every stadium.
 *   MSC_STADIUM_CUT=<id>:<mask>[,<id>:<mask>...]   masks for the named stadiums only (ids as MSC_STADIUM_HEAVY), the others 0. "table" may be one of the items: "table,4:3".
 *   MSC_STADIUM_CUT_FAR=<px>   the size (screen pixels at 960 wide, projected diameter) under which NP_CUT_FARDECOR drops a stadium drawable. Default 24.
 *
 * Bits (the order is the ranked list of w8/dumpgpu10.md, estimates there):
 *   1 WARBLE    the heat-shimmer particles are not built: no pre-warble whole-picture copy, no offset pass, no indirect composite (Game/main.cpp RenderParticleSystem).
 *   2 HAZE      the large long-lived additive haze sprites are thinned as under the heavy profile (MSC_FX_HAZE), also the wide family (ParticleSystem.cpp).
 *   4 CROWDLOW  the crowd sprites refresh at the heavy profile's floor (GameRenderTask.cpp), the crowd stays drawn.
 *   8 FARDECOR  stadium drawables whose projected diameter is under MSC_STADIUM_CUT_FAR pixels are not drawn (World::Render). Goal frames, animated and shadow objects are kept.
 *  16 WORLDNPC  the world's decorative NPCs are not drawn (they still update), as the heavy profile does.
 *  64 DUMPFX    The Dump's rain: the streak meshes of template fx_dump_rain (0x8270565b, one mesh draw per drop, 32x32 additive streak texture, emitted by the stadium's WorldEffect rain emitters,
 *               ParticleSystem.cpp RenderAllParticles model branch): every second streak is not built, the others are drawn at twice the alpha (the light sum stays). Render only.
 * 256..4096 and 8192: see the enum below (the Dump's 4-texture material layers, far layers, bilinear, the gentler rain).
 * 128 TEST      test only: counts, does nothing (the "did it act" control).
 * Every bit that acts counts itself (NpStadiumCutCount); the report line "[stadiumcut]" prints the counters at each stadium end and every 900 frames.
 * The policy parse is here, without game headers, so a host test can compile it.
 */
#ifndef NP_STADIUMCUT_H
#define NP_STADIUMCUT_H

#include <stdlib.h>
#include <string.h>

enum
{
    NP_CUT_WARBLE = 1,
    NP_CUT_HAZE = 2,
    NP_CUT_CROWDLOW = 4,
    NP_CUT_FARDECOR = 8,
    NP_CUT_WORLDNPC = 16,
    NP_CUT_AMBIENTFX = 32,   /* ambient effect templates named by hash (MSC_STADIUM_CUT_FXHASH=0x..,0x.. or the table's list) are not drawn */
    NP_CUT_DUMPFX = 64,   /* The Dump's rain streaks (template fx_dump_rain, model particles) drawn one in two at double alpha */
    NP_CUT_TEST = 128,
    NP_CUT_MAT1 = 256,    /* The Dump's 4-texture stadium material (fragment program of renderer/src/vgx_dumpmat25.h): layer 1 (u_tex1) is its own mean colour, not fetched */
    NP_CUT_MAT2 = 512,    /* the same for layer 2 (u_tex2) */
    NP_CUT_MAT3 = 1024,   /* the same for layer 3 (u_tex3) */
    NP_CUT_MATFAR = 2048, /* layers 2 and 3 are not fetched where the fog depth is past MSC_DUMPMAT_FAR (default 0.9926, the far quarter of the pitch): their mean colour there */
    NP_CUT_RAIN3 = 8192,  /* The Dump's rain, the gentler form of 64: one streak in three is not built, the others are drawn at 1.5 x alpha */
    NP_CUT_RAINHULL = 16384, /* The Dump's rain drawn as small quads round the visible dashes of its texture (MSC_RAIN_HULL, np/vgx_rainhull.h): the same pixels, a quarter of the shaded area */
    NP_CUT_MATBILIN = 4096, /* layers 1..3 of that material are sampled bilinear (nearest mip), not trilinear / anisotropic */
    NP_CUT_ALL = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 | 4096 | 8192 | 16384
};

// The per-stadium default rows (stadium ids as STAD_*: 4 The Dump, 7 Lava Pit). Used by MSC_STADIUM_CUT=table. Filled from the census of w8/dumpgpu10.md.
static inline int NpStadiumCutTableMask(int stadium)
{
    switch (stadium)
    {
    case 4: return NP_CUT_RAINHULL | NP_CUT_MATFAR;   // The Dump (wave 25, w8/dumpgpu25.md): the rain as hull quads (same pixels, 2.0 -> 0.5 shaded screens) and the pitch's sheen layers not fetched in its far quarter (PSNR 42, no visible band). The 'steam' of dumpgpu10 is this rain (tex 32x32 additive). Not in the row: 1/2 and 1/3 rain thinning (64, 8192), layer constants (256..1024), bilinear (4096)
    case 7: return NP_CUT_WARBLE;    // Lava Pit: heat shimmer off (census: -0.77 Mpx of 2.76, 3 scenes)
    default: return 0;
    }
}

static inline int NpStadiumCutNumber(const char** pp)
{
    const char* p = *pp;
    int n = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
        for (;; ++p)
        {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : (*p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1));
            if (d < 0) break;
            if (n < 0x1000000) n = n * 16 + d;
        }
    }
    else
    {
        while (*p >= '0' && *p <= '9')
        {
            if (n < 100000) n = n * 10 + (*p - '0');
            ++p;
        }
    }
    *pp = p;
    return n;
}

// The mask the setting gives this stadium (-1 or an unnamed one: 0).
static inline int NpStadiumCutParse(const char* e, int stadium)
{
    if (!e || !*e || stadium < 0) return 0;
    if (strcmp(e, "0") == 0 || strcmp(e, "off") == 0 || strcmp(e, "none") == 0) return 0;
    if (strcmp(e, "table") == 0) return NpStadiumCutTableMask(stadium);
    int mask = 0;
    bool plain = *e >= '0' && *e <= '9';
    if (plain)
    {   // one bitmask, unless a ':' follows the first number
        const char* p = e;
        const int n = NpStadiumCutNumber(&p);
        if (*p == 0) return n & NP_CUT_ALL;
    }
    for (const char* p = e; *p;)
    {
        while (*p == ',' || *p == ' ') ++p;
        if (strncmp(p, "table", 5) == 0)
        {
            mask |= NpStadiumCutTableMask(stadium);
            p += 5;
            continue;
        }
        if (*p < '0' || *p > '9') break;
        const int id = NpStadiumCutNumber(&p);
        if (*p != ':') break;
        ++p;
        const int m = NpStadiumCutNumber(&p);
        if (id == stadium) mask |= m & NP_CUT_ALL;
    }
    return mask;
}

// The size floor of FARDECOR in pixels of a 960-wide picture.
static inline float NpStadiumCutFarParse(const char* e)
{
    float v = e && *e ? (float)atof(e) : 24.0f;
    return v < 1.0f ? 1.0f : (v > 200.0f ? 200.0f : v);
}

// Is a drawable of this projected diameter (in pixels of a 960-wide picture) too small to draw under FARDECOR?
static inline int NpStadiumCutFarSmall(float diameterPx, float floorPx)
{
    return diameterPx < floorPx;
}

#ifdef NP_NATIVE
extern "C" int NpStadiumCutMask(void);               // the active mask of the stadium being played (0 off)
extern "C" void NpStadiumCutSet(int stadium);        // from the stadium load (-1 at its end): sets the mask, prints the [stadiumcut] line
extern "C" void NpStadiumCutCount(int bit, int n);   // the action counter of a bit
extern "C" float NpStadiumCutFarPx(void);
extern "C" void NpStadiumCutFrame(void);
extern "C" int NpStadiumCutFxHashed(unsigned hash);   // bit 32: is this template one the stadium does not draw (counts)
extern "C" unsigned long long NpStadiumCutFrameNo(void);
extern "C" int NpStadiumCutNewMax(unsigned hash, int n);   // log helper: true when n is a new maximum (x2) for the hash
extern "C" int NpStadiumCutLogObjects(void);   // MSC_STADIUM_CUT_LOG=2
extern "C" void NpStadiumCutLogObject(unsigned hash, int type, int animated, int flags, float px);
extern "C" void NpStadiumCutLogTemplate(unsigned hash, int blend, unsigned tex, float size, float life, float fountain, int layer, int nParticles);   // MSC_STADIUM_CUT_LOG=1: one line per template             // once per rendered frame: the periodic report
static inline int NpStadiumCutHas(int bit) { return (NpStadiumCutMask() & bit) != 0; }
// CROWDLOW: the floor of the crowd sprite refresh is at least 3 (every third frame), the crowd stays drawn. Counts each frame the floor was raised.
static inline int NpStadiumCutCrowdFloor(int floor)
{
    if (NpStadiumCutHas(NP_CUT_CROWDLOW) && floor < 3 && floor >= 1)
    {
        NpStadiumCutCount(NP_CUT_CROWDLOW, 1);
        return 3;
    }
    return floor;
}
#endif

#endif // NP_STADIUMCUT_H
