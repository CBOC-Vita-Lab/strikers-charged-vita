#include "Game/SH/SHVitaSettings.h"
#include "Game/SH/SHNavigation.h"

#include "Game/BaseGameSceneManager.h"
#include "Game/FE/FEAudio.h"
#include "Game/FE/feDPD.h"
#include "Game/FE/feFinder_impl.h"
#include "Game/FE/feInput.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/FE/feScene.h"
#include "Game/FE/feSceneManager.h"
#include "Game/GameSceneManager.h"
#include "Game/Render/ShootToScoreArrow.h"
#include "NL/nlBind.h"
#include "NL/nlFunction.h"
#include "NL/nlFunction.inl"
#include "platform/sdk/include/np/np_text.h"
#include "platform/sdk/net/np_menu_flow.h"
#include "platform/sdk/net/np_native_settings.h"
#include "platform/sdk/net/np_pause_settings.h"
#include "platform/sdk/net/np_settings_help.h"
#include "Game/FE/feText.h"
#include "Game/FE/feFontResource.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/tlComponent.h"
#include "Game/FE/tlComponentInstance.h"
#include "Game/FE/tlInstance.h"
#include "Game/FE/tlSlide.h"
#include "Game/FE/tlTextInstance.h"

#include <cstdio>
#include <cstdlib>



namespace {

typedef VitaSettingsScene Scene;
namespace NS = NpNativeSettings;

const char* const kBarNames[7] = {"btn_0", "btn_1", "btn_2", "btn_3", "btn_4", "btn_5", "btn_6"};
const char* const kStates[3] = {"off", "over", "down"};
const int kRows = 5;                        // bars 0-4 are the rows
const int kDone = 5;                        // btn_5 is the last bar on the panel (the pause menu's QUIT)
const int kMore = 6;                        // btn_6 is the one above it (the pause menu's VITA SETTINGS)
const int kBuf = 80;                        // UTF-16 units of one bar text (the longest label is 24 characters)
unsigned short g_barText[7][kBuf];          // the game keeps the pointer it is given (TLTextInstance::m_wcUserString): static
TLTextInstance* g_barTexts[7][3];           // the three states' "option" texts of each bar
nlColour g_barColour[7][3];                 // their colour as the scene made it (a grey row is a colour override; going back restores this)
bool g_barGrey[7];

const unsigned kSoundIntro = 0xBB142B94, kSoundHover = 0xF6EB899E, kSoundPress = 0xF0AFD586, kSoundChanged = 0x3021A1EE, kSoundBack = 0x304FDD1E;

void Sound(unsigned id) { FEAudio::PlayAnimAudioEvent(id, 0, 0, 1); }

// the popup (np_menu_ui.cpp) that is open for this screen: what the result means
enum VsPopup { POPUP_NONE = 0, POPUP_REASON, POPUP_LEAVE, POPUP_SAVE_FAILED, POPUP_RESTART_ASK };
VsPopup g_popupKind = POPUP_NONE;
bool g_popupWanted = false;                 // a popup is to be shown as soon as the game allows one
char g_popupText[320], g_popupB[3][NS::kMaxLabelBytes];
int g_popupButtons = 0;
FEPopupMenu* g_popupObj = 0;
int g_result = -1;                          // the button pressed (set by the popup's callback when its scene is destroyed)
int g_goneFrames = 0;
void Pressed0() { g_result = 0; }
void Pressed1() { g_result = 1; }
void Pressed2() { g_result = 2; }
void PressedBack() {}

void Utf16(const char* utf8, unsigned short* out, int cap) { NpText::ToUtf16(utf8, out, cap - 1); }

}  // namespace

bool VitaSettingsScene::sFrontEndNext = false;
static const int kStaleLockFrames = 20;   // a front-end popup waits at most this long for a lock whose owner has left (np_menu_ui.cpp)
static int g_sinceShown = 0;          // front end: frames since the popup was pushed (log only)
static bool g_readyReported = false;

