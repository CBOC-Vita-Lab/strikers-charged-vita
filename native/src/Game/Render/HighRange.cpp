#include "Game/Render/HighRange.h"

#include "Game/SharedStaticStorage.h"
#include "NL/nlDebugViews.h"

#include "Game/Render/RLView.h"
#include "Game/Render/RLViewLayers.h"

#include "NL/gl/glDraw2.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glState.h"
#include "Game/GL/GLFourTextureAddMeshWriter.h"
#include "NL/gl/glView.h"
#include "NL/nlColour.h"
#include "NL/nlMemory.h"
#include "NL/nlPrint.h"
#include "NL/nlString.h"
#include "np/scenecut.h"

#ifdef NP_NATIVE
#include <stdio.h>
// MSC_FX_GLOW=half (np/scenecut.h): the chain stops one level early
// MSC_FX_GLOW_DEAD=1 (np/scenecut.h): the same stop one level early, but only when the composite does not read the last level (exact)
static inline int GlowHalf()
{
    if (NpSceneGlowHalf()) return 1;
    if (!NpSceneGlowDead() || !NpSceneGlowDeadUnused(*GetHighRangeTweaks()->miHighRangeIndex.m_pValue)) return 0;
    return 1;
}
// MSC_FX_GLOW_COARSE (np/scenecut.h): the game's coarse route, four passes in place of six
static inline bool GlowFine(HighRangeTweaks* t) { return *t->mbFineHighRange.m_pValue && !NpSceneGlowCoarse(); }
#else
static inline int GlowHalf() { return 0; }
static inline bool GlowFine(HighRangeTweaks* t) { return *t->mbFineHighRange.m_pValue; }
#endif

HighRange gHighRange;

int sHighRangeDebugTextureIndex = -1;
bool sHighRangeEnabled = true;
int sHighRangeSampleOffset0 = 1;
int sHighRangeSampleOffset1 = 2;
int sHighRangeSampleOffset2 = 3;

int sHighRangeSampleOffset3;
int sHighRangeSampleOffset4;
int sHighRangeSampleOffset5;
int sHighRangeSampleOffset6;

extern nlVector2 sHighRangeVertices[4];

void BindHighRangeTweaks(
    HighRangeTweaks* values, const char* group)
{
    bool registered = values->miHighRangeIndex.Bind(
        "miHighRangeIndex", 0.0f, group, true, 6.0f, 1.0f);
    if (!registered)
    {
        *values->miHighRangeIndex.m_pValue = 6;
    }

    values->mfHighRangeMult.BindWithDefault(
        "mfHighRangeMult", 2.5f, group, true, 0.5f, 16.0f, 0.125f);
    values->mfHighRangeOffset.BindWithDefault(
        "mfHighRangeOffset", 3.5f, group, true, 0.0f, 8.0f, 0.125f);

    registered = values->miHighRangeGray.Bind(
        "miHighRangeGray", 0.0f, group, true, 255.0f, 1.0f);
    if (!registered)
    {
        *values->miHighRangeGray.m_pValue = 80;
    }

    values->mbFineHighRange.BindWithDefault(
        "mbFineHighRange", true, group, true, 0.0f, 0.0f, 0.0f);

    registered = values->miRed.Bind(
        "miRed", 0.0f, group, true, 255.0f, 1.0f);
    if (!registered)
    {
        *values->miRed.m_pValue = 255;
    }

    registered = values->miGreen.Bind(
        "miGreen", 0.0f, group, true, 255.0f, 1.0f);
    if (!registered)
    {
        *values->miGreen.m_pValue = 255;
    }

    registered = values->miBlue.Bind(
        "miBlue", 0.0f, group, true, 255.0f, 1.0f);
    if (!registered)
    {
        *values->miBlue.m_pValue = 255;
    }

    registered = values->miAlpha.Bind(
        "miAlpha", 0.0f, group, true, 255.0f, 1.0f);
    if (!registered)
    {
        *values->miAlpha.m_pValue = 108;
    }
}

bool IsHighRangeEnabled(const HighRange*)
{
    return sHighRangeEnabled;
}

