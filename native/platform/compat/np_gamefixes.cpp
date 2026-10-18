// Fixes of bugs of the original game, an option: see include/np/gamefixes.h.
#include "np/gamefixes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
int g_npGameFixes = -1;

int NpGameFixesDecide(void)
{
    int mask = 0;
    const char* e = getenv("MSC_GAME_FIXES");
    if (e && *e)
    {
        if (!strcmp(e, "all"))
        {
            mask = NP_FIX_NET_REDRAW | NP_FIX_ELECTROCUTION_TEXTURE | NP_FIX_NK_BUG;
        }
        else if (e[0] >= '0' && e[0] <= '9')
        {
            mask = atoi(e) & 7;
        }
        else
        {
            if (strstr(e, "net")) mask |= NP_FIX_NET_REDRAW;
            if (strstr(e, "electro")) mask |= NP_FIX_ELECTROCUTION_TEXTURE;
            if (strstr(e, "nk")) mask |= NP_FIX_NK_BUG;
        }
        fprintf(stderr, "[fixes] MSC_GAME_FIXES=%s: net redraw %d, electrocution texture %d, NK bug %d\n", e, (mask & NP_FIX_NET_REDRAW) != 0,
                (mask & NP_FIX_ELECTROCUTION_TEXTURE) != 0, (mask & NP_FIX_NK_BUG) != 0);
    }
    g_npGameFixes = mask;
    return mask;
}
}