// one scene at a time: the pointer callbacks are plain methods of it
VitaSettingsScene::VitaSettingsScene()
    : mInitialized(false)
    , mIntroSoundPlayed(false)
    , mLeaving(false)
    , mLeaveTo(LEAVE_NONE)
    , mPage(0)
    , mPopup(0)
    , mHovered(-1)
    , mAllFrames(0)
    , mHelpKey(0)
    , mBusy(false)
    , mSwallow(0)
    , mFrontEnd(sFrontEndNext)
{
    sFrontEndNext = false;
    for (int i = 0; i < 7; ++i) {
        mButtons[i].mContext = (void*)i;
        mBars[i] = 0;
    }
    for (int i = 0; i < 4; ++i) mHoverCounts[i] = 0;
}

VitaSettingsScene::~VitaSettingsScene() {
    NpSettingsHelpShow(0, 0, 0, 0);   // (set18: the help line goes with the panel)
    g_popupKind = POPUP_NONE;
    g_popupWanted = false;
    NS::Close();
}

void VitaSettingsScene::SceneCreated() {
    if (mFrontEnd) {
        // the front end's navigation overlay owns the corner buttons (BACK, DONE...): this screen has its own DONE bar
        SHNavigation* navigation = GetNavigationScene();
        if (navigation) navigation->SetButtons(NAVIGATION_BUTTON_NONE, true);   // (zeroes the visible bits too: HideButtons alone let a pushed popup bring OPTIONS' corner button back)
    }
    FEAudio::EnableSounds(false);
    FEPresentation* presentation = mFEScene->m_pFEPackage->GetPresentation();
    for (int i = 0; i < 7; ++i) mBars[i] = FEFinder<TLComponentInstance, 4>::FindOrDefault(presentation->m_currentSlide, "Layer", kBarNames[i]);
    FEAudio::EnableSounds(true);
    for (int i = 0; i < 7; ++i)
        for (int k = 0; k < 3; ++k) {
            g_barTexts[i][k] = FEFinder<TLTextInstance, 3>::FindOrDefault(mBars[i], kStates[k], "option");
            g_barColour[i][k] = g_barTexts[i][k]->GetAssetColour();
        }
    for (int i = 0; i < 7; ++i) g_barGrey[i] = false;
    // the title band is text: TITLE2 holds the slides "challenges" (the pause menu) and "tutorials", each with three text layers (TITLE, TITLE2, TITLE3)
    static unsigned short title[40];
    Utf16(NpText::Tr("VITA SETTINGS"), title, 40);
    TLComponentInstance* band = FEFinder<TLComponentInstance, 4>::FindOrDefault(presentation->m_currentSlide, "Layer", "TITLE2");
    static const char* const kTitleSlides[2] = {"challenges", "tutorials"};
    static const char* const kTitleTexts[3] = {"TITLE", "TITLE2", "TITLE3"};
    for (const char* sl : kTitleSlides)
        for (const char* tx : kTitleTexts) FEFinder<TLTextInstance, 3>::FindOrDefault(band, sl, tx)->SetString(title);
    if (!NS::Open()) std::fprintf(stderr, "[vitaset] the settings model did not open\n");
    mPage = 0;
    Relabel();
    std::fprintf(stderr, "[vitaset] screen created (%s): pages %d, rows %d\n", mFrontEnd ? "front end" : "match", NS::Pages(), NS::Rows(0));
}

// the texts of the seven bars for the page being shown
void VitaSettingsScene::Relabel() {
    char text[NS::kMaxLabelBytes];
    for (int bar = 0; bar < 7; ++bar) {
        bool grey = false;
        if (bar < kRows) {
            if (bar < NS::Rows(mPage)) {
                NS::Label(mPage, bar, text, sizeof text);
                grey = NS::Dim(mPage, bar);
            } else {
                text[0] = 0;
            }
        } else if (bar == kMore) {
            NS::MoreLabel(mPage, text, sizeof text);
        } else {
            NS::DoneLabel(text, sizeof text);
        }
        Utf16(text, g_barText[bar], kBuf);
        for (int k = 0; k < 3; ++k) {
            if (!g_barTexts[bar][k]) continue;
            g_barTexts[bar][k]->SetString(g_barText[bar]);
            if (grey) {
                nlColour c = g_barColour[bar][k];
                c.c[0] = (unsigned char)(c.c[0] / 2 + 40);
                c.c[1] = (unsigned char)(c.c[1] / 2 + 40);
                c.c[2] = (unsigned char)(c.c[2] / 2 + 40);
                g_barTexts[bar][k]->SetAssetColour(c);
            } else if (g_barGrey[bar]) {
                g_barTexts[bar][k]->SetAssetColour(g_barColour[bar][k]);
            }
        }
        g_barGrey[bar] = grey;
    }
    mHelpKey = -1;   // (the page or the language may have changed: the help line is sent again)
}