void InitializeHighRange(HighRange* state)
{
    GLTargetInfo info;
    u32 widths[7] = { 320, 160, 80, 40, 80, 160, 320 };
    u32 heights[7] = { 224, 112, 56, 28, 56, 112, 224 };
    int i;

    for (i = 0; i < 7; ++i)
    {
        state->mViewports[i].x = 0;
        state->mViewports[i].y = 0;
        state->mViewports[i].width = widths[i] * 2;
        state->mViewports[i].height = heights[i] * 2;
    }

    char targetName0[0x20];
    const char* name0;
    nlSNPrintf(state->mNames[0], sizeof(state->mNames[0]), "%s",
        name0 = "hr320d");
    nlSNPrintf(targetName0, sizeof(targetName0), "target/%s", name0);
    state->mTextures[0] = glGetTexture(targetName0);

    char targetName1[0x20];
    const char* name1 = "hr160d";
    nlSNPrintf(state->mNames[1], sizeof(state->mNames[1]), "%s", name1);
    nlSNPrintf(targetName1, sizeof(targetName1), "target/%s", name1);
    state->mTextures[1] = glGetTexture(targetName1);

    char targetName2[0x20];
    const char* name2 = "hr80d";
    nlSNPrintf(state->mNames[2], sizeof(state->mNames[2]), "%s", name2);
    nlSNPrintf(targetName2, sizeof(targetName2), "target/%s", name2);
    state->mTextures[2] = glGetTexture(targetName2);

    char targetName3[0x20];
    const char* name3 = "hr40d";
    nlSNPrintf(state->mNames[3], sizeof(state->mNames[3]), "%s", name3);
    nlSNPrintf(targetName3, sizeof(targetName3), "target/%s", name3);
    state->mTextures[3] = glGetTexture(targetName3);

    char targetName4[0x20];
    const char* name4 = "hr80u";
    nlSNPrintf(state->mNames[4], sizeof(state->mNames[4]), "%s", name4);
    nlSNPrintf(targetName4, sizeof(targetName4), "target/%s", name4);
    state->mTextures[4] = glGetTexture(targetName4);

    char targetName5[0x20];
    const char* name5 = "hr160u";
    nlSNPrintf(state->mNames[5], sizeof(state->mNames[5]), "%s", name5);
    nlSNPrintf(targetName5, sizeof(targetName5), "target/%s", name5);
    state->mTextures[5] = glGetTexture(targetName5);

    char targetName6[0x20];
    const char* name6 = "hr320u";
    nlSNPrintf(state->mNames[6], sizeof(state->mNames[6]), "%s", name6);
    nlSNPrintf(targetName6, sizeof(targetName6), "target/%s", name6);
    state->mTextures[6] = glGetTexture(targetName6);

    for (i = 0; i < 7; ++i)
    {
        nlZeroMemory(&info, sizeof(info));
        info.width = widths[i];
        info.height = heights[i];
        info.format = GLTargetFormat_A8;
        info.clearFlags = 0;
        info.clearDepthSetting = 0;
        state->mRenderPairs[i]
            = glCreateTarget(state->mNames[i], &info);
    }

    for (i = 0; i < 7; ++i)
    {
        GLView* view = new (8, false) GLView(
            GetOrthoCamera(), state->mRenderPairs[i], GLViewSort_Texture);
        view->m_Name = state->mNames[i];
        view->m_Viewport.x = state->mViewports[i].x;
        view->m_Viewport.y = state->mViewports[i].y;
        view->m_Viewport.width = state->mViewports[i].width;
        view->m_Viewport.height = state->mViewports[i].height;
        view->m_ClearColour = false;
        view->m_ClearDepth = false;
        view->m_Target = 10;
        state->mViews[i] = view;
    }

    for (i = 0; i < 7; ++i)
    {
        state->mViews[i]->m_Enabled = true;
        GetLayerView(eCLV_HighRangeChain)->AddChild(state->mViews[i]);
    }
}

