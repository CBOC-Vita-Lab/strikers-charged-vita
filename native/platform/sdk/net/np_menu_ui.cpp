// The game's own popup as the screen of the online menu flow (np_menu_flow.cpp). One popup at a time: a message and one to three buttons, in the look of
// every other popup of the game (FEPopupMenu, POPUP_NP_TEXT). The flow never touches the game's classes; it talks to NpMenuUi* below.
//
// A popup is pushed on the scene stack, so its buttons and its pointer work exactly as in the rest of the front end (Cross presses the highlighted
// button, the D-pad / stick moves between them). The popup holds the game's exclusive input lock while it is open, so Circle (cancel) is read straight
// from the pad (PlatJustPressed), the way the movie player reads its skip button. The text of an open popup can be replaced (NpMenuUiSetText): the box
// keeps its size, so a status popup is opened with a message as tall as the tallest status it will show (NpMenuUiShow pads it).
#include "platform/sdk/net/np_menu_flow.h"
#include "platform/sdk/net/np_lan.h"
#include "platform/sdk/net/np_pause_settings.h"
#include "platform/sdk/include/np/np_text.h"
#include "platform/sdk/net/np_mpbeta.h"

#include <stdio.h>
#include <string.h>

#include "Game/BaseGameSceneManager.h"
#include "Game/FE/feInput.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/FE/feScene.h"
#include "Game/GameSceneManager.h"
#include "NL/nlBind.h"
#include "NL/nlFunction.h"
#include "NL/nlFunction.inl"
#include "NL/nlString.h"

namespace {

const int kMaxText = 480;       // UTF-16 units of the message (the popup's own buffer is 1024)
const int kMaxButton = 40;

unsigned short g_message[kMaxText + 1];
unsigned short g_button[3][kMaxButton + 1];
int g_buttons = 0;
int g_highlight = 0;
bool g_wide = true;

FEPopupMenu* g_popup = 0;
bool g_open = false;        // we pushed a popup and have not seen it go
int g_result = -1;          // the button pressed (set by the popup's callback when it is destroyed)
int g_goneFrames = 0;       // frames since the popup left the scene stack without a result
int g_frames = 0;
unsigned g_openedFrame = 0;
bool g_closePending = false;    // the popup is to be closed but its scene is still loading (see NpMenuUiClose)
int g_closeWaited = 0;

// (1.3) every text of these popups is the port's own: in the game's language (np_text.h; sentence by sentence, so a reason followed by the
// flow's own "Press Circle to cancel." reads whole), then UTF-8 to the game's UTF-16. English texts are unchanged.
void ToWide(const char* in, unsigned short* out, int capacity) {
    static char translated[kMaxText * 3 + 1];
    NpText::Sentences(NpText::CurrentLanguage(), in ? in : "", translated, sizeof translated);
    NpText::ToUtf16(translated, out, capacity);
}

void Pressed0() { g_result = 0; }
void Pressed1() { g_result = 1; }
void Pressed2() { g_result = 2; }
void PressedBack() {}

bool PopupOnStack() { return GameSceneManager::Instance() && GameSceneManager::Instance()->IsOnStack(SCENE_POPUP_MENU); }

// Another popup of the game (an error, a confirmation) is on screen: ours waits for it.
bool TopIsPopup() {
    GameSceneManager* m = GameSceneManager::Instance();
    return m && m->GetCurrentScene() && m->GetSceneType(m->GetCurrentScene()) == SCENE_POPUP_MENU;
}

// A popup scene loads its resources asynchronously (FEResourceManager). Popping it before it is ready leaves the resource manager with a dangling
// scene (it marks the dead scene ready when the load ends: a crash, or silent corruption, a few frames later). The online menu closes a status popup the
// moment the service answers, which on a fast link is a few frames after it was opened: so a close waits for the scene to be ready.
bool PopupReady() { return g_popup && g_popup->mFEScene && g_popup->mFEScene->mState == FE_SCENE_READY; }

// A popup that was popped holds the game's input lock until its scene is destroyed, a few frames later (FEPopupMenu::~FEPopupMenu pops the top entry of the
// lock stack, whoever owns it). A popup pushed in that gap takes a lock above the dead one, the destructor then pops the NEW popup's entry and leaves the
// dead one on top: the new popup never has input focus (Cross does nothing) and the stack leaks an entry. So a new popup waits until the top lock's owner is
// gone from the scene stack (the game's own "connection lost" popup is followed at once by RECONNECT / LEAVE in the lobby).
bool LockOwnedByGoneScene() {
    if (!g_pFEInput || g_pFEInput->m_InputLockDepth <= 0) return false;
    BaseSceneHandler* owner = g_pFEInput->m_nExclusiveInputSceneHashIDStack[g_pFEInput->m_InputLockDepth - 1].m_pBaseSceneHandler;
    GameSceneManager* m = GameSceneManager::Instance();
    if (!owner || !m) return false;
    for (unsigned long i = 0; i < m->mCurrentStackDepth; ++i)
        if (m->mBaseSceneHandlerStack[i] == owner) return false;
    return true;
}

void DoClose() {
    if (g_open && PopupOnStack() && TopIsPopup()) GameSceneManager::Instance()->Pop();
    g_open = false;
    g_popup = 0;
    g_result = -1;
    g_closePending = false;
    g_closeWaited = 0;
}

void ServiceClose() {
    if (!g_closePending) return;
    if (!g_open || !PopupOnStack()) { DoClose(); return; }
    if (PopupReady() || ++g_closeWaited > 900) DoClose();   // (a scene that never becomes ready for 900 frames is closed anyway)
}

}  // namespace

