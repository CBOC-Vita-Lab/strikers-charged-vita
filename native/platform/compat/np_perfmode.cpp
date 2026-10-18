// Performance Mode switch: see include/np/perfmode.h.
#include "np/perfmode.h"

#include <stdlib.h>

extern "C" int NpPerformanceMode(void)
{
    static int mode = -1;
    if (mode < 0)
    {
        const char* e = getenv("MSC_PERFORMANCE_MODE");
        mode = (e && *e && *e != '0') ? 1 : 0;
    }
    return mode;
}