// set18: the one-line help of the row under the cursor, on the panel's steel foot, in the bars' own lettering (np_settings_help.h). Nothing over MORE or DONE, while the panel
// slides, while a popup is up, or while the ONLINE page is open.
void VitaSettingsScene::UpdateHelp() {
    const bool shown = mInitialized && !mLeaving && mPopup == POPUP_NONE && !g_popupWanted && mAllFrames == 0;
    const int bar = shown ? mHovered : -1;
    const int key = bar >= 0 && bar < kRows ? mPage * 16 + bar + 1 : 0;
    if (key == mHelpKey) return;
    mHelpKey = key;
    char line[200];
    NS::HelpLine(mPage, bar, shown, line, sizeof line);
    const nlFont* font = 0;
    const nlColour* colour = 0;
    if (TLTextInstance* t = g_barTexts[kDone][0]) {
        const FEText* text = (const FEText*)t->m_component;
        if (text && text->m_pFeFontResource && text->m_pFeFontResource->m_bValid) font = text->m_pFeFontResource->GetFontReference();
        colour = &g_barColour[kDone][0];
    }
    NpSettingsHelpShow(line, font, colour, mFEScene);
}

void VitaSettingsScene::InitializePointerButtons() {
    typedef Detail::MemFunImpl<void, void (Scene::*)(unsigned int, void*)> PointerMethod;
    typedef BindExp3<void, PointerMethod, Scene*, Placeholder<0>, Placeholder<1> > PointerBinding;
    FEPointerListener::Callback enter(PointerBinding(MemFun(&Scene::OnPointerEnter), this, Placeholder<0>(), Placeholder<1>()));
    FEPointerListener::Callback leave(PointerBinding(MemFun(&Scene::OnPointerLeave), this, Placeholder<0>(), Placeholder<1>()));
    FEPointerListener::Callback press(PointerBinding(MemFun(&Scene::OnPointerPress), this, Placeholder<0>(), Placeholder<1>()));
    for (int i = 0; i < 7; ++i) {
        mButtons[i].SetInstanceBounds(mBars[i], true, 0.0f, 0.0f, 1.0f, 0.5f);
        mButtons[i].SetPointerEnterCallback(enter);
        mButtons[i].SetPointerLeaveCallback(leave);
        mButtons[i].SetPointerPressCallback(press);
    }
}

// hover: the game's own lighting and sound, as the pause menu
void VitaSettingsScene::OnPointerEnter(unsigned int index, void* context) {
    const unsigned int which = (unsigned int)context;
    ++mHoverCounts[index];
    if (!mButtons[which].HasOtherPointerState(POINTER_BUTTON_HOVER, index)) {
        Sound(kSoundHover);
        mBars[which]->SetActiveSlide("over", true, false);
    }
    mButtons[which].SetPointerState(POINTER_BUTTON_HOVER, index);
    mHovered = (int)which;
}

void VitaSettingsScene::OnPointerLeave(unsigned int index, void* context) {
    const unsigned int which = (unsigned int)context;
    --mHoverCounts[index];
    if (!mButtons[which].HasOtherPointerState(POINTER_BUTTON_HOVER, index)) mBars[which]->SetActiveSlide("off", true, false);
    mButtons[which].SetPointerState(POINTER_BUTTON_NORMAL, index);
    if (mHovered == (int)which) mHovered = -1;
}

