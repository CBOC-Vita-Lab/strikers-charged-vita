namespace
{
static void* sTriggerByteCode;
}

#ifdef NP_NATIVE
#include "np/dvd_keep.h"
#include <stdlib.h>

// MSC_NIS_PREFETCH=0|1 (default 1; it does nothing unless the disc layer's keep cache is on, MSC_DVD_KEEP_KIB). Called when a match first loads a cut-scene (the walk-out): the files its goals,
// Mega Strikes, losers' reactions and end-of-game sequence will ask for are handed to the keep cache, so that after kick-off the disc layer answers them from memory. Only names are looked up:
// no game state is read that the simulation writes, no random number is drawn, and nothing is read on this thread.
void NisPlayer::PrefetchMatchFiles()
{
    static const bool on = [] { const char* e = getenv("MSC_NIS_PREFETCH"); return !(e && e[0] == '0'); }();
    static unsigned sSignature = 0, sDrops = 0;
    if (!on || mDictSize <= 0)
    {
        return;
    }
    GameInfoManager* info = GameInfoManager::Instance();
    if (info == 0 || info->GetCurrentGameInfo() == 0 || info->GetTeam(0) < 0 || info->GetTeam(1) < 0)
    {
        return;   // no match is set up yet
    }
    const char* names[9];
    int count = 0;
    names[count++] = GetTeamName((eTeamID)info->GetTeam(0));
    names[count++] = GetTeamName((eTeamID)info->GetTeam(1));
    for (int side = 0; side < 2; ++side)
    {
        for (int slot = 0; slot < 3; ++slot)
        {
            names[count++] = GetSidekickName((eSidekickID)info->GetSidekick(side, slot));
        }
    }
    names[count++] = "goalie";
    unsigned signature = 2166136261u;
    for (int i = 0; i < count; ++i)
    {
        for (const char* c = names[i] ? names[i] : ""; *c; ++c)
        {
            signature = (signature ^ (unsigned char)*c) * 16777619u;
        }
        signature = (signature ^ '|') * 16777619u;
    }
    const unsigned drops = NpDvdKeepDrops();
    if (signature == sSignature && drops == sDrops)
    {
        return;   // the same line-ups as the last call and nothing dropped from the cache since: already asked for (a drop, or another line-up in between, asks again)
    }
    sSignature = signature;
    sDrops = drops;
    NpDvdKeepBegin();
    // what the game asks for once a match is on (see docs/TECHNICAL.md MSC_NIS_PREFETCH), in the order play needs it: the goal and Mega Strike scenes first, then the defence HUD, the
    // sidekicks' own animation files (the scenes load them by name as they start), then the losers' reactions and the end of the game. Not the captains' introductions, the stadium
    // flyby or the cup trophies, and of the "_<sidekick>_other" variants only those of the sidekicks that play.
    static const char* const kPhases[][3] = { { "_goal_winner", "_megastrike", 0 }, { "_outraged", "_defeated", "_riotguard" }, { "_end_of_game", 0, 0 } };
    int hinted = 0;
    for (int phase = 0; phase < 3; ++phase)
    {
        if (phase == 1)
        {
            hinted += NpDvdKeepHint("art/fe/defensive_play.fen");
            for (int n = 2; n < count - 1; ++n)
            {
                char dir[64];
                nlSNPrintf(dir, sizeof(dir), "art/animation/%s", names[n] ? names[n] : "");
                hinted += NpDvdKeepHintDir(dir);
            }
        }
        for (int i = 0; i < mDictSize; ++i)
        {
            const char* name = mDict[i].name;
            bool wanted = false;
            if (phase == 0 && nlStrNICmp(name, "megastrike", 10) == 0)
            {
                wanted = true;
            }
            if (phase == 2 && nlStrNICmp(name, "end_of_game", 11) == 0)
            {
                wanted = true;
            }
            for (int n = 0; !wanted && n < count; ++n)
            {
                const int length = names[n] ? (int)nlStrLen(names[n]) : 0;
                if (length == 0 || nlStrNICmp(name, names[n], length) != 0 || name[length] != '_')
                {
                    continue;
                }
                for (int k = 0; k < 3 && kPhases[phase][k]; ++k)
                {
                    if (strstr(name, kPhases[phase][k]) != NULL)
                    {
                        wanted = true;
                        break;
                    }
                }
            }
            const char* other = wanted ? strstr(name, "_other") : NULL;
            if (other != NULL)
            {
                // "..._toad_other": the sidekick named just before the suffix must be one that plays
                const char* start = other;
                while (start > name && start[-1] != '_')
                {
                    --start;
                }
                bool plays = false;
                for (int n = 2; n < count - 1 && !plays; ++n)
                {
                    const int length = names[n] ? (int)nlStrLen(names[n]) : 0;
                    plays = length > 0 && (other - start) == length && nlStrNICmp(start, names[n], length) == 0;
                }
                wanted = plays;
            }
            if (wanted)
            {
                char path[96];
                nlSNPrintf(path, sizeof(path), "art/nis/%s", name);
                hinted += NpDvdKeepHint(path);
            }
        }
    }
    unsigned files = 0, bytes = 0, hits = 0, misses = 0;
    const int enabled = NpDvdKeepStats(&files, &bytes, &hits, &misses);
    fprintf(stderr, "[nisprefetch] %d cut-scene files handed to the keep cache for %s, %s, %s (cache %s: %u files, %u KiB held so far)\n", hinted, names[0], names[1], names[count - 1],
            enabled ? "on" : "off", files, bytes / 1024u);
}
#endif

