// One line for the run header of the logs: the Settings screen's rows that the game side reads (performance mode, frame pacing, the game fixes, the bonus
// unlocks), as the code in force decided them rather than as the file said. Frame rate and picture settings are described by their own modules
// (NpFpsDescribe, VgxDynResDescribe, VgxGfxDescribe, vgx_vita_describe), the controls by NpInputDescribe. tools/settings-audit checks the lines per choice.
#include "np/gamefixes.h"
#include "np/perfmode.h"
#include "np/unlocks.h"

#include <stdio.h>

extern "C" {
int NpFramePaceEnabled(void);

int NpSettingsDescribe(char* out, int size)
{
    return snprintf(out, (size_t)size, "settings: performance-mode=%s frame-pacing=%s game-fixes=net:%d,electro:%d,nk:%d bonus-captains=%s bonus-stadiums=%s",
                    NpPerformanceMode() ? "on" : "off", NpFramePaceEnabled() ? "on" : "off", NpGameFix(NP_FIX_NET_REDRAW) ? 1 : 0,
                    NpGameFix(NP_FIX_ELECTROCUTION_TEXTURE) ? 1 : 0, NpGameFix(NP_FIX_NK_BUG) ? 1 : 0, NpUnlockCaptains() ? "on" : "off", NpUnlockStadiums() ? "on" : "off");
}
}