void SetHighRangeTargetsEnabled(
    HighRange* state, int enabled)
{
    bool fineHighRange = GlowFine(GetHighRangeTweaks());
    for (int i = 0; i < 7; ++i)
    {
        GLView* view = state->mViews[i];
        int target = 0;
        if (enabled)
        {
            target = 10;
        }
        view->m_Target = target;
        if (!fineHighRange && (unsigned int)(i - 3) <= 1)
        {
            state->mViews[i]->m_Target = 0;
        }
    }
    if (GlowHalf())
    {
        state->mViews[6]->m_Target = 0;
    }
}

void CompositeHighRange(HighRange*)
{
    glPoly2 poly;
    int index = NpSceneCompositeLevel(GlowHalf(), *GetHighRangeTweaks()->miHighRangeIndex.m_pValue);

    glSetDefaultState(false);
    glSetCurrentTexture(gHighRange.mTextures[index], GLTT_Diffuse);
    glSetRasterState(GLS_AlphaBlend, 3);
    glSetCurrentRasterState(glHandleizeRasterState());
    glSetTextureState(GLTS_DiffuseWrap, 3);
    glSetCurrentTextureState(glHandleizeTextureState());

    HighRangeTweaks* values = GetHighRangeTweaks();
    nlColour colour;
    nlColourSet(colour, *values->miRed.m_pValue, *values->miGreen.m_pValue, *values->miBlue.m_pValue, *values->miAlpha.m_pValue);
    poly.FullCoverage(colour, 0.0f);
    poly.Attach(GetLayerView(eCLV_HighRange2D), 0, 0);

    int debugIndex = sHighRangeDebugTextureIndex;
    if (debugIndex >= 0)
    {
        glSetDefaultState(false);
        glSetCurrentTexture(
            gHighRange.mTextures[debugIndex], GLTT_Diffuse);
        glSetRasterState(GLS_AlphaBlend, 1);
        glSetCurrentRasterState(glHandleizeRasterState());
        glSetTextureState(GLTS_DiffuseWrap, 3);
        glSetCurrentTextureState(glHandleizeTextureState());

        glPoly2 debugPoly;
        nlColour white;
        white.c[0] = 255;
        white.c[1] = 255;
        white.c[2] = 255;
        white.c[3] = 255;
        debugPoly.SetColour(white);
        debugPoly.SetupRectangle(
            32.0f, 24.0f, 200.0f, 200.0f, 10000000000.0f);
        debugPoly.Attach(GetDebugFontView(), 2, 0);
    }
}

