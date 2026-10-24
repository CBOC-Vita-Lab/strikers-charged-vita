/* np/camzoom.h: the match camera's default zoom for the Vita's small screen (W5-CAM, wave 5).
 *
 * The game has its own camera zoom option (Options > Visual: Auto Zoom on/off, Zoom Level 1..5, kept in the save as
 * VisualSettings: mIsAutoZoomCamera, mCameraZoomLevel = (level - 1) / 4, so level 5 is the closest of retail).
 *
 * Levels 6 and 7 (17 Oct, the maintainer: "a more zoomed in option ... follow the previous logic"): mCameraZoomLevel 1.25 and 1.5, the same steps
 * again. Options > Visual: pressing level 5's button again goes 5 -> 6 -> 7 -> 5 (the screen has five buttons; the text shows the level).
 * GameplayCam.cpp lets these levels go past the game's near camera (20 m at 30 degrees) along the same line: with Auto Zoom on, open play
 * 24.3 m (6) / 20.8 m (7) against 27.9 m at 5, the attacking half 18.6 / 15.1 m against 22.2 m, the closest moments 17.2 / 13.6 m; Auto
 * Zoom off 17.2 / 13.6 m. Levels 1..5 are unchanged bit for bit. The players' on-screen flags the simulation gets are taken over a wider
 * window at 6 and 7 (NpCameraVisibilityWindow) so that the CPU team plays as it does at level 5. Retail's default is Auto Zoom on,
 * level 3, made for a TV. On the Vita the default is level 5, the closest (the maintainer, 17 Oct 07:27, after
 * the level 3/4/5 pictures; first set at 4 at 05:48), and Auto Zoom still pulls the camera out when play spreads (docs/wave5-CAM.md has the numbers and pictures).
 *
 *   default level      NP_CAMERA_ZOOM_DEFAULT (compile time: 5 for the Vita (__vita__), 3 elsewhere, so host32, armhf and the
 *                      regression suite keep retail's camera), overridden by the environment MSC_CAMERA_ZOOM_DEFAULT=1..7
 *                      (env.txt / np-player.txt on the Vita). Used for the settings of a new save (VisualSettings' constructor).
 *   one-time move      a save that still holds the TV default (level 3, Auto Zoom on) when it is loaded, in a data directory that has
 *                      no marker file <data dir>/np-camera-zoom.txt yet, gets the default level in memory (the maintainer save, imported once
 *                      from the recompiled build by np_save_import, is such a save). The marker is written the first time the game
 *                      writes its save (whatever level it then holds), so from then on the level in the save is the player's own
 *                      choice and is never changed again (a level 3 he picks later in Options included). Until that first write the
 *                      file still holds level 3, and every load moves it again (same result).
 *   test override      MSC_CAMERA_ZOOM=1..7 and/or MSC_CAMERA_AUTO=0|1 force the settings over the new-save defaults and over every
 *                      loaded save (measurement runs and tests; not a player setting; the Options menu still changes them in the session).
 *   test visibility    MSC_TEST_VISIBILITY=all|none: the player on-screen flags machine 0 puts into the simulation's input (below) are
 *                      replaced by a constant (tests only: it changes the match; it shows that the flags are the camera's only way in).
 *   camera log         MSC_CAMERA_LOG=1 (or N: every Nth frame): one "[cam]" line per frame with the projection the frame used (view
 *                      matrix, vertical FOV, aspect), the gameplay camera's zoom state, the ball and the ten players (tools/cam/).
 *
 * The camera reaches the simulation in one place, by the original game's design: every 10 simulation frames machine 0 (the only machine
 * in single player, the host in a LAN match) sends which of the ten players are on its screen (cGame::SendPlayerVisibility,
 * DETERM_PLAYER_VISIBILITY, from the HUD's projection in Indicators.cpp), every console applies the flags to m_DetChar.m_bOnScreen (part of
 * the game's checksum), and the AI reads them (script builtin 231 "on screen"; sideline avoidance of controlled fielders on Thunder Island).
 * A closer camera puts fewer players on screen, so a single-player match plays differently from the same inputs at another level (as on
 * the Wii at any level). In a LAN match the flags travel in the deterministic input stream, so all consoles apply the host's flags and stay
 * in sync whatever level each one shows (as on the Wii, where each console of an online match had its own option).
 * docs/wave5-CAM.md has the proof runs.
 */
#ifndef NP_CAMZOOM_H
#define NP_CAMZOOM_H

#ifndef NP_CAMERA_ZOOM_MAX_LEVEL
#define NP_CAMERA_ZOOM_MAX_LEVEL 7
#endif

#ifndef NP_CAMERA_ZOOM_DEFAULT
#if defined(__vita__)
#define NP_CAMERA_ZOOM_DEFAULT 5
#else
#define NP_CAMERA_ZOOM_DEFAULT 3
#endif
#endif

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
/* the default level 1..7 (NP_CAMERA_ZOOM_DEFAULT or MSC_CAMERA_ZOOM_DEFAULT) */
int NpCameraZoomDefaultLevel(void);
/* VisualSettings' constructor: the settings of a new save (level and Auto Zoom in the game's units) */
void NpCameraZoomNewSettings(bool* autoZoom, float* zoomLevel);
/* SaveLoad::ReadSaveData, after the settings were read from the save: the one-time move and the test override */
void NpCameraZoomSettingsLoaded(bool* autoZoom, float* zoomLevel);
/* SaveLoad::WriteSaveData, before the settings go into the save: writes the marker the first time */
void NpCameraZoomSettingsWritten(bool autoZoom, float zoomLevel);
/* cGame::SendPlayerVisibility (test only): MSC_TEST_VISIBILITY=all|none replaces the on-screen flags machine 0 sends (bits 0x1F per team);
   -1 when unset (the game's own flags). It changes the simulation: it exists to show that these flags are how the camera reaches it. */
int NpCameraTestVisibility(void);
/* cGame::SendPlayerVisibility: the |x|, |y| limit of the on-screen test for the zoom option's value (0.95, the game's, up to level 5) */
float NpCameraVisibilityWindow(float zoomLevel);
float NpCameraVisibilityWindowFixed(float zoomLevel);   /* Auto Zoom off */
#ifdef __cplusplus
}
/* GameplayCam.cpp (game side, needs the game's headers): the MSC_CAMERA_LOG line; called by SetupMatrices with what it hands the renderer */
void NpCameraLogFrame(float fovDeg, float aspect);
#endif
#endif

#endif // NP_CAMZOOM_H
