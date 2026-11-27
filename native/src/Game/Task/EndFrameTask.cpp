#include <revolution/gx.h>

#include "Game/Task/EndFrameTask.h"
#include "np/gsplit.h"

unsigned char gDrawScreenBorder;

#include "Game/Debug/FrameCounter.h"
#include "Game/HBMManager.h"
#include "Game/Render/RLView.h"
#include "NL/gl/gl.h"
#include "types.h"
#include "Game/Render/Warble.h"
#include "Game/SharedStaticStorage.h"
#include "Game/Render/Amortize.h"
#include "Game/Render/StillBackdrop.h"

void EndFrameTask::Run(float)
{
    if (gpHBMManager == 0 || !gpHBMManager->mActive || !gpHBMManager->mReady)
    {
        bool useWarble = gWarbleEnabled;

        GLView* view = GetLayerView(eCLV_PreWarble);
        unsigned long targetMode = GLViewTarget_None;
        if (useWarble)
        {
            targetMode = GLViewTarget_Mode8;
        }
        view->m_Target = targetMode;

        view = GetLayerView(eCLV_Warble);
        targetMode = GLViewTarget_None;
        if (useWarble)
        {
            targetMode = GLViewTarget_Mode8;
        }
        view->m_Target = targetMode;
        GetLayerView(eCLV_Warble)->m_ClearColour = useWarble;

        if (useWarble)
        {
            NP_GS(NPGS_E_WARBLE);
            UpdateWarbleTexture(&gWarbleEnabled);
            RenderWarbleQuad(&gWarbleEnabled);
        }

        NpGsSeq gsq(NPGS_E_STILL);   // gt27 (MSC_GSPLIT)
        StillBackdropBeforeSend();   // MSC_MENU_STILL, MSC_PAUSE_STILL
        AmortizeBeforeSend();        // MSC_AMORT_LOG, MSC_FX_HAZE_CACHE
        gsq.Next(NPGS_E_SEND);
        glEndFrame();
        g_FrameCounter.StartTimer(1);
        glSendFrame();
        gsq.Next(NPGS_E_STILL);
        StillBackdropAfterSend();
        AmortizeAfterSend();
        gsq.Next(NPGS_E_TAIL);

        if (gpHBMManager != 0 && gpHBMManager->mReady && gpHBMManager->mActive)
        {
            HBMManager::Render();
            glIsFrameActive();
        }

        g_FrameCounter.FinishTiming();

        if (gDrawScreenBorder)
        {
            for (int x = 0; x < 640; ++x)
            {
                GXPokeARGB(x, 0, -1);
                GXPokeARGB(x, 479, -1);
            }

            for (int y = 0; y < 480; ++y)
            {
                GXPokeARGB(0, y, -1);
                GXPokeARGB(639, y, -1);
            }
        }
    }
}