#ifdef NP_NATIVE
// MSC_HELD_GLOW_HALF: once a picture (every picture, glow or not, so that the count of a held scene starts at its first picture); true when this picture's chain is to be skipped.
// A skip is allowed only when the chain was really drawn within the last two pictures of this held scene (a chain that has not run yet has nothing to show: the first walk-out pictures of the
// host run read an empty level, the composite then lost the whole glow for one picture).
static int sGlowTick = 0, sGlowDrawnAt = -100, sHeldPics = 0;
int HighRangeHeldSkip(int held)
{
    if (!held)
    {
        sHeldPics = 0;
        sGlowTick = 0;
        sGlowDrawnAt = -100;
        return 0;
    }
    ++sGlowTick;
    return NpSceneGlowSkipPicture(NpSceneGlowRate(), sHeldPics++) && NpSceneGlowRate() > 0 && sGlowTick - sGlowDrawnAt <= (NpSceneGlowRate() >= 3 ? 3 : 2);   // (held25 MSC_HELD_GOV_EFFECTS: rate 3 = every 4th picture, three skipped in a row)
}
static int sLiveRun = 0, sLiveTick = 0, sLiveDrawnAt = -100;
extern "C" int VgxGlowHalfPlay(void) __attribute__((weak));   // renderer (vgx_backend_gl.cpp, MSC_GLOW_HALF_PLAY=1): the ladder's glow step is in force
int HighRangeLiveSkip(int live)
{
    static unsigned sCounted = 0;
    const int mode = NpSceneGlowPlay();
    if (!live || mode <= 0 || (mode == 1 && !(&VgxGlowHalfPlay != 0 && VgxGlowHalfPlay())))
    {
        sLiveRun = 0;
        sLiveTick = 0;
        sLiveDrawnAt = -100;
        return 0;
    }
    ++sLiveTick;
    // the chain is skipped only when it was really drawn within the last two pictures (the first picture of a run is drawn in full)
    const int skip = NpSceneGlowPlaySkipPicture(sLiveRun++) && sLiveTick - sLiveDrawnAt <= 2;
    if (skip && (++sCounted == 1 || sCounted % 600 == 0))
        fprintf(stderr, "[glow] MSC_GLOW_HALF_PLAY=%d: %u live-play glow chains skipped (the composite reads the previous picture's chain)\n", mode, sCounted);
    return skip;
}
void HighRangeChainDrawn()
{
    if (sGlowTick > 0) sGlowDrawnAt = sGlowTick;
    if (sLiveTick > 0) sLiveDrawnAt = sLiveTick;
}
// the blur passes are not made: their views carry no target, so no pass and no copy; level 0 (the capture of the EFB) keeps its target unless MSC_GLOW_L0OFF (gem2a: nothing reads it on a
// picture that does not run the chain, so its copy and scene are left out too)
void HighRangeChainOff(HighRange* state)
{
    static unsigned sSkipped = 0, sClearOnly = 0;
    for (int i = NpSceneGlowL0Off() ? 0 : 1; i < 7; ++i)
        state->mViews[i]->m_Target = 0;
    // MSC_GLOW_L0OFF=2 (nxr4b): level 0's copy is not needed (nothing reads hr320d on a picture that skips the chain, unless the composite itself reads level 0: index 0) but its clear of the EFB's
    // alpha plane is (the shadow volumes count in it): the view keeps a target of mode 11, the copy is marked clear-only and the renderer keeps the clear inside the EFB's scene.
    if (NpSceneGlowL0Mode() == 2 && *GetHighRangeTweaks()->miHighRangeIndex.m_pValue >= 1)
    {
        state->mViews[0]->m_Target = GLViewTarget_Mode11;
        ++sClearOnly;
    }
    if (++sSkipped == 1 || sSkipped % 600 == 0)
        fprintf(stderr, "[glow] MSC_HELD_GLOW_HALF=%d: %u held-scene glow chains skipped (the composite reads the previous picture's chain)%s\n", NpSceneGlowRate(), sSkipped,
                NpSceneGlowL0Off() ? ", level 0 capture left out too (MSC_GLOW_L0OFF=1: no alpha clear either, NOT exact)" : (NpSceneGlowL0Mode() == 2 ? ", level 0 copy left out, its alpha clear kept in the scene (MSC_GLOW_L0OFF=2)" : ""));
    if (sClearOnly && (sClearOnly == 1 || sClearOnly % 600 == 0))
        fprintf(stderr, "[glow] MSC_GLOW_L0OFF=2: %u level-0 copies marked clear-only\n", sClearOnly);
}
// MSC_HELD_GLOW_STAGGER (gem2a): the pair of pictures of MSC_HELD_GLOW_HALF=1 shares the chain: the drawn picture makes the capture and the steps down (levels 0..3), the skipped one the steps
// up (levels 4..6, from level 3 of the picture before). The composite reads level 6 on both: the skipped picture's fresh one, the drawn picture's the one before. Only the fine route and a
// composite that reads the last level; everything else keeps the all-or-nothing form. Returns true when it drew (or left out) the chain.
static int sStaggerPrev = 0;   // 0 none yet / a held scene began, 1 steps down drawn, 2 steps up drawn, 3 the whole chain drawn
bool HighRangeStaggerOn(int held)
{
    if (!held) { sStaggerPrev = 0; return false; }
    HighRangeTweaks* t = GetHighRangeTweaks();
    return NpSceneGlowStagger() && (NpSceneGlowRate() == 1 || (NpSceneGlowStaggerAllRates() && NpSceneGlowRate() >= 1)) && GlowFine(t) && (!GlowHalf() || (NpSceneGlowStaggerDead() && !NpSceneGlowHalf() && NpSceneGlowDeadUnused(*t->miHighRangeIndex.m_pValue))) && *t->miHighRangeIndex.m_pValue >= (GlowHalf() ? 5 : 6);
}
void HighRangeChainFull() { sStaggerPrev = 3; }
bool HighRangeChainStagger(HighRange* state, int skipped)
{
    static unsigned sDown = 0, sUp = 0;
    const int fineLevels[7] = { 0, 1, 2, 3, 4, 5, 6 };
    // int28: honour kNpGlowWarm (do not run 5 consecutive down-only passes freezing levels 4..6 during walk-out fade-in)
    if (sStaggerPrev == 0 || sHeldPics <= kNpGlowWarm) return false;   // (no whole chain drawn yet, or still in walk-out warm-up)
    if (skipped && sStaggerPrev == 2 && NpSceneGlowStaggerAllRates()) return false;   // (nxr4b MSC_HELD_GLOW_STAGGER=2: only the picture right after the steps down makes the steps up; the later skipped pictures of a rate 2 / 3 run take the old form)
    const int from = skipped ? 4 : 1, to = skipped ? (GlowHalf() ? 5 : 6) : 3;   // (MSC_HELD_GLOW_STAGGER_DEAD: the dead last level is not drawn)
    for (int i = 0; i < 7; ++i)
        state->mViews[i]->m_Target = (i >= from && i <= to) || (!skipped && i == 0) ? 10 : 0;
    // int28 (real-Vita visual fix: stadium entry / DK fly-in shadow flicker & flashing):
    // mViews[0] clears the EFB alpha plane where shadow volumes count. When skipped==true (steps up only), leaving m_Target=0
    // skipped the EFB alpha clear on alternating frames; preserve the clear via GLViewTarget_Mode11 (same as HighRangeChainOff).
    if (skipped)
    {
        if (NpSceneGlowL0Mode() == 2 && *GetHighRangeTweaks()->miHighRangeIndex.m_pValue >= 1)
            state->mViews[0]->m_Target = GLViewTarget_Mode11;
        else if (!NpSceneGlowL0Off())
            state->mViews[0]->m_Target = 10;
    }
    for (int i = from; i <= to; ++i)
        RenderHighRangePass(state, fineLevels[i], fineLevels[i - 1]);
    sStaggerPrev = skipped ? 2 : 1;
    unsigned& n = skipped ? sUp : sDown;
    if (++n == 1 || n % 600 == 0)
        fprintf(stderr, "[glow] MSC_HELD_GLOW_STAGGER: %u pictures with the steps %s only (%u down, %u up so far)\n", n, skipped ? "up (levels 4..6)" : "down (capture, levels 1..3)", sDown, sUp);
    return true;
}
#endif

