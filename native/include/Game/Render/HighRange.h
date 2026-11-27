#ifndef GAME_RENDER_HIGH_RANGE_H
#define GAME_RENDER_HIGH_RANGE_H

#include "Game/TweakValue.h"
#include "NL/gl/glTarget.h"
#include "types.h"

class GLView;

struct HighRangeTweaks
{
    /* 0x00 */ TweakIntBinding miHighRangeIndex;
    /* 0x10 */ TweakFloatBinding mfHighRangeMult;
    /* 0x20 */ TweakFloatBinding mfHighRangeOffset;
    /* 0x30 */ TweakIntBinding miHighRangeGray;
    /* 0x40 */ TweakBoolBinding mbFineHighRange;
    /* 0x50 */ TweakIntBinding miRed;
    /* 0x60 */ TweakIntBinding miGreen;
    /* 0x70 */ TweakIntBinding miBlue;
    /* 0x80 */ TweakIntBinding miAlpha;
};

struct HighRange
{
    struct Viewport
    {
        /* 0x00 */ u32 x;
        /* 0x04 */ u32 y;
        /* 0x08 */ u32 width;
        /* 0x0C */ u32 height;
    };

    /* 0x000 */ GLView* mViews[7];
    /* 0x01C */ Viewport mViewports[7];
    /* 0x08C */ char mNames[7][0x10];
    /* 0x0FC */ u32 mTextures[7];
    /* 0x118 */ GLRenderPair mRenderPairs[7];
};

extern HighRange gHighRange;

void BindHighRangeTweaks(HighRangeTweaks*, const char*);
bool IsHighRangeEnabled(const HighRange*);
void InitializeHighRange(HighRange*);
void SetHighRangeTargetsEnabled(HighRange*, int);
void CompositeHighRange(HighRange*);
void RenderHighRangeChain(HighRange*);
// MSC_HELD_GLOW_HALF (np/scenecut.h): called once a picture with whether a held scene is on; true when this picture's glow chain is not drawn (its views are switched off)
int HighRangeHeldSkip(int held);
// MSC_GLOW_HALF_PLAY (pace13): the same in live play, while the renderer's ladder step is in force (or always with =2); `live` is live play (not a replay / cut-scene)
int HighRangeLiveSkip(int live);
void HighRangeChainOff(HighRange*);
void HighRangeChainDrawn();
// MSC_HELD_GLOW_STAGGER (gem2a): see HighRange.cpp
bool HighRangeStaggerOn(int held);
bool HighRangeChainStagger(HighRange*, int skipped);
void HighRangeChainFull();
void RenderHighRangePass(HighRange*, int, int);
HighRangeTweaks* GetHighRangeTweaks();

#endif // GAME_RENDER_HIGH_RANGE_H