const char* NisPlayer::GetTargetFilter(NisTarget target, NisWinnerType winnerType) const
{
    if (target == NIS_TARGET_STADIUM)
    {
        int stadium = GameInfoManager::Instance()->GetStadium();
        const char* stadiumName = GetStadiumName(stadium);
        return stadiumName;
    }

    if (target == NIS_TARGET_HOME_CAPTAIN)
    {
        return GetTeamName((eTeamID)GameInfoManager::Instance()->GetTeam(0));
    }

    if (target == NIS_TARGET_AWAY_CAPTAIN)
    {
        return GetTeamName((eTeamID)GameInfoManager::Instance()->GetTeam(1));
    }

    if (target == NIS_TARGET_HOME_SIDEKICK)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(0, 0));
    }

    if (target == NIS_TARGET_HOME_SIDEKICK_1)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(0, 0));
    }

    if (target == NIS_TARGET_HOME_SIDEKICK_2)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(0, 1));
    }

    if (target == NIS_TARGET_HOME_SIDEKICK_3)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(0, 2));
    }

    if (target == NIS_TARGET_AWAY_SIDEKICK)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(1, 0));
    }

    if (target == NIS_TARGET_AWAY_SIDEKICK_1)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(1, 0));
    }

    if (target == NIS_TARGET_AWAY_SIDEKICK_2)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(1, 1));
    }

    if (target == NIS_TARGET_AWAY_SIDEKICK_3)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(1, 2));
    }

    if (target == NIS_TARGET_SCORER)
    {
        return g_pCharacters[mGoalScorerCharIndex]->m_pCharacterInfo->mName;
    }

    if (target == NIS_TARGET_WINNER_SIDEKICK)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick((short)GetWinnerSide(winnerType), 0));
    }

    if (target == NIS_TARGET_LOSER_SIDEKICK)
    {
        int side = (GetWinnerSide(winnerType) + 1) % 2;
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick((short)side, 0));
    }

    if (target == NIS_TARGET_WINNER_CAPTAIN)
    {
        return GetTeamName((eTeamID)GameInfoManager::Instance()->GetTeam((short)GetWinnerSide(winnerType)));
    }

    if (target == NIS_TARGET_LOSER_CAPTAIN)
    {
        int side = (GetWinnerSide(winnerType) + 1) % 2;
        return GetTeamName((eTeamID)GameInfoManager::Instance()->GetTeam((short)side));
    }

    if (target == NIS_TARGET_MEGASTRIKE_CAPTAIN)
    {
        return GetTeamName((eTeamID)GameInfoManager::Instance()->GetTeam((short)mMegaStrikeSide));
    }

    if (target == NIS_TARGET_HOME_GOALIE || target == NIS_TARGET_AWAY_GOALIE || target == NIS_TARGET_WINNER_GOALIE || target == NIS_TARGET_LOSER_GOALIE || target == NIS_TARGET_MEGASTRIKE_DEFENDING_GOALIE)
    {
        return "goalie";
    }

    if (target == NIS_TARGET_AWAY_SIDEKICK)
    {
        return GetSidekickName((eSidekickID)GameInfoManager::Instance()->GetSidekick(1, 0));
    }

    return "";
}