// ---- what FEPopupMenu::Create reads (fePopupMenu.cpp) ----------------------------------------------------------------------------------------
const unsigned short* NpPopupMessage() { return g_message; }
const unsigned short* NpPopupOption(int index) { return index >= 0 && index < 3 ? g_button[index] : g_button[0]; }
int NpPopupOptionCount() { return g_buttons; }
int NpPopupHighlight() { return g_highlight; }
bool NpPopupWide() { return g_wide; }

static unsigned short g_lostText[kMaxText + 1];
const unsigned short* NpPopupConnectionLostText() {
    char text[kMaxText];
    if (!NpMenuConnectionLostText(text, sizeof text)) return 0;
    ToWide(text, g_lostText, kMaxText);
    return g_lostText;
}

// (mpbeta27) the sync-error popup (a desync: the lockstep's checksums differ) in plain words, and that a report will be offered; the game's own text when MSC_MP_BETA=0
static unsigned short g_syncWide[kMaxText + 1];
const unsigned short* NpPopupSyncErrorText() {
    if (!NpMpBeta::On()) return 0;
    ++NpMpBeta::Count().syncText;
    ToWide(NpMpBeta::SyncErrorText(), g_syncWide, kMaxText);
    return g_syncWide;
}

extern "C" void NpOnlineGetNameRejectMessage(char* out, size_t size) __attribute__((weak));

static unsigned short g_badNameWide[kMaxText + 1];
const unsigned short* NpPopupBadNameText() {
    if (!NpOnlineGetNameRejectMessage) return 0;
    char text[kMaxText];
    text[0] = 0;
    NpOnlineGetNameRejectMessage(text, sizeof text);
    if (!text[0]) return 0;
    NpText::ToUtf16(text, g_badNameWide, kMaxText);
    return g_badNameWide;
}

static unsigned short g_noGamesWide[kMaxText + 1];
const unsigned short* NpPopupNoGamesText() {
    const char* text = NpLanNoGamesText();
    if (!text || !*text) return 0;
    ToWide(text, g_noGamesWide, kMaxText);
    return g_noGamesWide;
}

// ---- the flow's screen ---------------------------------------------------------------------------------------------------------------------
int NpMenuUiSceneReady(int sceneId) {
    GameSceneManager* m = GameSceneManager::Instance();
    if (!m || !m->GetCurrentScene()) return false;
    return (int)m->GetSceneType(m->GetCurrentScene()) == sceneId;
}

// The texts a popup of kind POPUP_NP_TEXT reads (NpPopupMessage / NpPopupOption...), set without pushing anything: a scene that pushes the popup itself (the native
// settings screen's leave popup) calls this first. NpMenuUiShow is this plus the push.
void NpMenuUiPrepare(const char* text, const char* b0, const char* b1, const char* b2, int highlight) {
    ToWide(text, g_message, kMaxText);
    g_buttons = 0;
    const char* labels[3] = {b0, b1, b2};
    for (int i = 0; i < 3; ++i)
        if (labels[i]) ToWide(labels[i], g_button[g_buttons++], kMaxButton);
    g_highlight = highlight >= 0 && highlight < g_buttons ? highlight : 0;
    g_wide = true;
}

