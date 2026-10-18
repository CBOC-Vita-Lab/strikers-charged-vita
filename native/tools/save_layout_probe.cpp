// Probe for tools/gen_save_layout.py: includes the save-related headers so that
// `clang++ -fsyntax-only -Xclang -fdump-record-layouts` prints the host layout of every struct the game's save files contain.
#include <revolution/os.h>
#include "Game/DB/BasicGameInfo.h"
#include "Game/DB/Cup.h"
#include "Game/DB/GameProgress.h"
#include "Game/DB/PlayerStats.h"
#include "Game/DB/StatsTracker.h"
#include "Game/DB/UserOptions.h"
#include "Game/GameInfo.h"

// instantiate the three cup templates the game saves
template struct Cup<4, 8>;
template struct Cup<6, 12>;
template struct Cup<10, 11>;
Cup<4, 8> g_probe_fire;
Cup<6, 12> g_probe_crystal;
Cup<10, 11> g_probe_striker;
UserInfo g_probe_user;
CupProgressRecord g_probe_progress;
ChallengeUnlockRecord g_probe_challenge;
GameInfoSaveSlot g_probe_slot;
PlayerStats g_probe_stats;
TeamStats g_probe_team;
BasicGameInfo g_probe_game;
CupHistoryRecord g_probe_history_record;
CupHistory g_probe_history;
static_assert(sizeof(CupManager) > 0, "");
BaseCup* g_probe_basecup_ptr;