void VitaSettingsScene::OnPointerPress(unsigned int index, void* context) {
    (void)index;
    if (mBusy || mLeaving || mPopup != POPUP_NONE || g_popupWanted) return;
    Press((int)(unsigned int)context);
}

// a press on bar `bar`
void VitaSettingsScene::Press(int bar) {
    if (bar >= kRows || (bar < NS::Rows(mPage) && !NS::Dim(mPage, bar))) Sound(kSoundPress);   // a grey row plays the hover sound only
    if (bar == kDone) {
        BeginDone();
        return;
    }
    if (bar == kMore) {
        mPage = NS::NextPage(mPage);
        Relabel();
        return;
    }
    if (bar >= NS::Rows(mPage)) return;
    switch (NS::Press(mPage, bar, 0)) {
    case NS::kChanged:
        Sound(kSoundChanged);
        Relabel();
        break;
    case NS::kNoEffect: {
        Sound(kSoundHover);
        const char* why = NS::Why(mPage, bar);
        std::snprintf(g_popupText, sizeof g_popupText, "%s", why ? why : "");
        g_popupButtons = 1;
        std::snprintf(g_popupB[0], sizeof g_popupB[0], "%s", NpText::Tr("OK"));
        WantPopup(POPUP_REASON);
        break;
    }
    case NS::kAllSettings:
        OpenAll();
        break;
    default:
        break;
    }
}

void VitaSettingsScene::WantPopup(int kind) {
    g_popupKind = (VsPopup)kind;
    g_popupWanted = true;
    mPopup = kind;
}

// ONLINE ACCOUNT (set18; the 1.3 ALL SETTINGS row): the pixel screen's ONLINE page reads the file, so unsaved steps are written first (they would be lost when the file is read again)
void VitaSettingsScene::OpenAll() {
    if (NS::Changed() && !NS::Save()) {
        NS::SaveFailedMessage(g_popupText, sizeof g_popupText);
        g_popupButtons = 1;
        std::snprintf(g_popupB[0], sizeof g_popupB[0], "%s", NpText::Tr("OK"));
        WantPopup(POPUP_SAVE_FAILED);
        return;
    }
    if (!NS::OpenAll()) return;
    mAllFrames = 1;   // the technical screen runs at this frame's end and returns before the next frame: Update reads the file again then
    std::fprintf(stderr, "[vitaset] ONLINE ACCOUNT requested\n");
}

// DONE (the bar, Start or Circle)
void VitaSettingsScene::BeginDone() {
    if (!NS::Changed()) {
        BeginLeave(LEAVE_PAUSE);
        return;
    }
    // 1.3 (live19): in a match the popup is SAVE / DON'T SAVE / KEEP EDITING and never restarts the game: CROWD, LESS SHAKE and REAR TOUCH apply at once, and the note
    // only appears when a row that waits for the next start changed
    // From OPTIONS (front end, no match) a next-start row changed offers SAVE AND START AGAIN instead (np_native_settings.h LeaveAction); the note says so.
    const bool match = !mFrontEnd;
    char note[160];
    NS::LeaveNote(match, note, sizeof note);
    std::snprintf(g_popupText, sizeof g_popupText, "%s%s%s", NpText::Tr("You changed settings and have not saved them."), note[0] ? " " : "", note);
    for (int i = 0; i < NS::LeaveCount(match); ++i) NS::LeaveLabel(i, match, g_popupB[i], sizeof g_popupB[i]);
    if (mFrontEnd && std::getenv("MSC_VITASET_STALE_LOCK") && g_pFEInput) {   // host proof only: a lock whose owner is not a scene, as a leaked one would be
        static char deadOwner;
        g_pFEInput->PushExclusiveInputLock((BaseSceneHandler*)&deadOwner, 77);
        std::fprintf(stderr, "[vitaset] TEST: a stale input lock was pushed\n");
    }
    g_popupButtons = NS::LeaveCount(match);
    WantPopup(POPUP_LEAVE);
}

