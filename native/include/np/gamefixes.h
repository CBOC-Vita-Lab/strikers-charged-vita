/* np/gamefixes.h: fixes of bugs of the ORIGINAL game (MSP, wave 4), an option, default OFF (the game stays as retail).
 *
 *   MSC_GAME_FIXES=all | net,electro,nk | a number (bit mask: 1 net, 2 electro, 4 nk)     (environment; env.txt on the Vita)
 *
 *   net      resting goal nets are redrawn (issue #19): DrawableNetMesh::Render rebuilds the mesh only while the net simulates or a replay frame
 *            changed its visibility, and NetMesh::Reset at the start and end of a goal replay does not invalidate it, so the shape last drawn in
 *            the replay stays on screen (a net left bulged outward after a goal) until the ball enters it again. Drawing only.
 *   electro  the electrocution overlay texture is loaded (issue #33): effects/fx_electrocution is in no bundle the game loads (only in
 *            art/effects/debugEffectsNonRes.bun, never read), so the overlay of an electrocuted player draws the missing-texture fallback
 *            (black with white letters). That one texture joins the resident effects. Drawing only; needs the file on the disc.
 *   nk       the "NK bug" / Boo glitch (issue #38): a fielder who turns intangible while carrying the ball also switches off the ball's player
 *            and goalie collisions; if he loses the ball while intangible they stay off and every shot scores until the next kick-off.
 *            cPlayer::ReleaseBall gives a released ball its collisions back, and a goalie no longer takes the ball from an intangible
 *            teammate. THIS CHANGES THE SIMULATION: in a LAN match every console must use the same setting.
 *
 * Adapted from yannicksuter/mscharged-port (CC0): tweaks patches 0006, 0009, 0010.
 */
#ifndef NP_GAMEFIXES_H
#define NP_GAMEFIXES_H

#define NP_FIX_NET_REDRAW 1
#define NP_FIX_ELECTROCUTION_TEXTURE 2
#define NP_FIX_NK_BUG 4

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
extern int g_npGameFixes;   /* -1 until decided */
int NpGameFixesDecide(void);
#ifdef __cplusplus
}
#endif
static inline int NpGameFix(int bit)
{
    int m = g_npGameFixes;
    if (m < 0) m = NpGameFixesDecide();
    return m & bit;
}
#else
static inline int NpGameFix(int) { return 0; }
#endif

#endif // NP_GAMEFIXES_H