static inline void FormatNisName(char* fullName, const char* filter, const char* nisType, NisUseFilter useFilter, const char* extraNameFilter)
{
    char prefix[64];
    if (nlStrCmp(filter, "") != 0)
    {
        nlSNPrintf(prefix, sizeof(prefix), "%s_", filter);
    }
    else
    {
        prefix[0] = '\0';
    }

    char extra[64];
    if (useFilter != NIS_NO_FILTER)
    {
        nlSNPrintf(extra, sizeof(extra), "_%s", extraNameFilter);
    }
    else
    {
        extra[0] = '\0';
    }

    nlSNPrintf(fullName, 64, "%s%s%s", prefix, nisType, extra);
}

static inline int RandomNisIndex(int count, unsigned int* seed)
{
    float value = (count - 1) * nlRandomf(1.0f, seed);
    value += value < 0.0f ? -0.5f : 0.5f;
    return (int)value;
}

static inline void PlayNisCue(NisPlayer* player, const char* nisName)
{
    char cueName[128];
    nlStrNCpy(cueName, nisName, sizeof(cueName));
    unsigned long length = nlStrLen(cueName);
    if (GetStadiumUnknown0x10(GameInfoManager::Instance()->GetStadium()))
    {
        cueName[length - 4] = '\0';
    }
    else
    {
        nlStrNCpy(cueName + length - 4, "_nocrowd", sizeof(cueName) - length - 4);
    }
    player->PrepareNisCue(nlStringLowerHash(cueName));
}