// the panel slides out, then the pause menu is back (as the game's own options go back, feOptionsSubMenus.cpp)
void VitaSettingsScene::BeginLeave(Leave to) {
    mLeaving = true;
    NpSettingsHelpShow(0, 0, 0, 0);
    mHelpKey = 0;
    mLeaveTo = to;
    mBusy = true;
    FEPresentation* presentation = mFEScene->m_pFEPackage->GetPresentation();
    presentation->SetActiveSlide("menu out", true);
    presentation->Update(0.0f);
    std::fprintf(stderr, "[vitaset] leaving to the %s\n", mFrontEnd ? "OPTIONS screen" : "pause menu");
}

// the popup of this screen: the game's own FEPopupMenu on the in-match overlay stack, as SHPause.cpp pushes its quit popup (np_menu_ui's NpMenuUiShow is for the
// front end's scene stack). The texts are NpMenuUiPrepare's; a press sets g_result when the popup scene is destroyed, a few frames later.
void VitaSettingsScene::ServicePopup() {
    if (mFrontEnd) {
        ServicePopupFrontEnd();
        return;
    }
    if (g_popupWanted) {
        // not while another popup is on screen or a popped one still holds the input lock
        if (g_pOverlayManager->IsOnStack(SCENE_POPUP_MENU)) return;
        if (g_pFEInput && g_pFEInput->m_InputLockDepth > 0) return;
        NpMenuUiPrepare(g_popupText, g_popupButtons > 0 ? g_popupB[0] : 0, g_popupButtons > 1 ? g_popupB[1] : 0, g_popupButtons > 2 ? g_popupB[2] : 0, 0);
        FEPopupMenu* popup = (FEPopupMenu*)g_pOverlayManager->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
        if (!popup) return;
        WorldDarkening::Instance().Fade(100.0f, 1.0f);   // as the pause menu's quit popup
        popup->mControlInput = FE_ALL_PADS;
        popup->mAllPointersActive = true;
        popup->Create(POPUP_NP_TEXT, Function<FnVoidVoid>(Pressed0), Function<FnVoidVoid>(Pressed1), Function<FnVoidVoid>(Pressed2), Function<FnVoidVoid>(PressedBack));
        g_popupObj = popup;
        g_popupWanted = false;
        g_result = -1;
        g_goneFrames = 0;
        std::fprintf(stderr, "[vitaset] popup %d shown\n", (int)g_popupKind);
        return;
    }
    if (g_popupKind == POPUP_NONE) return;
    const bool onStack = g_pOverlayManager->IsOnStack(SCENE_POPUP_MENU);
    const bool ready = g_popupObj && g_popupObj->mFEScene && g_popupObj->mFEScene->mState == FE_SCENE_READY;
    if (g_result < 0 && onStack && ready && g_pFEInput && g_pFEInput->PlatJustPressed(FE_ALL_PADS, 31, true, 0)) {   // Circle: keep editing / close the message
        g_pOverlayManager->Pop();
        WorldDarkening::Instance().Fade(100.0f, 0.0f);
        Sound(kSoundBack);
        g_popupObj = 0;
        const bool wasAsk = g_popupKind == POPUP_RESTART_ASK;
        g_popupKind = POPUP_NONE;
        mPopup = POPUP_NONE;
        if (wasAsk) BeginLeave(LEAVE_PAUSE);   // Circle on the restart question is NOT NOW (the settings are already saved)
        return;
    }
    if (g_result < 0) {
        // a press pops the popup at once and the callback runs when its scene is destroyed; wait for it (never more than 240 frames)
        if (!onStack && ++g_goneFrames > 240) {
            g_popupObj = 0;
            g_popupKind = POPUP_NONE;
            mPopup = POPUP_NONE;
        }
        return;
    }
    const int r = g_result;
    g_result = -1;
    g_popupObj = 0;
    WorldDarkening::Instance().Fade(100.0f, 0.0f);
    FinishPopup(r);
}

