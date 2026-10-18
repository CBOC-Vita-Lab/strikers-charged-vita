/* np/boot.h (W5 BOOT, 17 Oct 2026): the game's start-up, seen from the port.
 *
 *   NpBootMark(what)      a moment of the start (the boot screens, the title screen), logged with the time since the process
 *                         started ("[boot] <what>  [+N ms, frame F]" on the Vita). "boot screens" also ends the player build's
 *                         loading screen (platform/vita/src/vita_bootscreen.cpp).
 *   NpSkipStrapScreen()   MSC_SKIP_STRAP (environment; np-player.txt / env.txt on the Vita): 1 = the Wii Remote screens of the boot
 *                         ("use the wrist strap" and the Wii Remote + Nunchuk picture) are skipped, the boot goes straight to the
 *                         rating and developer screens (everything the game loads meanwhile is loaded as before). Default: 1 on the
 *                         Vita (there is no Wii Remote), 0 elsewhere (the host's regression traces stay as they are).
 */
#ifndef NP_BOOT_H
#define NP_BOOT_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
void NpBootMark(const char* what);
int NpSkipStrapScreen(void);
#ifdef __cplusplus
}
#endif
#else
static inline void NpBootMark(const char*) {}
static inline int NpSkipStrapScreen(void) { return 0; }
#endif

#endif /* NP_BOOT_H */