void RenderHighRangeChain(HighRange* state)
{
    int fineLevels[7] = { 0, 1, 2, 3, 4, 5, 6 };
    int coarseLevels[5] = { 0, 1, 2, 5, 6 };
#ifdef NP_NATIVE
    if (!NpSceneGlowHalf() && GlowHalf())
    {   // (counter of MSC_FX_GLOW_DEAD: chains drawn without the last level, which the composite does not read)
        static unsigned sDeadChains = 0;
        if (++sDeadChains == 1 || sDeadChains % 600 == 0)
            fprintf(stderr, "[glow] MSC_FX_GLOW_DEAD: %u glow chains drawn without their last level (the composite reads level %d)\n", sDeadChains, *GetHighRangeTweaks()->miHighRangeIndex.m_pValue);
    }
#endif

    if (GlowFine(GetHighRangeTweaks()))
    {
        const int levels = GlowHalf() ? 6 : 7;
        for (int i = 1; i < levels; ++i)
        {
            RenderHighRangePass(state, fineLevels[i], fineLevels[i - 1]);
        }
    }
    else
    {
#ifdef NP_NATIVE
        if (NpSceneGlowCoarse() && *GetHighRangeTweaks()->mbFineHighRange.m_pValue)
        {   // (counter of MSC_FX_GLOW_COARSE: chains drawn on the coarse route, two passes fewer than the fine one)
            static unsigned sCoarseChains = 0;
            if (++sCoarseChains == 1 || sCoarseChains % 600 == 0)
                fprintf(stderr, "[glow] MSC_FX_GLOW_COARSE: %u glow chains drawn on the 4-pass route (%u passes not drawn)\n", sCoarseChains, sCoarseChains * 2);
        }
#endif
        const int levels = GlowHalf() ? 4 : 5;
        for (int i = 1; i < levels; ++i)
        {
            RenderHighRangePass(state, coarseLevels[i], coarseLevels[i - 1]);
        }
    }
}

