// test_menu_nav.cpp: NpMenuNav against REAL game objects (FEPointerButton / FEBackButton registered in a real FEPointerManager).
// Linked with the game's objects by tools/nps2_menutest.sh; this file provides main() instead of the game's. The "game" side of
// the loop is emulated: each frame the pointer position follows the cursor (feDPD: x * w/2, -y * h/2) and every region receives the
// event of that position, which is what makes a button live for the navigation.
#define private public   // as np_menu_nav.cpp: FEPointerRegion keeps its geometry private
#include "Game/FE/fePointer.h"
#undef private

#include "Game/FE/feBackButton.h"
#include "Game/FE/feDPD.h"
#include "Game/FE/feInput.h"
#include "Game/FE/fePointerButton.h"
#include "Game/FE/fePointerManager.h"
#include "Game/GameSceneManager.h"
#include "NL/gl/glStruct.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "platform/sdk/input/np_game_input.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

#define CHECK_BTN(n)                                                                                 \
    do {                                                                                             \
        const int v_ = on_button();                                                                  \
        ++g_checks;                                                                                  \
        if (v_ != (n)) {                                                                             \
            printf("FAIL %s:%d: the pointer is on button %d, wanted %d\n", __FILE__, __LINE__, v_, (n)); \
            g_fail++;                                                                                \
        }                                                                                            \
    } while (0)

static FEPointerButton* g_btn[3][3];
static FEBackButton* g_back;
static float g_cursor[2] = {0, 0};
static NpMenuNav::Result g_last;
static int g_pressA_frames = 0;

static void emulate_game_pointer() {
    // the game's DPD task: the pointer is where the cursor says; every region gets the event of that position
    gl_ScreenInfo* info = glGetScreenInfo();
    float hw = 320.0f, hh = 240.0f;
    if (info && info->ScreenWidth > 100 && info->ScreenHeight > 100) { hw = info->ScreenWidth * 0.5f; hh = info->ScreenHeight * 0.5f; }
    gFEPointerPositions[0].x = g_cursor[0] * hw;
    gFEPointerPositions[0].y = -g_cursor[1] * hh;
    FEPointerEvent ev;
    ev.mIndex = 0;
    ev.mPosition = gFEPointerPositions[0];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) g_btn[r][c]->mPreviousEvents[0] = ev;
    g_back->mPreviousEvents[0] = ev;
}

static void frame(uint32_t dir = 0, NpPoint stick = {0, 0}, bool back = false, int page = 0) {
    g_last = NpMenuNav::Update(0, dir, stick, back, page, g_cursor, 1.0f / 30.0f);
    if (g_last.pressA) ++g_pressA_frames;
    emulate_game_pointer();
}
static void frames(int n) { for (int i = 0; i < n; ++i) frame(); }

// which grid button contains the pointer (-1 none, 9 = BACK)
static int on_button() {
    nlVector2 p = gFEPointerPositions[0];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (g_btn[r][c]->ContainsPoint(p)) return r * 3 + c;
    if (g_back->ContainsPoint(p)) return 9;
    return -1;
}

int main() {
    // ---- the game side
    static FEPointerManager mgr;
    g_pFEPointerManager = &mgr;
    static unsigned char inputBuf[sizeof(FEInput)];
    memset(inputBuf, 0, sizeof inputBuf);
    g_pFEInput = (FEInput*)inputBuf;
    static unsigned char gsmBuf[sizeof(GameSceneManager)] __attribute__((aligned(16)));
    memset(gsmBuf, 0, sizeof gsmBuf);
    GameSceneManager* gsm = (GameSceneManager*)gsmBuf;
    nlSingleton<GameSceneManager>::s_pInstance = gsm;
    static char dummyScene[64];
    gsm->mCurrentStackDepth = 1;
    gsm->m_sceneStack[0] = SCENE_MAIN_MENU;
    gsm->mBaseSceneHandlerStack[0] = (BaseSceneHandler*)dummyScene;

    // a 3 x 3 grid of 120 x 50 buttons (centre origin, +y up), and a BACK button in the corner
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            g_btn[r][c] = new FEPointerButton((void*)0);
            float cx = -150.0f + 150.0f * c, cy = 100.0f - 100.0f * r;
            g_btn[r][c]->SetBounds(cx - 60, cx + 60, cy + 25, cy - 25);
        }
    g_back = new FEBackButton();
    g_back->SetBounds(-330, -250, -180, -220);   // not lined up with the left column (a lined-up BACK is a valid wrap target)
    gFEPointerPositions[0].x = gFEPointerPositions[0].y = 0.0f;
    emulate_game_pointer();

    // 1. a menu just appeared: the first button in reading order (top row, leftmost) is selected
    frames(12);
    CHECK(g_last.menu == false || g_last.menu == true);
    CHECK_BTN(0);

    // 2. RIGHT moves along the row; the second press moves again
    frame(NpMenuNav::kRight); frames(8);
    CHECK_BTN(1);
    frames(3);
    frame(NpMenuNav::kRight); frames(8);
    CHECK_BTN(2);
    // past the end of the row it wraps to the far end of the same row
    frames(3);
    frame(NpMenuNav::kRight); frames(8);
    CHECK_BTN(0);

    // 3. DOWN moves down the column, UP back, and the column wraps too
    frames(3);
    frame(NpMenuNav::kDown); frames(8);
    CHECK_BTN(3);
    frames(3);
    frame(NpMenuNav::kDown); frames(8);
    CHECK_BTN(6);
    frames(3);
    frame(NpMenuNav::kDown); frames(8);
    CHECK_BTN(0);
    frames(3);
    frame(NpMenuNav::kUp); frames(8);
    CHECK_BTN(6);

    // 4. a stick flick works like the D-pad (the caller turns it into a direction)
    frames(3);
    frame(NpMenuNav::kRight, {1.0f, 0.0f}); frames(8);
    CHECK_BTN(7);

    // 5. L / R page: the first frame of a press taps -/+
    frames(3);
    frame(0, {0, 0}, false, 1);
    CHECK(g_last.page == 1);
    frames(8);
    frame(0, {0, 0}, false, -1);
    CHECK(g_last.page == -1);
    frames(8);

    // 6. B goes to BACK and taps A there
    g_pressA_frames = 0;
    frame(0, {0, 0}, true);
    frame(0, {0, 0}, true);
    bool tapped = false;
    for (int i = 0; i < 12; ++i) { frame(); tapped = tapped || g_last.pressA; }
    CHECK(tapped);
    CHECK_BTN(9);

    // 7. the focus memory: leaving BACK with a direction returns into the grid
    frames(6);
    frame(NpMenuNav::kUp); frames(10);
    CHECK(on_button() >= 0 && on_button() <= 8);

    // 8. input locked (a popup owns the input): buttons without mIgnoreInputLock are not live, so navigation sees no menu
    g_pFEInput->m_InputLockDepth = 1;
    frames(4);
    CHECK(g_last.menu == false);
    g_pFEInput->m_InputLockDepth = 0;
    frames(8);
    CHECK(g_last.menu == true);

    printf("test_menu_nav: %d checks, %d failed\n", g_checks, g_fail);
    fflush(stdout);
    _exit(g_fail ? 1 : 0);   // not exit(): the game's static destructors (its allocators) crash after main when the game was never initialised
}
