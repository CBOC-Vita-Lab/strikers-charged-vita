// MSC_STADIUM_HEAVY (np/scenecut.h): the reductions of a stadium the console census named as heavy. Nothing in the code knows such a stadium: what makes one heavy is in its data.
// Only the drawing paths read this flag (GameRenderTask::RenderFrame, WorldNPCManager::Update's pose of NPCs that are not drawn, the shadow interval of replays) and the renderer;
// peers may differ in it, as they may in Stadium Crowd.
#include "np/scenecut.h"
#include "np/stadiumcut.h"
#include "np/bg30.h"

#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>

extern "C" void VgxHeavyStadium(int on) __attribute__((weak)); // renderer (vgx_backend_gl.cpp): smaller floor, small effects, half-size warble
extern "C" void VgxStadiumIs(int stadium) __attribute__((weak)); // renderer (MSC_PICTURE_HOLD): which stadium the match is in, for what the picture policy learned there

static int sHeavy;

int NpStadiumHeavy()
{
    return sHeavy;
}

int NpStadiumHeavyCrowd()
{
    static int keep = -1;
    if (keep < 0)
    {
        const char* e = getenv("MSC_STADIUM_HEAVY_CROWD");
        keep = e && e[0] == '0' ? 1 : 0;
    }
    return sHeavy && !keep;
}

void NpStadiumHeavySet(int stadium)
{
    NpStadiumCutSet(stadium);   // (MSC_STADIUM_CUT, np/stadiumcut.h)
    NpBg30StadiumSet(stadium);   // (wave 10 MSC_BG30_CUTS: the per-stadium background cut row)
    if (VgxStadiumIs) VgxStadiumIs(stadium);
    const int heavy = NpSceneHeavyHas(getenv("MSC_STADIUM_HEAVY"), stadium);
    if (heavy == sHeavy) return;
    sHeavy = heavy;
    if (VgxHeavyStadium) VgxHeavyStadium(heavy);
    fprintf(stderr, "[heavy] stadium %d: heavy profile %s (crowd %s, replay shadows every 2nd frame, haze 1 in %d, renderer reductions %s)\n", stadium, heavy ? "on" : "off",
            NpStadiumHeavyCrowd() ? "hidden" : "kept", NpSceneHazeEvery(), VgxHeavyStadium ? "applied" : "not in this renderer");
}

#endif // NP_NATIVE
