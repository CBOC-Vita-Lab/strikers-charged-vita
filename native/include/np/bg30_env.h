/* MSC_BG30: ONE parser for every reader of the switch (np_bg30.cpp, amortize.h's NpViewLayers, the renderer's vgx_bg30.h and vgx_skin.cpp). Before 18 Nov they disagreed: =3 / =on / =yes turned the draw
   class marks on (and everything that follows from VIEW_LAYERS) while bg30 itself stayed off. Pure, no platform includes.
   0 (empty, "0", "off", any word not listed): off;  1: "1", "proof", "on", "yes", "true";  2: "2", "half". */
#ifndef NP_BG30_ENV_H
#define NP_BG30_ENV_H
#include <string.h>
static inline int NpBg30ParseMode(const char* e)
{
    if (!e || !*e) return 0;
    if (!strcmp(e, "1") || !strcmp(e, "proof") || !strcmp(e, "on") || !strcmp(e, "yes") || !strcmp(e, "true")) return 1;
    if (!strcmp(e, "2") || !strcmp(e, "half")) return 2;
    return 0;
}
#endif
