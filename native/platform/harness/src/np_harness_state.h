// np_harness_state.h: the game state the harness reads each presented frame, and the functions
// that provide it. Internal to platform/harness. Two providers exist: np_harness_game.cpp (the
// game's own globals, weak symbols + member offsets) and tests/mock_state.cpp (a scripted fake,
// for the unit tests of the script engine).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace NpHarness {

struct SceneStack {
    uint32_t depth = 0;
    int32_t ids[32]{};
    uintptr_t top = 0, topVtable = 0;
    bool operator==(const SceneStack& o) const {
        return depth == o.depth && top == o.top && topVtable == o.topVtable && std::memcmp(ids, o.ids, sizeof ids) == 0;
    }
};

// One live menu button (FEPointerRegion fed a pointer event for pad 0 this frame).
struct Button {
    uintptr_t listener = 0, vtable = 0;
    uint32_t ctx = 0;
    float minX = 0, maxX = 0, minY = 0, maxY = 0, cx = 0, cy = 0;
    bool back = false, hover = false;
};

struct State {
    SceneStack scene, overlay;
    uint32_t taskState = 0;
    float dtMs = 0;             // nlTaskManager::mRealTimeDelta of the last task run, milliseconds
    int32_t mode = -1;          // GameInfoManager::mCurrentMode
    int32_t team[2] = {-1, -1}; // captains (eTeamID) of the current mode's BasicGameInfo
    int32_t stadium = -1;
    uintptr_t game = 0;         // g_pGame (non-null while a match exists)
    int32_t gameState = -1;     // cGame::m_eGameState
    float clock = 0, duration = 0;
    int32_t clockState = -1;
    int32_t score[2]{};
    uint32_t simFrame = 0;      // FixedUpdateTask::mFrame (fixed steps since the match began)
    float simTime = 0;
    bool simStarted = false;
    float timeScale = 0;
    bool paused = false;
    uint32_t retrace = 0, swapMode = 0;
    float ball[3]{};
    uintptr_t ballOwner = 0;
    uint32_t inputLock = 0;
    int nButtons = 0, onCtx = -1;
    int32_t onOff = -1;         // hovered button: its offset inside the top scene handler object
};

// ---- provided by the game reader (np_harness_game.cpp) or a test mock ----
// Fills `s` from the game; `buttons` (optional) gets the live menu buttons, `pointer` (optional,
// two floats) the pointer position. Called on the game thread at a frame end, never concurrently.
void SampleState(State& s, std::vector<Button>* buttons, float* pointer);
void SampleCoreState(State& s);
// Name of the .fen file behind a scene id ("main_menu_v3"), "" if unknown.
std::string SceneName(int32_t id);
// Class name behind a vtable pointer, or the pointer in hex.
std::string ClassName(uintptr_t vtable);
// Reads `size` bytes at `addr` if the whole range is readable. Never faults.
bool SafeRead(uintptr_t addr, void* out, size_t size);
// Address of a named global (MSC_WATCH expressions): the built-in table, then the dynamic symbol table.
bool ResolveSymbol(const char* name, uintptr_t* addr);
// W5-SEED: the script command `launch` (a match started from wherever the script is, the way TitleScene::StartDemoMatch does it:
// game mode, teams, stadium, sidekicks, cheats set in GameInfoManager, then BaseGameSceneManager::PushLoadingScene(true)).
// -1 / empty fields keep what the game has. Returns false (and says why in `msg`) when the game's functions are not linked in.
struct LaunchSpec {
    int mode = 2;              // GameInfoManager::eGameMode: 0 friendly (Domination), 2 demo (AI against AI, no HUD overlays but the HUD), 3 cup, 4 challenge
    int stadium = -1;          // eStadiumID 0..17
    int home = -1, away = -1;  // eTeamID (captain) 0..11
    int hsk[3] = {-1, -1, -1}; // eSidekickID 0..7
    int ask[3] = {-1, -1, -1};
    int powerup = -1, env = -1, player = -1;  // CheatSettings (friendly mode): ePowerupCheat 0..11, eEnvironmentCheat 0..5, ePlayerCheat 0..4
    int human = -1;            // side (0 home, 1 away) the pad 0 plays; -1 keeps the pad sides cleared (nobody plays)
    int skill = -1;            // GameplaySettings::eSkillLevel 0..4 (mUserInfo.mGameplayOptions)
    int time = -1;             // match length in seconds (mUserInfo.mGameplayOptions.GameTime)
    int push = 1;              // 0: only set the state (mode, teams, ...), do not push the loading scene: a later `scene <id>` then finds a plausible game state
};
bool LaunchMatch(const LaunchSpec& spec, std::string& msg);
// W5-SEED: the script command `scene <id> [pop]`: BaseGameSceneManager::Push(id, SCREEN_FORWARD, pop) (SceneList numbers: include/Game/BaseGameSceneManager.h);
// `popscene` pops the top scene. For visiting every front-end screen without walking to it.
bool PushScene(int id, bool popFirst, std::string& msg);
bool PopScene(std::string& msg);
// W5-SEED: the script command `setf <name> <value>` sets one of a few game tuning globals (weather start chances) so that a short match meets the weather that a
// 3-minute match only meets by chance. Only the names SetGlobalFloat knows; false (with a message) for any other or a game without the symbol.
bool SetGlobalFloat(const char* name, float value, std::string& msg);
// A hash of the simulation state (positions, clock, score...), the portable determinism check.
uint64_t SimDigest();
// The game's own whole-simulation checksum (FixedUpdateTask::CalculateChecksum: game state, ball, every character of both
// teams; no pointers in it) and the state of the simulation's random number generator (gNetworkRandomSeed). Returns false
// when there is no match to read. NPB3: the layout-independent determinism probe.
bool DeepDigest(uint32_t* chk, uint32_t* rng);

} // namespace NpHarness
