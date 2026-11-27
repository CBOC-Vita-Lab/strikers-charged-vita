// The setter of the three live cells (np/live_settings.h). The readers are inline in the header.
#include "../../include/np/live_settings.h"

#include <stdio.h>
#include <string.h>

extern "C" int NpLiveSettingsSet(const char* key, const char* valueOrNull)
{
    if (!key) return 0;
    int id = -1;
    if (!strcmp(key, "MSC_REDUCED_MOTION")) id = NP_LIVE_SHAKE;
    else if (!strcmp(key, "MSC_PERFORMANCE_MODE")) id = NP_LIVE_CROWD;
    else if (!strcmp(key, "MSC_REAR_TOUCH")) id = NP_LIVE_REAR;
    if (id < 0) return 0;
    int v;
    if (id == NP_LIVE_REAR) v = (valueOrNull && *valueOrNull) ? (*valueOrNull != '0' ? 1 : 0) : -1;       // same rule as the first read
    else v = (valueOrNull && *valueOrNull && *valueOrNull != '0') ? 1 : 0;
    __atomic_store_n(NpLiveCell(id), v, __ATOMIC_RELAXED);
    fprintf(stderr, "[live] %s=%s -> cell %d\n", key, valueOrNull ? valueOrNull : "(unset)", v);
    return 1;
}
