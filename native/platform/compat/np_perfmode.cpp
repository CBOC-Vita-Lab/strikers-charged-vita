// Performance Mode switch: see include/np/perfmode.h. 1.3: one shared cell (np/live_settings.h); the settings screen changes it live (draw-only readers).
#include "np/perfmode.h"
#include "../../include/np/live_settings.h"

extern "C" int NpPerformanceMode(void)
{
    return NpLiveCrowdHidden();
}