int NpMenuUiShow(const char* text, const char* b0, const char* b1, const char* b2, int highlight) {
    GameSceneManager* m = GameSceneManager::Instance();
    if (!m || g_open || TopIsPopup()) return false;
    static int lockWaited = 0;
    if (LockOwnedByGoneScene() && ++lockWaited < 300) return false;   // see LockOwnedByGoneScene (never more than 300 frames)
    lockWaited = 0;
    NpMenuUiPrepare(text, b0, b1, b2, highlight);
    FEPopupMenu* popup = (FEPopupMenu*)m->Push(SCENE_POPUP_MENU, SCREEN_NOTHING, false);
    if (!popup) return false;
    popup->Create(POPUP_NP_TEXT, Function<FnVoidVoid>(Pressed0), Function<FnVoidVoid>(Pressed1), Function<FnVoidVoid>(Pressed2),
                  Function<FnVoidVoid>(PressedBack));
    g_popup = popup;
    g_open = true;
    g_result = -1;
    g_goneFrames = 0;
    g_openedFrame = (unsigned)g_frames;
    return true;
}

void NpMenuUiSetText(const char* text) {
    if (!g_open || !g_popup || !PopupOnStack()) return;
    ToWide(text, g_message, kMaxText);
    g_popup->NpSetMessage(g_message);
}

// -1 open, 0..2 the button pressed, -2 gone without a press (closed by the flow, or by the scene changing).
int NpMenuUiPoll() {
    ++g_frames;
    ServiceClose();
    if (!g_open) return -2;
    if (g_result >= 0) {
        const int r = g_result;
        g_result = -1;
        g_open = false;
        g_popup = 0;
        return r;
    }
    if (!PopupOnStack()) {
        // the button press pops the scene at once and the callback runs when the scene is destroyed, a few frames later
        if (++g_goneFrames > 240) {
            g_open = false;
            g_popup = 0;
            return -2;
        }
    } else {
        g_goneFrames = 0;
    }
    return -1;
}

// A lock entry whose owner is gone from the scene stack is normally released by the owner's destructor within a few frames (see LockOwnedByGoneScene). One that is still there
// after `staleFrames` calls in a row has no destructor coming: it is popped here, so the next popup is not held back for the 300 frames NpMenuUiShow would wait.
int NpMenuUiReleaseStaleLock(int staleFrames) {
    static BaseSceneHandler* lastOwner = 0;
    static int lastDepth = 0, frames = 0;
    if (!LockOwnedByGoneScene()) { lastOwner = 0; frames = 0; return 0; }
    BaseSceneHandler* owner = g_pFEInput->m_nExclusiveInputSceneHashIDStack[g_pFEInput->m_InputLockDepth - 1].m_pBaseSceneHandler;
    if (owner != lastOwner || g_pFEInput->m_InputLockDepth != lastDepth) { lastOwner = owner; lastDepth = g_pFEInput->m_InputLockDepth; frames = 0; }
    ++frames;
    if (!NpPauseSettings::StaleLockExpired(true, frames, staleFrames)) return 0;
    g_pFEInput->PopExclusiveInputLock(owner);
    fprintf(stderr, "[menu-ui] a stale input lock (owner gone for %d frames) was released\n", frames);
    lastOwner = 0;
    frames = 0;
    return 1;
}

int NpMenuUiReady() { return g_open && PopupOnStack() && PopupReady(); }

int NpMenuUiOpen() {
    ServiceClose();
    return g_open || TopIsPopup();
}

void NpMenuUiClose() {
    if (g_open && PopupOnStack() && !PopupReady()) {   // still loading: closed by NpMenuUiOpen / NpMenuUiPoll as soon as it is ready
        g_closePending = true;
        return;
    }
    DoClose();
}

// Circle, once per press (PlatJustPressed is the edge). Read from the pad itself: the popup owns the game's input lock.
int NpMenuUiBack() { return g_pFEInput && g_pFEInput->PlatJustPressed(FE_ALL_PADS, 31, true, 0); }