// a button of the popup was pressed (r = 0..2): a message just closes; the leave popup does what the button says (np_native_settings.h Apply)
void VitaSettingsScene::FinishPopup(int r) {
    mSwallow = NpPauseSettings::PopupSwallowFrames(mFrontEnd);   // (front end: the press that closed the popup is not read again by this screen)
    const VsPopup kind = g_popupKind;
    g_popupKind = POPUP_NONE;
    mPopup = POPUP_NONE;
    if (kind == POPUP_RESTART_ASK) {   // ux13 (U2): the question after SAVE in a match: START AGAIN (1) or NOT NOW (0 or Circle); the settings are saved either way
        if (r == 1) {
            std::fprintf(stderr, "[vitaset] pause restart: START AGAIN chosen\n");
            NS::Restart();   // returns only when the system refused: then the match goes on and the changes apply at the next start
            std::fprintf(stderr, "[vitaset] pause restart: refused, the changes apply at the next start\n");
        } else {
            std::fprintf(stderr, "[vitaset] pause restart: NOT NOW\n");
        }
        BeginLeave(LEAVE_PAUSE);
        return;
    }
    if (kind != POPUP_LEAVE) return;   // a message: OK
    const NS::LeaveActionId action = NS::LeaveAction(r, !mFrontEnd);
    const bool askRestart = action == NS::kActSave && NS::PauseRestartOffered(!mFrontEnd);   // before Apply: the save clears Changed
    switch (NS::Apply(action)) {   // the live rows are applied by Save
    case NS::kLeft:
        if (askRestart) {
            NS::RestartAskMessage(g_popupText, sizeof g_popupText);
            NS::RestartAskLabel(0, g_popupB[0], sizeof g_popupB[0]);
            NS::RestartAskLabel(1, g_popupB[1], sizeof g_popupB[1]);
            g_popupButtons = 2;
            std::fprintf(stderr, "[vitaset] pause restart: asked\n");
            WantPopup(POPUP_RESTART_ASK);
        } else {
            BeginLeave(LEAVE_PAUSE);
        }
        break;
    case NS::kOutSaveFailed: FailedSave(); break;
    default: break;                                         // KEEP EDITING: the popup is gone, the screen stays
    }
}

// the front end's popup: the game's popup through np_menu_ui.cpp on GameSceneManager (NpMenuUiShow / Poll / Back), the way the online menu flow does it
void VitaSettingsScene::ServicePopupFrontEnd() {
    if (g_popupWanted) {
        static int waited = 0;   // frames this request has waited (NpMenuUiShow answers false while another popup is up, or one that was popped still holds the input lock)
        ++waited;
        if ((waited == 3 || waited % 60 == 0) && g_pFEInput) {
            const int d = (int)g_pFEInput->m_InputLockDepth;
            std::fprintf(stderr, "[vitaset] front-end popup waits (%d frames): input lock depth %d, top owner %p id %d\n", waited, d,
                         d > 0 ? (void*)g_pFEInput->m_nExclusiveInputSceneHashIDStack[d - 1].m_pBaseSceneHandler : 0,
                         d > 0 ? (int)g_pFEInput->m_nExclusiveInputSceneHashIDStack[d - 1].m_customID : -1);
        }
        if (!NpMenuUiOpen()) NpMenuUiReleaseStaleLock(kStaleLockFrames);   // (no popup is on screen: a lock left behind by a gone scene is released, not waited for)
        if (NpMenuUiOpen()) return;   // (a popup of ours that was closed while it still loaded is closed for good by this call; NpMenuUiShow does not service that)
        if (!NpMenuUiShow(g_popupText, g_popupButtons > 0 ? g_popupB[0] : 0, g_popupButtons > 1 ? g_popupB[1] : 0, g_popupButtons > 2 ? g_popupB[2] : 0, 0)) return;   // not yet: retried next frame
        g_popupWanted = false;
        std::fprintf(stderr, "[vitaset] popup %d shown (front end) after %d frame(s)\n", (int)g_popupKind, waited);
        waited = 0;
        g_sinceShown = 0;
        g_readyReported = false;
        return;
    }
    if (g_popupKind == POPUP_NONE) return;
    ++g_sinceShown;
    if (!g_readyReported && NpMenuUiReady()) {
        g_readyReported = true;
        std::fprintf(stderr, "[vitaset] front-end popup ready %d frame(s) after it was pushed\n", g_sinceShown);
    }
    const int r = NpMenuUiPoll();
    if (r == -1) {
        if (NpMenuUiBack()) {   // Circle: keep editing / close the message
            NpMenuUiClose();
            std::fprintf(stderr, "[vitaset] front-end popup closed by Circle (KEEP EDITING)\n");
            Sound(kSoundBack);
            g_popupKind = POPUP_NONE;
            mPopup = POPUP_NONE;
            mSwallow = NpPauseSettings::PopupSwallowFrames(true);
        }
        return;
    }
    if (r == -2) {   // gone without a press
        std::fprintf(stderr, "[vitaset] front-end popup gone without a press\n");
        g_popupKind = POPUP_NONE;
        mPopup = POPUP_NONE;
        mSwallow = NpPauseSettings::PopupSwallowFrames(true);
        return;
    }
    FinishPopup(r);
}

