// np_save_game.cpp: ties the save codec to the game's structures (compile-time size checks) and offers the hooks
// SaveLoad.cpp calls. Compiled with the game (PLAT_DIRS).
#include <revolution/os.h>
#include <revolution/nand/NANDBanner.h>

#include "Game/DB/Cup.h"
#include "Game/DB/GameProgress.h"
#include "Game/DB/StatsTracker.h"
#include "Game/GameInfo.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/sdk/save/np_save_codec.h"
#include "platform/sdk/save/np_save_game.h"

// The codec's tables are generated from these layouts (tools/gen_save_layout.py); a header change that moves them must
// regenerate np_save_layout.inc (tools/save_layouts.sh) or these fail.
static_assert(sizeof(UserInfo) == 0x80, "UserInfo");
static_assert(sizeof(BasicGameInfo) == 0x128, "BasicGameInfo");
static_assert(sizeof(TeamStats) == 0x70, "TeamStats");
static_assert(sizeof(CupProgressRecord) == 0x37C, "CupProgressRecord");
static_assert(sizeof(ChallengeUnlockRecord) == 0x34, "ChallengeUnlockRecord");
static_assert(sizeof(GameInfoSaveSlot) == 0xB10, "GameInfoSaveSlot");
static_assert(sizeof(((Cup<4, 8>*)0)->mGameInfo) == 16 * 0x128, "Fire cup match table");
static_assert(sizeof(((Cup<6, 12>*)0)->mGameInfo) == 36 * 0x128, "Crystal cup match table");
static_assert(sizeof(((Cup<10, 11>*)0)->mGameInfo) == 55 * 0x128, "Striker cup match table");
static_assert(sizeof(((Cup<4, 8>*)0)->mTeamStats) == 4 * 0x70, "Fire cup teams");
static_assert(sizeof(((Cup<6, 12>*)0)->mTeamStats) == 6 * 0x70, "Crystal cup teams");
static_assert(sizeof(((Cup<10, 11>*)0)->mTeamStats) == 10 * 0x70, "Striker cup teams");
static_assert(sizeof(((GameInfoManager*)0)->mSaveSlots) == 10 * 0xB10, "friend slots");
// Field offsets of the records the codec walks, the Wii values (the decomp's offset comments); the sizes above do not notice two fields of equal size
// swapping places. Adapted from yannicksuter/mscharged-port (CC0): src/platform/save_data.cpp.
static_assert(offsetof(TeamStats, mPlayerTotalStats) == 0x1C, "TeamStats::mPlayerTotalStats");
static_assert(offsetof(PlayerStats, mBallPossessionTime) == 0x3C, "PlayerStats::mBallPossessionTime");
static_assert(sizeof(PlayerStats) == 0x54, "PlayerStats");
static_assert(offsetof(CupProgressRecord, mHistory) == 0x10, "CupProgressRecord::mHistory");
static_assert(sizeof(CupHistory) == 0x36C && offsetof(CupHistory, mWriteIndex) == 0x360, "CupHistory");
static_assert(sizeof(CupHistoryRecord) == 8, "CupHistoryRecord");
static_assert(__builtin_offsetof(CupManager, mCupRecord) - __builtin_offsetof(CupManager, mState) == 0x20, "CupManager::mCupRecord");
static_assert(__builtin_offsetof(CupManager, mCurrentMode) - __builtin_offsetof(CupManager, mState) + sizeof(int) == 0x3A0, "CupManager progress span");

namespace {
unsigned long expected_size(int online) { return online ? NpSaveOnlinePayloadSize() : NpSaveSettingsPayloadSize(); }
bool size_ok(int online, unsigned long size) {
    if (size == expected_size(online)) return true;
    static int warned = 0;
    if (warned++ < 4)
        fprintf(stderr, "[save] payload of %lu bytes, the layout is for %lu: NOT converted (regenerate platform/sdk/save/np_save_layout.inc)\n",
                size, expected_size(online));
    return false;
}
}  // namespace

extern "C" {

void NpSavePayloadToWii(void* payload, int online, unsigned long size) {
    if (!size_ok(online, size)) return;
    if (online) NpSaveOnlineToWii(payload); else NpSaveSettingsToWii(payload);
}

void NpSavePayloadFromWii(void* payload, int online, unsigned long size) {
    if (!size_ok(online, size)) return;
    if (online) NpSaveOnlineFromWii(payload); else NpSaveSettingsFromWii(payload);
}

const void* NpSaveBannerShadow(const void* hostBanner) {
    static unsigned char* shadow = 0;
    if (!shadow) shadow = (unsigned char*)malloc(NP_SAVE_BANNER_SIZE);
    NpSaveBannerToWii(hostBanner, shadow);
    return shadow;
}

}  // extern "C"
