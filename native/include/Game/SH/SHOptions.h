#ifndef GAME_SH_SH_OPTIONS_H
#define GAME_SH_SH_OPTIONS_H

#include "Game/BaseGameSceneManager.h"
#include "Game/FE/fePointerButton.h"
#include "Game/FE/feBackButton.h"

class TLComponentInstance;

enum eOptionsMenuAction
{
    OPTIONS_VISUAL = 0,
    OPTIONS_AUDIO = 1,
    OPTIONS_CREDITS = 2,
};

class OptionsScene : public BaseSceneHandler
{
public:
    OptionsScene();
    virtual ~OptionsScene();
    virtual void Update(float fDeltaT);
    virtual void SceneCreated();

    void OnButtonPointerPress(int index, void* context);
    void OnButtonPointerEnter(int index, void* context);
    void OnButtonPointerLeave(int index, void* context);
    void InitializePointerButtons();
    // 1.3 (front19): the navigation overlay's DONE corner button, relabelled VITA SETTINGS (offered when NpOptionsSettingsOffer says so): opens SCENE_PAUSE_VITA_SETTINGS (SHVitaSettings.cpp)
    void OnDonePointerPress(int index, void* context);
    void OnDonePointerEnter(int index, void* context);
    void OnDonePointerLeave(int index, void* context);

    enum ScenePhase
    {
        PHASE_ENTERING = 0,
        PHASE_CHOOSING = 1,
        PHASE_EXITING_TO_OPTION = 2,
        PHASE_EXITING_TO_MAIN_MENU = 3,
    };

    /* 0x01C */ TLComponentInstance* mOptionInstances[3];
    /* 0x028 */ FEPointerButton mOptionButtons[3];
    /* 0x244 */ FEBackButton mBackButton;
    /* 0x31C */ bool mPointerButtonsInitialized;
    /* 0x31D */ u8 mPadding31D[3];
    /* 0x320 */ int mScenePhase;
    /* 0x324 */ SceneList mNextScene;
    // np (front19): not in the original layout (nothing reads OptionsScene's size)
    FEPointerButton mDoneButton;
    TLComponentInstance* mDoneInstance;
    bool mDoneOffered;
};

#endif // GAME_SH_SH_OPTIONS_H