void VitaSettingsScene::FailedSave() {
    NS::SaveFailedMessage(g_popupText, sizeof g_popupText);
    g_popupButtons = 1;
    std::snprintf(g_popupB[0], sizeof g_popupB[0], "%s", NpText::Tr("OK"));
    WantPopup(POPUP_SAVE_FAILED);
}

void VitaSettingsScene::Update(float fDeltaT) {
    if (!mIntroSoundPlayed) {
        mIntroSoundPlayed = true;
        Sound(kSoundIntro);
    }
    BaseSceneHandler::Update(fDeltaT);
    if (!mInitialized) {
        TLSlide* slide = mPresentation->m_currentSlide;
        if (slide->GetCurrentTime() < slide->GetStartTime() + slide->GetDuration()) return;
        InitializePointerButtons();
        mInitialized = true;
        for (int i = 0; i < 4; ++i) GetPointerInstance(i)->SetActiveSlide("waiting", true, false);
    }
    if (mLeaving) {
        for (int i = 0; i < 4; ++i) GetPointerInstance(i)->SetActiveSlide("waiting", true, false);
        TLSlide* slide = mPresentation->m_currentSlide;
        if (!(slide->GetCurrentTime() >= slide->GetStartTime() + slide->GetDuration())) return;
        mLeaving = false;
        if (mFrontEnd) GameSceneManager::Instance()->Push(SCENE_OPTIONS, SCREEN_NOTHING, true);
        else g_pOverlayManager->Push(SCENE_PAUSE, SCREEN_NOTHING, true);
        return;
    }
    if (mAllFrames > 0 && ++mAllFrames > 2) {
        mAllFrames = 0;
        NS::Reload();
        Relabel();
        std::fprintf(stderr, "[vitaset] back from ONLINE ACCOUNT: file read again\n");
    }
    UpdateHelp();
    if (mPopup != POPUP_NONE) {
        ServicePopup();
        return;
    }
    if (mSwallow > 0) {   // front end only: a few frames after a popup closed, no press is read (see NpPauseSettings::PopupSwallowFrames)
        --mSwallow;
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (mFrontEnd && (unsigned int)i != gFEControllerIndex) {   // the front end follows the one controller that is in charge
            GetPointerInstance(i)->SetActiveSlide("waiting", true, false);
            continue;
        }
        if (g_pFEInput->JustPressed((eFEINPUT_PAD)i, 47, true, 0)) {   // Start, and Circle (the menu navigation taps it where there is no back bar)
            Sound(kSoundBack);
            BeginDone();
            return;
        }
        u8 valid = true;
        FEPointerEvent event;
        event.mIndex = i;
        event.mPosition = GetPointerPosition(i, &valid);
        event.mPressed = g_pFEInput->JustPressed((eFEINPUT_PAD)i, 30, true, 0);
        TLComponentInstance* cursor = GetPointerInstance(i);
        for (int j = 0; j < 7; ++j) mButtons[j].HandlePointerEvent(&event);
        if (mPopup != POPUP_NONE || mLeaving) return;
        if (mHoverCounts[i] > 0) cursor->SetActiveSlide("A", true, false);
        else cursor->SetActiveSlide("cursor", true, false);
    }
}