void NisPlayer::Load(const char* nisType, NisTarget target, NisUseStadiumOffset useStadiumOffset, NisUseFilter useFilter, NisWinnerType winnerType, int renderMode, int variantIndex)
{
    char fullName[64];
    mActive = true;
#ifdef NP_NATIVE
    PrefetchMatchFiles();
#endif

    const char* filter = GetTargetFilter(target, winnerType);
    FormatNisName(fullName, filter, nisType, useFilter, mExtraNameFilter);

    int numAvailableNis = 0;
    NisHeader* availableNis[10] = { 0 };
    int dictionaryIndex;
    for (dictionaryIndex = 0; dictionaryIndex < mDictSize && numAvailableNis < 10; dictionaryIndex++)
    {
        if (nlStrNICmp(mDict[dictionaryIndex].name, fullName, nlStrLen(fullName)) != 0)
        {
            continue;
        }
        NisHeader* candidate = &mDict[dictionaryIndex];
        if (strstr(candidate->name, "_same") != NULL)
        {
            continue;
        }
        if (strstr(candidate->name, "_other") != NULL)
        {
            continue;
        }
        availableNis[numAvailableNis++] = candidate;
    }

    if (numAvailableNis == 0)
    {
        return;
    }

    int index;
    if (variantIndex >= 0)
    {
        index = variantIndex % numAvailableNis;
    }
    else
    {
        index = RandomNisIndex(numAvailableNis, &GetPresentation()->mRandomSeed);
        if (DuringGoalCelebration(GetPresentation()) && renderMode == NIS_RENDER_PRIMARY)
        {
            if (numAvailableNis > 1 && mLastCelebrationIndex == index && nlStrCmp(mLastCelebrationFilter, mExtraNameFilter) == 0)
            {
                while (index == mLastCelebrationIndex)
                {
                    index = RandomNisIndex(numAvailableNis, &GetPresentation()->mRandomSeed);
                }
            }
            mLastCelebrationIndex = index;
            nlStrNCpy(mLastCelebrationFilter, mExtraNameFilter, sizeof(mLastCelebrationFilter));
        }
    }

    NisHeader& nisHeader = *availableNis[index];
    QueueNisLoad(nisHeader, target, useStadiumOffset, winnerType, renderMode, false);

    NisTarget sameTarget = NIS_TARGET_NONE;
    NisTarget otherTarget = NIS_TARGET_NONE;
    switch (target)
    {
    case NIS_TARGET_HOME_CAPTAIN:
    case NIS_TARGET_HOME_GOALIE:
        sameTarget = NIS_TARGET_HOME_SIDEKICK;
        otherTarget = NIS_TARGET_AWAY_SIDEKICK;
        break;
    case NIS_TARGET_AWAY_CAPTAIN:
    case NIS_TARGET_AWAY_GOALIE:
        sameTarget = NIS_TARGET_AWAY_SIDEKICK;
        otherTarget = NIS_TARGET_HOME_SIDEKICK;
        break;
    case NIS_TARGET_LOSER_CAPTAIN:
    case NIS_TARGET_LOSER_GOALIE:
        sameTarget = NIS_TARGET_LOSER_SIDEKICK;
        otherTarget = NIS_TARGET_WINNER_SIDEKICK;
        break;
    case NIS_TARGET_WINNER_CAPTAIN:
    case NIS_TARGET_WINNER_GOALIE:
        sameTarget = NIS_TARGET_WINNER_SIDEKICK;
        otherTarget = NIS_TARGET_LOSER_SIDEKICK;
        break;
    case NIS_TARGET_SCORER:
    {
        cCharacter* character = g_pCharacters[mGoalScorerCharIndex];
        if (character != NULL && character->IsCaptain())
        {
            if (((cPlayer*)character)->m_pTeam->m_nSide == 0)
            {
                sameTarget = NIS_TARGET_HOME_SIDEKICK;
                otherTarget = NIS_TARGET_AWAY_SIDEKICK;
            }
            else
            {
                sameTarget = NIS_TARGET_AWAY_SIDEKICK;
                otherTarget = NIS_TARGET_HOME_SIDEKICK;
            }
        }
        break;
    }
    case NIS_TARGET_MEGASTRIKE_DEFENDING_GOALIE:
    {
        cCharacter* character = g_pCharacters[mGoalScorerCharIndex];
        if (character != NULL && character->IsCaptain())
        {
            if (((cPlayer*)character)->m_pTeam->m_nSide == 0)
            {
                sameTarget = NIS_TARGET_AWAY_SIDEKICK;
                otherTarget = NIS_TARGET_HOME_SIDEKICK;
            }
            else
            {
                sameTarget = NIS_TARGET_HOME_SIDEKICK;
                otherTarget = NIS_TARGET_AWAY_SIDEKICK;
            }
        }
        break;
    }
    case NIS_TARGET_MEGASTRIKE_CAPTAIN:
    {
        cCharacter* character = g_pCharacters[mGoalScorerCharIndex];
        if (character != NULL && character->IsCaptain())
        {
            if (((cPlayer*)character)->m_pTeam->m_nSide == 0)
            {
                sameTarget = NIS_TARGET_HOME_SIDEKICK;
                otherTarget = NIS_TARGET_AWAY_SIDEKICK;
            }
            else
            {
                sameTarget = NIS_TARGET_AWAY_SIDEKICK;
                otherTarget = NIS_TARGET_HOME_SIDEKICK;
            }
        }
        break;
    }
    }

    if (sameTarget != NIS_TARGET_NONE)
    {
        LoadRelatedNis(nisHeader.name, "same", sameTarget, useStadiumOffset, winnerType, nisHeader.mirrored, renderMode);
    }
    if (otherTarget != NIS_TARGET_NONE)
    {
        LoadRelatedNis(nisHeader.name, "other", otherTarget, useStadiumOffset, winnerType, nisHeader.mirrored, renderMode);
    }

    if (renderMode != NIS_RENDER_SECONDARY && mPreparedNisCue == 0)
    {
        PlayNisCue(this, nisHeader.name);
    }
}

