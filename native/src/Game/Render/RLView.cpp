#include "Game/Render/RLView.h"
#include <stdlib.h>
#include "Game/Render/RLViewLayers.h"

#include "NL/gl/glModel.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxGX.h"
#include "NL/glx/glxSend.h"
#ifdef NP_NATIVE
#include "np/amortize.h"
#include "np/fx.h"
#endif

static bool sFogState;
static bool sFogChanged;

RLView::RLView(GLViewInterface* viewInterface, const GLRenderPair& renderPair, GLViewSortMode sortMode)
    : GLView(viewInterface, renderPair, sortMode)
{
    mFogEnabled = true;
    mCoPlanarEnabled = false;
}

RLView::~RLView()
{
}

#ifdef NP_NATIVE
// W12 FG: the draw class of each layer view (np/fx.h NPFX_*), for the renderer's foreground / background split. The layers that hold both kinds
// (eCLV_WorldShadowed: decorative NPCs and cut-scene characters; eCLV_HighRange3D: far scenery and the Mega Strike characters) are foreground
// or are told apart by the renderer; what is not listed is unknown, and unknown counts as foreground.
static int LayerDrawClass(const RLView* view, int* layerOut)
{
    const int layer = view->m_Unknown48;
    // MSC_VGX_HUDNATIVE needs the layer of every draw. In a forked send the view here is a shadow, never sLayerViews[layer], so the layer and the class below have always been 0 / unknown
    // on the Vita. Only the layer number is mended, and only under that switch: the class stays unknown (a real class would wake the foreground split, a change of its own).
    static const bool shadowsToo = [] {
        const char* e = getenv("MSC_VGX_HUDNATIVE");
        return (e && e[0] != 0 && e[0] != '0') || getenv("MSC_VGX_PASSES_HUD") != 0;
    }();
    if (shadowsToo && !NpViewLayers() && layer >= 0 && layer < eCLV_Num && sLayerViews[layer] != view && sLayerViews[layer] == GLViewIdentity(view))
    {
        *layerOut = layer;
        return NPFX_UNKNOWN;
    }
    // MSC_VIEW_LAYERS (np/amortize.h): the two-stage pipeline sends shadows of the views (glPlat.cpp), which never equal sLayerViews; off, those all read as unknown, as in 1.2
    if (layer < 0 || layer >= eCLV_Num || sLayerViews[layer] != (NpViewLayers() ? GLViewIdentity(view) : view))
    {
        *layerOut = 0;
        return NPFX_UNKNOWN;   // (the shadow partition views, etc.)
    }
    *layerOut = layer;
    switch (layer)
    {
    case eCLV_Shadowed: return NPFX_STADIUM;
    case eCLV_WorldAlphaBlended: return NPFX_STADIUM_A;
    case eCLV_ImpostorOut: return NPFX_CROWD;
    case eCLV_ImpostorTexture: return NPFX_CROWD_CAP;
    case eCLV_NoFog: return NPFX_SKY;
    case eCLV_HighRange3D:
    case eCLV_HighRange3DNoFog: return NPFX_FAR;
    case eCLV_Characters:
    case eCLV_MoreCharacters:
    case eCLV_PeachPhoto3D: return NPFX_PLAYER;
    case eCLV_Unshadowed: return NPFX_ITEM;
    case eCLV_WorldShadowed: return NPFX_NPC;
    case eCLV_LingeringParticles:
    case eCLV_Particles:
    case eCLV_BallChargeAlphaBlended: return NPFX_FX;
    case eCLV_FrontEnd:
    case eCLV_UnsortedOrtho640:
    case eCLV_UnsortedSquareOrtho:
    case eCLV_HighRange2D:
    case eCLV_HighRangeChain:
    case eCLV_CameraSpace:
    case eCLV_Transitions:
    case eCLV_HomeButtonFadeOut: return NPFX_HUD;
    default: return NPFX_UNKNOWN;
    }
}
#endif

#ifdef NP_NATIVE
#include "np/bg30.h"
// 1.3 MSC_BG30: in a light picture the background layer views are not sent (the renderer re-projects the kept background). The layers are the
// renderer's background classes (vgx_bg30.h BgCls): stadium opaque, crowd quads, sky, far scenery.
extern "C" int NpBg30ViewSkip(const void* view)
{
    if (!NpBg30SendLight())
        return 0;
    static const eCLV kBg[] = { eCLV_Shadowed, eCLV_ImpostorOut, eCLV_NoFog, eCLV_HighRange3D, eCLV_HighRange3DNoFog };
    for (unsigned i = 0; i < sizeof kBg / sizeof kBg[0]; ++i)
        if (GetLayerView(kBg[i]) == GLViewIdentity(static_cast<const GLView*>(view)))   // (a forked frame sends shadows of the views: compare the live views they stand for)
            return 1;
    return 0;
}
#endif

void RLView::BeginRender()
{
#ifdef NP_NATIVE
    {
        int layer = 0;
        const int cls = LayerDrawClass(this, &layer);
        NpFxSetLayer(cls, layer);
    }
#endif
    sFogState = mFogEnabled;
    glx_Fog(mFogEnabled);
}

void RLView::EndRender()
{
#ifdef NP_NATIVE
    NpFxSetLayer(NPFX_UNKNOWN, 0);
#endif
    if (mCoPlanarEnabled)
    {
        gxSetCoPlanar(false);
    }
}

void RLView::BeginPacket(const glModelPacket* packet)
{
    unsigned long state = glGetRasterState(packet->rasterState, GLS_AlphaBlend);
    bool fog = false;
    if (mFogEnabled && state != 2 && state != 3)
    {
        fog = true;
    }
    sFogChanged = fog != sFogState;
    if (sFogChanged)
    {
        glx_Fog(fog);
    }
    if (mCoPlanarEnabled && packet->indexBuffer != 0)
    {
        gxSetCoPlanar(true);
    }
}

void RLView::EndPacket(const glModelPacket* packet)
{
    if (mCoPlanarEnabled && packet->indexBuffer != 0)
    {
        gxSetCoPlanar(false);
    }
}
