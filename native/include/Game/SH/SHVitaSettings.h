#ifndef GAME_SH_SHVITASETTINGS_H
#define GAME_SH_SHVITASETTINGS_H

#include "Game/BaseSceneHandler.h"
#include "Game/FE/fePointerButton.h"

class TLComponentInstance;

// np: the VITA SETTINGS screen of a match. It is the game's own pause panel again (art/fe/pausemenu_v3.fen: frame, title band, seven button bars, font,
// pointer hand, hover animation, sounds, slide transitions); only the bar texts and what a press does are ours. State and rules come from the
// settings model (platform/sdk/net/np_native_settings.h); this class draws nothing itself.
class VitaSettingsScene : public BaseSceneHandler
{
public:
    VitaSettingsScene();
    virtual ~VitaSettingsScene();
    virtual void Update(float fDeltaT);
    virtual void SceneCreated();

    // 1.3 (front19): the same panel is also pushed from the front end's OPTIONS screen (OptionsScene, SHOptions.cpp) on GameSceneManager. The OPTIONS screen sets this just
    // before its Push; the next scene made takes it (and clears it). A front-end scene hides the navigation overlay's buttons, takes popups through NpMenuUiShow, reads only the
    // active controller's pointer, offers SAVE AND START AGAIN when a next-start row changed, and leaves to SCENE_OPTIONS.
    static bool sFrontEndNext;

    void InitializePointerButtons();
    void OnPointerEnter(unsigned int index, void* context);
    void OnPointerLeave(unsigned int index, void* context);
    void OnPointerPress(unsigned int index, void* context);

private:
    friend struct VitaSettingsPrivate;
    enum Leave { LEAVE_NONE = 0, LEAVE_PAUSE, LEAVE_ALL_SETTINGS };

    void Relabel();
    void UpdateHelp();   // set18: the help line of the bar under the cursor (np_settings_help.h)
    void Press(int bar);
    void BeginDone();
    void BeginLeave(Leave to);
    void OpenAll();
    void WantPopup(int kind);
    void ServicePopup();
    void ServicePopupFrontEnd();
    void FinishPopup(int pressed);
    void FailedSave();

    TLComponentInstance* mBars[7];
    FEPointerButton mButtons[7];
    int mHoverCounts[4];
    bool mInitialized;
    bool mIntroSoundPlayed;
    bool mLeaving;
    Leave mLeaveTo;
    int mPage;
    int mPopup;          // 0 none, 1 reason (OK), 2 leave (3 buttons)
    int mHovered;        // bar under a pointer, -1 none
    int mAllFrames;      // frames since ONLINE ACCOUNT was asked for (0 = not waiting)
    int mHelpKey;        // set18: the help line last given to NpSettingsHelpShow (page * 16 + bar + 1, 0 = none, -1 = must be sent again)
    bool mBusy;
    int mSwallow;        // frames left in which no press is read after a front-end popup closed
    bool mFrontEnd;      // pushed from OPTIONS (front-end stack) and not from the pause menu (in-match overlay stack)
};

#endif // GAME_SH_SHVITASETTINGS_H
