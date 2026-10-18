/* np/perfmode.h: Performance Mode (MSP, wave 4): skip the two decorative draws of a frame, default OFF.
 *
 *   crowd impostors   ImpostorManager::Render in GameRenderTask::RenderFrame (the stands' crowd; the sprites are still updated)
 *   world NPCs        WorldNPCManager::Render (the decorative NPCs around the pitch; they still update)
 *
 * Simulation, updates, gameplay effects and weather are unchanged, so a match plays the same with the mode on or off (state hash equal).
 * Switch: MSC_PERFORMANCE_MODE=1 (environment, env.txt on the Vita); default off.
 *
 * Adapted from yannicksuter/mscharged-port (CC0): patch tweaks/0018-performance-mode-background-rendering (the two skipped calls).
 */
#ifndef NP_PERFMODE_H
#define NP_PERFMODE_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
int NpPerformanceMode(void);
#ifdef __cplusplus
}
#endif
#else
static inline int NpPerformanceMode(void) { return 0; }
#endif

#endif // NP_PERFMODE_H
