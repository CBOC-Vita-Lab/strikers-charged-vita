// The "VITA SETTINGS" item of the game's own pause menu (SHPause.cpp, the button that was the Wii's controller map). Opening it asks the platform's Settings screen
// (np_vita_settings.cpp) to open at this frame's end, over the paused game; closing it with a change offers Save and restart / Save for next start / Discard / Keep editing.
//
// Where it is offered (Decide, host-tested in tests/test_pause_settings.cpp):
//   - the build has a settings screen (the player build on a console; the host and lab builds do not: the Wii's controller map stays);
//   - MSC_PAUSE_SETTINGS is not 0;
//   - no LAN or online session (any phase) exists: the screen takes the display and the game thread stops, which would stall and desync a networked match.
// In those cases the button is untouched (the game's own controller map).
#ifndef NP_PAUSE_SETTINGS_H
#define NP_PAUSE_SETTINGS_H

class TLComponentInstance;

namespace NpPauseSettings {

// The decision, with its inputs spelled out so a test can feed each one. switchValue: the text of MSC_PAUSE_SETTINGS (null: not set).
inline bool Decide(const char* switchValue, bool platformHasScreen, bool networkedSession) {
    if (!platformHasScreen) return false;
    if (switchValue && switchValue[0] == '0') return false;
    if (networkedSession) return false;
    return true;
}

// 1.3 (front19): the game's OPTIONS screen shows the navigation overlay's corner button, relabelled VITA SETTINGS, under the very same rules as the pause item: a build with a
// settings screen, MSC_PAUSE_SETTINGS not 0, and no LAN or online session (any phase). Kept as its own function so the OPTIONS entry has its own mutants.
inline bool DecideOptions(const char* switchValue, bool platformHasScreen, bool networkedSession) {
    if (networkedSession) return false;
    if (switchValue && switchValue[0] == '0') return false;
    return platformHasScreen;
}

// 1.3 (front20): frames after a popup of the settings screen has closed during which the screen takes no pad press and no pointer press. In the front end the popup is closed
// by the very press (Circle, or Cross on a button) that the screen's own Start / Circle handler and bar buttons would read again a frame later (Circle: KEEP EDITING left the
// screen; Cross on KEEP EDITING pressed the bar under the pointer). In a match (the pause menu's overlay stack, the path every player uses) the game's behaviour is kept: 0.
inline int PopupSwallowFrames(bool frontEnd) { return frontEnd ? 12 : 0; }

// 1.3 (front20): a front-end popup waits for a lock whose owner has left the scene stack only as long as that owner's destructor can still be coming (a few frames); after
// `limit` frames in a row with the same lock the entry is leaked and is released. framesGone counts the frames this same lock has been seen with its owner gone.
inline bool StaleLockExpired(bool ownerGone, int framesGone, int limit) { return ownerGone && framesGone >= limit; }

} // namespace NpPauseSettings

// PauseMenuScene::SceneCreated, after it has found its buttons: decides for this pause menu and, when the item is offered, relabels `button` (btn_6). True when offered.
bool NpPauseSettingsOffer(TLComponentInstance* button);
// OptionsScene (the front end's OPTIONS screen): is the VITA SETTINGS corner button offered now? Decided once when the screen's buttons are set; logs "[options] ...".
bool NpOptionsSettingsOffer();
// The press on that button: true when it may open the screen (decided again: a session that started meanwhile takes the press away, the corner button then does nothing).
bool NpOptionsSettingsPress();
// PauseMenuScene::OnOptionPointerPress, case PAUSE_CONTROLLER_MAP: true when this press was the Vita Settings item (the caller then slides the panel out and pushes SCENE_PAUSE_VITA_SETTINGS).
bool NpPauseSettingsPress();

#endif
