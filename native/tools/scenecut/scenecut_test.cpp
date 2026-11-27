// Host test of np/scenecut.h (tests/test_scenecut.py builds and runs it, with ASan and UBSan).
#include "np/scenecut.h"
#include "np/floorbake.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++fails; } } while (0)

int main()
{
    CHECK(!NpSceneGlowHalfParse(nullptr));
    CHECK(!NpSceneGlowHalfParse("full"));
    CHECK(NpSceneGlowHalfParse("half"));
    CHECK(!NpSceneGlowDeadParse(nullptr));
    CHECK(!NpSceneGlowDeadParse("0"));
    CHECK(NpSceneGlowDeadParse("1"));
    CHECK(NpSceneGlowDeadUnused(5) && NpSceneGlowDeadUnused(2));   // the composite reads a level before the last: the last is dead
    CHECK(!NpSceneGlowDeadUnused(6));                                // the game's default index 6 reads the last level: nothing is dead
    CHECK(NpSceneGlowLastLevel(0) == 6);
    CHECK(NpSceneGlowLastLevel(1) == 5);
    CHECK(NpSceneCompositeLevel(0, 6) == 6);
    CHECK(NpSceneCompositeLevel(1, 6) == 5);
    CHECK(NpSceneCompositeLevel(1, 3) == 3);   // an ini that reads an earlier level is left alone

    CHECK(NpSceneShadowPlaneParse(nullptr) == 1);   // on by default (nxr3b)
    CHECK(NpSceneShadowPlaneParse("") == 1);
    CHECK(NpSceneShadowPlaneParse("0") == 0);
    CHECK(NpSceneShadowPlaneParse("1") == 1);
    CHECK(NpSceneShadowPlaneZ(0, 1, 0.1f) == 0.0f);    // off: the flattened mesh stays at z = 0, as 1.2
    CHECK(NpSceneShadowPlaneZ(1, 0, 0.1f) == 0.0f);    // no indexed packet: GX froze nothing either
    CHECK(NpSceneShadowPlaneZ(1, 1, 0.1f) == 0.1f);
    CHECK(NpSceneShadowEveryParse(nullptr) == 1);
    CHECK(NpSceneShadowEveryParse("0") == 1);
    CHECK(NpSceneShadowEveryParse("-3") == 1);
    CHECK(NpSceneShadowEveryParse("2") == 2);
    CHECK(NpSceneShadowEveryParse("99") == 4);
    CHECK(NpSceneShadowEveryParse("x") == 1);

    for (unsigned v = 0; v <= 512u; v += 16u) {         // MSC_FX_GOV: the mean weight is 1 at every level, nothing goes under 0, and slots fade out in the right order
        unsigned sum = 0;
        for (unsigned s = 0; s < 4u; ++s) sum += NpSceneGovWeight(s, v);
        CHECK(sum == 4u * 256u);
    }
    CHECK(NpSceneGovWeight(1, 0) == 256 && NpSceneGovWeight(0, 0) == 256);
    CHECK(NpSceneGovWeight(1, 256) == 0 && NpSceneGovWeight(0, 256) == 512 && NpSceneGovWeight(2, 256) == 512);
    CHECK(NpSceneGovWeight(3, 512) == 0 && NpSceneGovWeight(2, 512) == 0 && NpSceneGovWeight(0, 512) == 1024);
    CHECK(NpSceneGovWeight(0, 9999) == 1024);

    CHECK(NpSceneFarLodParse(nullptr) == 0.0f);
    CHECK(NpSceneFarLodParse("-1") == 0.0f);
    CHECK(NpSceneFarLodParse("8") == 8.0f);

    CHECK(NpSceneUseLowPoly(1, 0, 0.0f, 0.0f));         // open play: the game's choice, whatever the switch
    CHECK(!NpSceneUseLowPoly(0, 1, 0.0f, 1e9f));        // switch off: the game's choice
    CHECK(!NpSceneUseLowPoly(0, 0, 8.0f, 1e9f));        // the front end is never touched
    CHECK(!NpSceneUseLowPoly(0, 1, 8.0f, 64.0f));       // at the distance: still the full mesh
    CHECK(NpSceneUseLowPoly(0, 1, 8.0f, 64.5f));

    CHECK(NpSceneCrowdReplayParse(nullptr) == 1);      // the game's rate
    CHECK(NpSceneCrowdReplayParse("0") == 1);
    CHECK(NpSceneCrowdReplayParse("2") == 2);
    CHECK(NpSceneCrowdReplayParse("9") == 4);
    CHECK(NpSceneCrowdReplayParse("x") == 1);
    CHECK(NpSceneCrowdFloor(1, 1, 2) == 2);            // a replay with the crowd's list
    CHECK(NpSceneCrowdFloor(1, 0, 2) == 1);            // the front end's list has no cluster and draws random numbers in a capture
    CHECK(NpSceneCrowdFloor(0, 1, 2) == 1);            // the gameplay camera has MSC_CROWD_REFRESH
    CHECK(NpSceneCrowdFloor(1, 1, 1) == 1);

    CHECK(NpSceneClusterWanted(1, 0, 0));              // off: as the game, read or not
    CHECK(!NpSceneClusterWanted(0, 0, 5));             // the gameplay camera never
    CHECK(!NpSceneClusterWanted(1, 1, 0));
    CHECK(NpSceneClusterWanted(1, 1, 3));              // a stadium that reads it keeps it

    CHECK(NpSceneMenuLodParse(nullptr) == 0);
    CHECK(NpSceneMenuLodParse("-1") == 0);
    CHECK(NpSceneMenuLodParse("2") == 2);
    CHECK(NpSceneMenuLodParse("7") == 3);

    CHECK(!NpSceneHeavyHas(nullptr, 8));
    CHECK(!NpSceneHeavyHas("", 8));
    CHECK(NpSceneHeavyHas("8", 8));
    CHECK(!NpSceneHeavyHas("8", 0));
    CHECK(!NpSceneHeavyHas("18", 8));                  // a whole number, not a part of one
    CHECK(!NpSceneHeavyHas("80", 8));
    CHECK(NpSceneHeavyHas("3,8", 8));
    CHECK(NpSceneHeavyHas("3, 8 ,12", 12));
    CHECK(NpSceneHeavyHas("0", 0));
    CHECK(!NpSceneHeavyHas("8", -1));                  // the front end's world, the end of a stadium
    CHECK(!NpSceneHeavyHas("x,8", 8));                 // not a list
    CHECK(NpSceneHeavyHas("8,x", 8));
    CHECK(!NpSceneHeavyHas("99999999999999", 8));      // no overflow
    CHECK(NpSceneHeavyShadowEvery(1, 0) == 1);
    CHECK(NpSceneHeavyShadowEvery(1, 1) == 2);
    CHECK(NpSceneHeavyShadowEvery(4, 1) == 4);         // never less often than asked for
    CHECK(NpSceneHeavyShadowEvery(3, 0) == 3);

    // MSC_FX_HAZE: Mist_rain (additive, 12 units, 9 s, a 3 s fountain) and nothing like a burst or a short or small sprite
    CHECK(NpSceneHazeTemplate(1, 12.0f, 9.0f, 3.0f));
    CHECK(!NpSceneHazeTemplate(0, 12.0f, 9.0f, 3.0f));   // normal blend
    CHECK(!NpSceneHazeTemplate(1, 5.9f, 9.0f, 3.0f));    // small
    CHECK(!NpSceneHazeTemplate(1, 24.0f, 3.5f, 12.0f));  // effects.bun's largest fountain: short-lived
    CHECK(!NpSceneHazeTemplate(1, 8.0f, 100.0f, 0.0f));  // a burst (battledome)
    CHECK(NpSceneHazeParse(nullptr) == 2);
    CHECK(NpSceneHazeParse("") == 2);
    CHECK(NpSceneHazeParse("1") == 1);
    CHECK(NpSceneHazeParse("0") == 1);
    CHECK(NpSceneHazeParse("9") == 4);

    // The thinned alpha: the game's Mega Strike figures, then haze
    CHECK(NpSceneThinAlpha(10, 12, 20, 4, 1) == 40);     // under full: four times, the others none
    CHECK(NpSceneThinAlpha(10, 12, 20, 4, 0) == 0);
    CHECK(NpSceneThinAlpha(32, 12, 20, 4, 1) == 32);     // at full + span: the original, kept or not
    CHECK(NpSceneThinAlpha(32, 12, 20, 4, 0) == 32);
    CHECK(NpSceneThinAlpha(22, 12, 20, 4, 1) == 55);     // half way: a * 2.5 and a * 0.5, the sum over four stays 4a
    CHECK(NpSceneThinAlpha(22, 12, 20, 4, 0) == 11);
    CHECK(NpSceneThinAlpha(120, 255, 1, 2, 1) == 240);   // haze 1 in 2: the sum stays
    CHECK(NpSceneThinAlpha(120, 255, 1, 2, 0) == 0);
    CHECK(NpSceneThinAlpha(143, 255, 1, 2, 1) == 255);   // Mist_rain's peak: clamped, a tenth of it lost
    CHECK(NpSceneThinAlpha(255, 255, 1, 4, 1) == 255);
    // MSC_FX_HAZE_WIDE: the disc's four, and not the ball glow or a burst
    CHECK(NpSceneHazeTemplateWide(1, 12.0f, 9.0f, 3.0f));    // fx_mistrain
    CHECK(NpSceneHazeTemplateWide(1, 9.0f, 3.5f, 5.0f));     // fx_crystalCaustic
    CHECK(NpSceneHazeTemplateWide(1, 5.5f, 5.0f, 3.0f));     // fx_snow
    CHECK(NpSceneHazeTemplateWide(1, 7.5f, 3.0f, 20.0f));    // fx_stormWind
    CHECK(!NpSceneHazeTemplateWide(1, 24.0f, 3.5f, 12.0f));  // fx_ballglow_ground
    CHECK(!NpSceneHazeTemplateWide(1, 8.0f, 100.0f, 0.0f));  // a burst
    CHECK(!NpSceneHazeTemplateWide(0, 9.0f, 3.5f, 5.0f));    // normal blend

    // MSC_SHADOW_RES
    CHECK(NpSceneShadowResParse(nullptr) == 144);
    CHECK(NpSceneShadowResParse("96") == 96);
    CHECK(NpSceneShadowResParse("72") == 72);
    CHECK(NpSceneShadowResParse("71") == 144);
    CHECK(NpSceneShadowResParse("73") == 144);               // odd: no half
    CHECK(NpSceneShadowResParse("288") == 144);
    CHECK(NpSceneShadowResParse("x") == 144);

    // MSC_FLOOR_BAKE: the list
    CHECK(!NpFloorBakeHas(nullptr, 8));
    CHECK(!NpFloorBakeHas("", 8));
    CHECK(NpFloorBakeHas("all", 8));
    CHECK(!NpFloorBakeHas("all", -1));
    CHECK(NpFloorBakeHas("7,8", 8));
    CHECK(!NpFloorBakeHas("7,8", 12));

    // A block: every texel of a CMPR block must come out the colour it had plus the add, with the same opacity, whatever the end colours do
    for (unsigned c0 = 0; c0 < 65536u; c0 += 257u)
        for (unsigned c1 = 0; c1 < 65536u; c1 += 263u)
            for (unsigned add = 0; add <= 40u; add += 8u)
            {
                uint8_t b[8] = {(uint8_t)(c0 >> 8), (uint8_t)c0, (uint8_t)(c1 >> 8), (uint8_t)c1, 0xE4, 0x1B, 0x4E, 0xB1};
                uint8_t was[8];
                std::memcpy(was, b, 8);
                unsigned s0[3] = {0, 0, 0}, s1[3] = {0, 0, 0};
                const unsigned n0 = NpFbBlockSum(was, s0);
                NpFbShiftBlock(b, add, add, add);
                const unsigned n1 = NpFbBlockSum(b, s1);
                CHECK(n0 == n1);                                  // the hole stays where it is
                if (add == 0) CHECK(std::memcmp(was, b, 8) == 0);
                if (n0 == n1 && n0)
                    for (int i = 0; i < 3; ++i)
                    {
                        const int d = (int)(s1[i] / n1) - (int)(s0[i] / n0);
                        CHECK(d >= 0 && d <= (int)add + 9);       // never darker; at most a step of 5 bits over
                    }
            }

    // The mean of an overlay, the map of a packet, a bake of two levels
    {
        const uint8_t solid[8] = {0xF8, 0x00, 0x00, 0x00, 0, 0, 0, 0};   // four-colour block, all index 0: red
        uint8_t tile[32];
        for (int i = 0; i < 4; ++i) std::memcpy(tile + i * 8, solid, 8);
        unsigned m[3];
        CHECK(NpFbMeanOverlay(2, tile, 8, 8, m) && m[0] == 255 && m[1] == 0 && m[2] == 0);
        CHECK(!NpFbMeanOverlay(4, tile, 8, 8, m));
        const int16_t d[6] = {0, 0, 1024, 0, 512, 1024};
        const int16_t k[6] = {256, 512, 512, 512, 384, 1024};              // u/4 + 1/4, v/2 + 1/2
        NpFbMap map;
        CHECK(NpFbFitMap(d, k, 3, 4.0f / 1024.0f, &map));
        CHECK(map.su > 0.249f && map.su < 0.251f && map.ou > 0.249f && map.ou < 0.251f && map.sv > 0.499f && map.sv < 0.501f && map.ov > 0.499f && map.ov < 0.501f);
        const int16_t bent[6] = {256, 512, 512, 512, 900, 1024};
        CHECK(!NpFbFitMap(d, bent, 3, 4.0f / 1024.0f, &map));
        // 16 x 16 and 8 x 8 grey blocks; a 2 x 1 mask of black and white in blocks: only the right half of each level rises
        uint8_t tex[32 * 4 + 32];
        for (size_t i = 0; i < sizeof tex; i += 8)
        {
            const uint8_t grey[8] = {0x84, 0x10, 0x84, 0x10, 0, 0, 0, 0};
            std::memcpy(tex + i, grey, 8);
        }
        const uint8_t grid[6] = {0, 0, 0, 255, 255, 255};
        const NpFbMap id = {1.0f, 0.0f, 1.0f, 0.0f};
        const unsigned ov[3] = {255, 255, 255};
        NpFbBakeLevels(tex, 16, 16, 2, grid, 2, 1, &id, ov, 128);
        CHECK(NpFbBe16(tex + NpFbBlockOffset(16, 0, 0)) == 0x8410);
        CHECK(NpFbBe16(tex + NpFbBlockOffset(16, 1, 3)) == 0x8410);
        CHECK(NpFbBe16(tex + NpFbBlockOffset(16, 2, 0)) > 0x8410);
        CHECK(NpFbBe16(tex + NpFbBlockOffset(16, 3, 3)) > 0x8410);
        CHECK(NpFbBe16(tex + 32 * 4 + NpFbBlockOffset(8, 0, 0)) == 0x8410);
        CHECK(NpFbBe16(tex + 32 * 4 + NpFbBlockOffset(8, 1, 1)) > 0x8410);
        // wrapped coordinates stay in the grid
        const NpFbMap wrap = {-3.0f, -2.5f, 2.0f, 7.0f};
        NpFbBakeLevels(tex, 16, 16, 2, grid, 2, 1, &wrap, ov, 8);
    }
    // MSC_FLOOR_SHEEN
    CHECK(NpFloorSheenParse(nullptr) == 0);
    CHECK(NpFloorSheenParse("vertex") == NP_FB_SHEEN_VERTEX);
    CHECK(NpFloorSheenParse("lookup") == NP_FB_SHEEN_LOOKUP);
    CHECK(NpFloorSheenParse("off") == 0);
    CHECK(NpFloorSheenMode(NP_FB_SHEEN_VERTEX, 4.3f, 70.0f) == NP_FB_SHEEN_VERTEX);     // Pipeline Central
    CHECK(NpFloorSheenMode(NP_FB_SHEEN_VERTEX, 11.4f, 75.0f) == NP_FB_SHEEN_LOOKUP);    // Wastelands
    CHECK(NpFloorSheenMode(NP_FB_SHEEN_LOOKUP, 1.0f, 70.0f) == NP_FB_SHEEN_LOOKUP);
    {
        // a 2 x 2 reflection of black and white columns, RGBA8 is a tile; the table is 2 x 2 cells... of an 8 x 8 CMPR, red left half
        uint8_t tile[32];
        const uint8_t red[8] = {0xF8, 0x00, 0x00, 0x00, 0, 0, 0, 0}, black[8] = {0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0};
        std::memcpy(tile, red, 8); std::memcpy(tile + 8, black, 8); std::memcpy(tile + 16, red, 8); std::memcpy(tile + 24, black, 8);
        uint8_t grid[NP_FB_GRID * NP_FB_GRID * 3];
        uint32_t sums[NP_FB_GRID * NP_FB_GRID * 4];
        unsigned gw = 0, gh = 0;
        int mean[3];
        CHECK(NpFbReflectionGrid(2, tile, 8, 8, grid, sums, &gw, &gh, mean));
        CHECK(gw == 8 && gh == 8 && mean[0] == 127 && mean[1] == 0);
        CHECK(grid[0] == 255 && grid[4 * 3] == 0);
        CHECK(!NpFbReflectionGrid(4, tile, 8, 8, grid, sums, &gw, &gh, mean));
        // every coordinate, repeat included, stays in the table
        int r[3];
        for (float u = -0.99f; u < 1.0f; u += 0.0371f)
            for (float v = -0.99f; v < 1.0f; v += 0.0413f) NpFbGridSample(grid, gw, gh, u, v, r);
        NpFbGridSample(grid, gw, gh, 0.125f, 0.5f, r);
        CHECK(r[0] > 200);
        NpFbGridSample(grid, gw, gh, 0.75f, 0.5f, r);
        CHECK(r[0] < 30);

        float xy[4] = {0.0f, 0.0f, 3.0f, 0.0f};
        uint8_t weight[6] = {255, 255, 255, 0, 0, 0};
        uint32_t colours[2] = {0, 0};
        NpFloorSheen t;
        std::memset(&t, 0, sizeof t);
        t.mode = NP_FB_SHEEN_VERTEX; t.count = 2; t.scale = 8.0f; t.scroll = 1.0f; t.xy = xy; t.weight = weight; t.colours = colours; t.grid = grid; t.gw = gw; t.gh = gh;
        std::memcpy(t.mean, mean, sizeof mean);
        NpFbSheenColours(&t, 2.0f, 0.0f);                    // u = 0 / 8 + 0.5 - 2 / 8 = 0.25: in the red half, above the mean
        CHECK((colours[0] >> 24) > 128u);
        CHECK(colours[1] == 0x808080FFu);                    // no weight: nothing, whatever the reflection
        NpFbSheenColours(&t, -18.0f, 3.0f);                  // far away: wraps, and moves with the camera
        CHECK(colours[1] == 0x808080FFu);
        const uint32_t was = colours[0];
        NpFbSheenColours(&t, 2.0f, 0.0f);
        CHECK(colours[0] != was || was == 0);

        uint8_t lut[NP_FB_LUT_BYTES];
        NpFbLookupTexture(grid, gw, gh, mean, lut);
        CHECK(lut[0] == 0xFF && lut[1] > 170);                // the first texel: alpha, then red above the middle
        CHECK(lut[33] > 120 && lut[33] < 136);                // blue channel of the first texel stays at the middle

        uint8_t mg[12] = {0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 255, 255};   // 2 x 2 cells
        unsigned m[3];
        NpFbMaskAround(mg, 2, 2, 0.75f, 0.25f, 0, m);
        CHECK(m[0] == 255);
        NpFbMaskAround(mg, 2, 2, -0.25f, 0.25f, 0, m);        // repeats, negative too
        CHECK(m[0] == 255);
        NpFbMaskAround(mg, 2, 2, 0.75f, 0.25f, 1, m);
        CHECK(m[0] == 85);                                    // three columns: 0, 255, 0
    }
    // MSC_HELD_GLOW_HALF (held30): the policy of which held pictures skip the glow chain
    CHECK(NpSceneGlowRateParse(nullptr) == 0 && NpSceneGlowRateParse("0") == 0 && NpSceneGlowRateParse("1") == 1 && NpSceneGlowRateParse("2") == 2);
    CHECK(NpSceneGlowRateParse("-3") == 0 && NpSceneGlowRateParse("9") == 2 && NpSceneGlowRateParse("junk") == 0);
    for (int n = 0; n < 12; ++n) CHECK(!NpSceneGlowSkipPicture(0, n));                 // off: never
    for (int n = 0; n < kNpGlowWarm; ++n) { CHECK(!NpSceneGlowSkipPicture(1, n)); CHECK(!NpSceneGlowSkipPicture(2, n)); }   // the first pictures of a held scene are drawn in full
    {
        int skipped = 0;
        for (int n = kNpGlowWarm; n < kNpGlowWarm + 120; ++n) skipped += NpSceneGlowSkipPicture(1, n);
        CHECK(skipped == 60);                                                          // every other picture
        for (int n = kNpGlowWarm + 1; n < kNpGlowWarm + 120; ++n) CHECK(NpSceneGlowSkipPicture(1, n) != NpSceneGlowSkipPicture(1, n - 1));   // never two skips in a row
        skipped = 0;
        int run = 0, longest = 0;
        for (int n = kNpGlowWarm; n < kNpGlowWarm + 120; ++n) { const int k = NpSceneGlowSkipPicture(2, n); skipped += k; run = k ? run + 1 : 0; if (run > longest) longest = run; }
        CHECK(skipped == 80 && longest == 2);                                          // two in three, never three in a row
    }
    CHECK(!NpSceneGlowSkipPicture(1, -1));
    // scenes30 MSC_HELD_GOV: level 0 changes nothing; 1 the crowd floor (only where a floor applies), 2 also shadows, 3 also the glow; capped at 4; glow 0 (the game's chain) stays 0
    for (int b = 1; b <= 4; ++b) { CHECK(NpHeldGovCrowd(b, 0) == b); CHECK(NpHeldGovShadow(b, 0) == b); CHECK(NpHeldGovShadow(b, 1) == b); }
    CHECK(NpHeldGovCrowd(1, 4) == 1);                        // no floor (the front end's list, or MSC_CROWD_REPLAY=1): the governor adds none
    CHECK(NpHeldGovCrowd(2, 1) == 3 && NpHeldGovCrowd(4, 3) == 4);
    CHECK(NpHeldGovShadow(2, 2) == 3 && NpHeldGovShadow(4, 4) == 4);
    CHECK(NpHeldGovGlow(1, 2) == 1 && NpHeldGovGlow(1, 3) == 2 && NpHeldGovGlow(2, 4) == 2 && NpHeldGovGlow(0, 4) == 0);
    // held25 MSC_HELD_GOV_EFFECTS=1: the renderer reports 5 for the effects step; the crowd floor +2, shadows +2 (capped at 4), glow every 4th picture; levels 0-4 keep their meaning
    CHECK(NpHeldGovCrowd(2, 5) == 4 && NpHeldGovCrowd(1, 5) == 1 && NpHeldGovCrowd(2, 4) == 3);
    CHECK(NpHeldGovShadow(2, 5) == 4 && NpHeldGovShadow(3, 5) == 4 && NpHeldGovShadow(2, 4) == 3);
    CHECK(NpHeldGovGlow(1, 5) == 3 && NpHeldGovGlow(0, 5) == 0 && NpHeldGovGlow(1, 4) == 2);
    {
        int skipped = 0, run = 0, longest = 0;
        for (int n = kNpGlowWarm; n < kNpGlowWarm + 120; ++n) { const int k = NpSceneGlowSkipPicture(3, n); skipped += k; run = k ? run + 1 : 0; if (run > longest) longest = run; }
        CHECK(skipped == 90 && longest == 3);                                          // three in four, never four in a row
    }
    if (!fails) std::printf("scenecut: all passed\n");
    return fails != 0;
}