void NisPlayer::LoadRelatedNis(const char* nisName, const char* relation, NisTarget target, NisUseStadiumOffset useStadiumOffset, NisWinnerType winnerType, bool mirrored, int renderMode)
{
    char baseName[64];
    int length = nlStrChr(nisName, '.') - nisName + 1;
    nlStrNCpy(baseName, nisName, nlMin((int)sizeof(baseName), length));

    const char* filter = GetTargetFilter(target, winnerType);
    char fullName[64];
    nlSNPrintf(fullName, sizeof(fullName), "%s_%s_%s.nis", baseName, filter, relation);

    NisHeader* nisHeader = NULL;
    for (int dictionaryIndex = 0; dictionaryIndex < mDictSize; dictionaryIndex++)
    {
        if (nlStrCmp(mDict[dictionaryIndex].name, fullName) == 0)
        {
            mDict[dictionaryIndex].mirrored = mirrored;
            nisHeader = &mDict[dictionaryIndex];
            break;
        }
    }

    if (nisHeader != NULL)
    {
        QueueNisLoad(*nisHeader, target, useStadiumOffset, winnerType, renderMode, true);
    }
}

void NisPlayer::QueueNisLoad(NisHeader& nisHeader, NisTarget target, NisUseStadiumOffset useStadiumOffset, NisWinnerType winnerType, int renderMode, bool preserveMirroring)
{
    nisHeader.target = target;
    nisHeader.winnerType = winnerType;
    nisHeader.mTime = 0.0f;
    nisHeader.renderMode = renderMode;
    if (!preserveMirroring)
    {
        nisHeader.mirrored = IsMirrored(target, nisHeader.name, winnerType);
    }
    if (useStadiumOffset == NIS_NO_STADIUM_OFFSET)
    {
        nisHeader.stadiumOffset.x = 0.0f;
        nisHeader.stadiumOffset.y = 0.0f;
        nisHeader.stadiumOffset.z = 0.0f;
    }
    else
    {
        float scale = (useStadiumOffset == NIS_AWAY_STADIUM_OFFSET) ? -1.0f : 1.0f;
        char offsetConfigName[64];
        nlSNPrintf(offsetConfigName, 64, "nisHeader/%s_offset", GetStadiumName(GameInfoManager::Instance()->GetStadium()));
        float offset = scale * GetConfigFloat(Config::Global(), offsetConfigName, 0.0f);
        nisHeader.stadiumOffset.x = 0.0f;
        nisHeader.stadiumOffset.y = offset;
        nisHeader.stadiumOffset.z = 0.0f;
    }
    for (int i = 0; i < nisHeader.numAnimations; i++)
    {
        mBeginPositions[i] = nisHeader.beginPositions[i];
        if (nisHeader.mirrored)
        {
            mBeginPositions[i].x *= -1.0f;
        }
    }
    if (nisHeader.buffer != NULL)
    {
        Load(nisHeader.buffer, nisHeader.bufferSize, nisHeader);
    }
    else
    {
        for (int i = 0; i < 8; i++)
        {
            if (mLoadQueue[i] == NULL)
            {
                mLoadQueue[i] = &nisHeader;
                break;
            }
        }
    }
}
