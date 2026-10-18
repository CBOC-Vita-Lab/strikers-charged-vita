// Starts NPS's SDK layer (OS, memory arenas, timers, settings) before main(), for WITH_SDK=1 builds.
// The Wii does this in its start-up code before the game's main; a platform harness may call
// NpSdk_Init itself later (a second call is ignored) or install input/audio hooks first.
#include "np/np_sdk.h"

namespace {
__attribute__((constructor(101))) void NpBootSdk()
{
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);   // NP_DISC_DIR, NP_DATA_DIR, NP_MEM1_MB ... from the environment
    NpSdk_Init(&cfg);
}
}