static inline nlVector2 MakeHighRangeTexcoord(float x, float y)
{
    nlVector2 texcoord;
    nlVec2Set(texcoord, x, y);
    texcoord.x /= glplatGetOrthographicWidth();
    texcoord.y /= glplatGetOrthographicHeight();
    return texcoord;
}

void RenderHighRangePass(
    HighRange*, int outputLevel, int inputLevel)
{
    u32 texture;
    int* sampleOffset;
    nlVector2* vertex;
    HighRange* highRange;
    int gray;
    GLFourTextureAddMeshWriter writer;
    int sampleOffsets[7] = {
        sHighRangeSampleOffset0,
        sHighRangeSampleOffset1,
        sHighRangeSampleOffset2,
        sHighRangeSampleOffset3,
        sHighRangeSampleOffset4,
        sHighRangeSampleOffset5,
        sHighRangeSampleOffset6,
    };
    highRange = &gHighRange;
    texture = highRange->mTextures[inputLevel];

    glSetDefaultState(false);
    glSetRasterState(GLS_AlphaBlend, 6);
    glSetRasterState(GLS_ColourWrite, 2);
    glSetCurrentRasterState(glHandleizeRasterState());

    gray = *GetHighRangeTweaks()->miHighRangeGray.m_pValue;
    if (writer.Begin(4, 3, 0))
    {
        sampleOffset = sampleOffsets;
        vertex = sHighRangeVertices;
        for (int i = 0; i < 4; ++i, ++sampleOffset)
        {
            nlColour colour;
            nlColourSet(colour, gray, gray, gray, gray);
            writer.Colour(colour);

            HighRangeTweaks* values = GetHighRangeTweaks();
            float sampleOffset0 = *sampleOffset
                                * *values->mfHighRangeMult.m_pValue;
            sampleOffset0
                = sampleOffset0 * *values->mfHighRangeOffset.m_pValue;
            writer.Texcoord0(MakeHighRangeTexcoord(
                vertex->x - sampleOffset0, vertex->y));

            values = GetHighRangeTweaks();
            float sampleOffset1 = *sampleOffset
                                * *values->mfHighRangeMult.m_pValue;
            sampleOffset1
                = sampleOffset1 * *values->mfHighRangeOffset.m_pValue;
            writer.Texcoord1(MakeHighRangeTexcoord(
                vertex->x + sampleOffset1, vertex->y));

            values = GetHighRangeTweaks();
            float sampleOffset2 = *sampleOffset
                                * *values->mfHighRangeMult.m_pValue;
            sampleOffset2
                = sampleOffset2 * *values->mfHighRangeOffset.m_pValue;
            writer.Texcoord2(MakeHighRangeTexcoord(
                vertex->x, vertex->y - sampleOffset2));

            values = GetHighRangeTweaks();
            float sampleOffset3 = *sampleOffset
                                * *values->mfHighRangeMult.m_pValue;
            sampleOffset3
                = sampleOffset3 * *values->mfHighRangeOffset.m_pValue;
            nlVector2 texcoord3 = MakeHighRangeTexcoord(
                vertex->x, vertex->y + sampleOffset3);
            float positionX = vertex->x;
            float positionY = vertex->y;
            ++vertex;
            writer.Texcoord3(texcoord3);
            writer.Vertex(positionX, positionY, 0.0f);
        }

        writer.Texture(0, texture);
        writer.Texture(1, texture);
        writer.Texture(2, texture);
        writer.Texture(3, texture);

        if (writer.End())
        {
            highRange->mViews[outputLevel]->AttachModel(writer.model, 0);
        }
    }
}

nlVector2 sHighRangeVertices[4] = {
    { { 0.0f, 0.0f } },
    { { 0.0f, 480.0f } },
    { { 640.0f, 480.0f } },
    { { 640.0f, 0.0f } },
};
