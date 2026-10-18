// layout_check.cpp: asserts, against the game's own headers, the structure offsets that
// platform/harness/src/np_harness_game.cpp reads by number (the Wii/MWCC offsets of the recompiled
// build's harness). A failed static_assert names the offset that is different in the native build:
// fix the constant in np_harness_game.cpp (and in the table of np/docs/vita.md).
//   make -C platform/harness layout        (uses the game's flags: np/Makefile CXXFLAGS)
// Members are mostly public; the few private ones are reached with the usual test-only trick.
#define private public
#define protected public
#include "Game/BaseGameSceneManager.h"
#include "Game/GameSceneManager.h"
#include "Game/OverlayManager.h"
#include "NL/nlTask.h"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/Game.h"
#include "Game/Sys/clock.h"
#include "Game/Team.h"
#include "Game/Ball.h"
#include "Game/GameInfo.h"
#include "Game/FE/feInput.h"
#include "Game/FE/fePointer.h"
#include "Game/FE/fePointerManager.h"
#include "Game/SH/SHGameplayOptions.h"
#include "Game/SH/SHChooseCaptains.h"
#include "Game/SH/SHChooseSidekicks.h"
#include "Game/SH/SHChooseSides.h"
#include "Game/SH/SHStadiumSelect.h"
#undef private
#undef protected
#include <stddef.h>

#define CHECK_OFFSET(type, member, expected) static_assert(offsetof(type, member) == (expected), #type "::" #member " is not at " #expected)
#define CHECK_SIZE(type, expected) static_assert(sizeof(type) == (expected), "sizeof(" #type ") is not " #expected)

// ---- scene and overlay stacks: depth +0x04, scene ids +0x08, handlers +0x88 (np_harness_game.cpp ReadStack)
CHECK_OFFSET(BaseGameSceneManager, mCurrentStackDepth, 0x04);
CHECK_OFFSET(BaseGameSceneManager, m_sceneStack, 0x08);
CHECK_OFFSET(BaseGameSceneManager, mBaseSceneHandlerStack, 0x88);
// ---- task manager: state +0x08, real time step +0x18
CHECK_OFFSET(nlTaskManager, mCurrentState, 0x08);
CHECK_OFFSET(nlTaskManager, mRealTimeDelta, 0x18);
// ---- fixed update task: time +0x2C, frame +0x34, started +0x38, time scale +0x60
CHECK_OFFSET(FixedUpdateTask, mSimulationTime, 0x2C);
CHECK_OFFSET(FixedUpdateTask, mFrame, 0x34);
CHECK_OFFSET(FixedUpdateTask, mSimulationStarted, 0x38);
CHECK_OFFSET(FixedUpdateTask, mTimeScale, 0x60);
// ---- the match: clock pointer +0x08, state +0x18, duration +0x1C; clock timer +0x08; team score +0x04
CHECK_OFFSET(cGame, m_pGameClock, 0x08);
CHECK_OFFSET(cGame, m_eGameState, 0x18);
CHECK_OFFSET(cGame, m_fGameDuration, 0x1C);
CHECK_OFFSET(Clock, m_fTimer, 0x08);
CHECK_OFFSET(cTeam, m_nScore, 0x04);
// ---- ball: position +0x54, owner +0xC8
CHECK_OFFSET(cBall, m_v3Position, 0x54);
CHECK_OFFSET(cBall, m_pOwner, 0xC8);
// ---- game info: current mode +0x11C, per-mode info pointers +0x80; info: teams +0, stadium +0x20
CHECK_OFFSET(GameInfoManager, mCurrentMode, 0x11C);
CHECK_OFFSET(GameInfoManager, mGameInfo, 0x80);
CHECK_OFFSET(BasicGameInfo, mTeamIndex, 0x00);
CHECK_OFFSET(BasicGameInfo, mStadiumIndex, 0x20);
// ---- front-end input lock +0x20; pointer listeners (menu buttons)
CHECK_OFFSET(FEInput, m_InputLockDepth, 0x24);   // native: 0x24 (the Wii header says 0x20; found by this check, 9 Oct)
CHECK_OFFSET(FEPointerListener, mPreviousEvents, 0x3C);
CHECK_OFFSET(FEPointerListener, mContext, 0x7C);
CHECK_OFFSET(FEPointerListener, mDisabled, 0x80);
CHECK_OFFSET(FEPointerListener, mIgnoreInputLock, 0x81);
CHECK_OFFSET(FEPointerRegion, mMinX, 0x84);
CHECK_OFFSET(FEPointerRegion, mMaxX, 0x88);
CHECK_OFFSET(FEPointerRegion, mMaxY, 0x8C);
CHECK_OFFSET(FEPointerRegion, mMinY, 0x90);
CHECK_OFFSET(FEPointerRegion, mRotation, 0x94);
CHECK_OFFSET(FEPointerRegion, mPivot, 0x98);
// the list the pointer manager walks: head pointer at +0x04 of the manager, nodes {next, data}
typedef ListContainerBase<FEPointerListener*, NewAdapter<ListEntry<FEPointerListener*> > > PointerList;
static_assert(offsetof(FEPointerManager, mListeners) == 0x00, "listener list moved");
CHECK_OFFSET(PointerList, m_Head, 0x04);   // the harness reads the first node at manager + 4
CHECK_OFFSET(ListEntry<FEPointerListener*>, next, 0x00);
CHECK_OFFSET(ListEntry<FEPointerListener*>, entry, 0x04);
// ---- the nav scripts' `onoff` values: byte offsets of the hovered button inside its scene handler
// (tools/scripts/nav_match60.txt waits on these; the recompiled build reported the Wii's layout)
CHECK_OFFSET(SHGameplayOptions, mDoneButton, 0x1470);
CHECK_OFFSET(ChooseCaptainsSceneV2, mSelectButtons, 0x920);
CHECK_OFFSET(ChooseCaptainsSceneV2, mDoneButton, 0xBF0);
static_assert(offsetof(ChooseCaptainsSceneV2, mCaptainButtons) + 2 * sizeof(FEPointerButton) == 0x218, "captain button 2");
static_assert(offsetof(ChooseCaptainsSceneV2, mCaptainButtons) + 3 * sizeof(FEPointerButton) == 0x2CC, "captain button 3");
static_assert(offsetof(ChooseCaptainsSceneV2, mCaptainButtons) + 4 * sizeof(FEPointerButton) == 0x380, "captain button 4");
static_assert(offsetof(ChooseCaptainsSceneV2, mSelectButtons) + sizeof(FEPointerButton) == 0x9D4, "select button 1");
CHECK_OFFSET(ChooseSidekicksSceneV2, mDoneButton, 0x11C8);
CHECK_OFFSET(SHChooseSides2, mControllerComponents, 0x20);
CHECK_OFFSET(SHChooseSides2, mHomeAwayComponent, 0x188);
CHECK_OFFSET(StadiumSelectScene, mPlayButton, 0x110);
int main() { return 0; }
