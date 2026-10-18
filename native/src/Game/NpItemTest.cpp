// NpItemTest.cpp (NP, W5-ITEM): the test-only item hook. Not part of the decomp; see include/np/itemtest.h and docs/wave5-ITEM.md.
//
// MSC_ITEM_TEST=1 switches it on. Off (the default) nothing here runs except the receive of a type-240 deterministic event, which only
// a console with the switch on can have sent.
#include "Game/AI/Fielder.h"
#include "Game/AI/FielderDesireTypes.h"
#include "Game/AI/Powerups.h"
#include "Game/AI/ScriptMachine.h"
#include "Game/AI/AIContext.h"
#include "Game/Ball.h"
#include "Game/DB/CharacterInfo.h"
#include "Game/DetInput.h"
#include "Game/Game.h"
#include "Game/Goalie.h"
#include "Game/InputFrameProvider.h"
#include "Game/InputManager.h"
#include "Game/InputRouter.h"
#include "Game/NetworkPeer.h"
#include "Game/Player.h"
#include "Game/Render/ChainChomp.h"
#include "Game/Render/NPCManager.h"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/Team.h"
#include "NL/nlMath.h"

#ifdef NP_NATIVE
#include "np/itemtest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void NpHarnessAtExit(void (*fn)(void)) __attribute__((weak));

namespace
{
int sEnabled = -1;
int sAimSide = -2;   // MSC_ITEM_SIDE: the side whose controlled fielder `aim` and side -1 mean (-1 = the first local controlled player)

const char* const kNames[21] = { "green", "red", "spiny", "freeze", "banana", "bobomb", "chomp", "mushroom", "star", "mario", "peach", "dk",
    "bowser", "luigi", "daisy", "wario", "waluigi", "bowserjr", "diddy", "yoshi", "petey" };

const char* TypeName(int t)
{
    if (t >= 0 && t < 21)
        return kNames[t];
    return t == -1 ? "none" : "?";
}
const char* SizeName(int s) { return s == POWERUPSIZE_SMALL ? "small" : s == POWERUPSIZE_MEDIUM ? "medium" : s == POWERUPSIZE_LARGE ? "large" : "?"; }

// effect bits of a fielder
enum
{
    FX_FROZEN = 1 << 0, FX_SHRUNK = 1 << 1, FX_STAR = 1 << 2, FX_MUSHROOM = 1 << 3, FX_GIANT = 1 << 4, FX_SUPER = 1 << 5, FX_ELECTRO = 1 << 6,
    FX_FALLEN = 1 << 7, FX_DAZED = 1 << 8, FX_FIRE = 1 << 9, FX_CONFUSED = 1 << 10, FX_SLIPPERY = 1 << 11, FX_STUCK = 1 << 12, FX_GOOEY = 1 << 13,
    FX_SHATTERED = 1 << 14, FX_COUNT = 15
};
// (the game's names: IsFrozen() = DesireFrozen FROZEN_PHOTO, Peach's camera; IsFielderDazed() = FROZEN_ICE, the freeze shell's ice block)
const char* const kFxNames[FX_COUNT] = { "photo", "shrunk", "star", "mushroom", "giant", "super", "electrocuted", "fallen", "iced", "burning",
    "confused", "slippery", "stuck", "gooey", "shattered" };

struct Stats
{
    unsigned given[21], hitsField[21], hitsKeeper[21], spawned[21], used[21];
    unsigned fxStarts[2][FX_COUNT];
};
Stats sStats;
unsigned sHits = 0, sHitsKeeper = 0;
const void* sLastHitTarget[8];   // the log counts a hit once per (item type, target) within 25 simulation steps (contact lasts several steps)
int sLastHitType[8];
unsigned sLastHitStep[8];
unsigned sHitRing = 0;
PowerupBase* sLastObj[25];
int sLastObjType[25];
unsigned sLastFx[2][4];
int sLastSlots[2][2], sLastCounts[2][2];
int sLastChomp = -1;
bool sScanInit = false;

unsigned SimStep()
{
    FixedUpdateTask* t = GetFixedUpdateTask();
    return t ? (unsigned)t->GetFrame() : 0u;
}

bool InMatch() { return g_pGame != 0 && g_pTeams[0] != 0 && g_pTeams[1] != 0 && g_pBall != 0; }

unsigned FielderFx(cFielder* f)
{
    unsigned m = 0;
    if (f->IsFrozen()) m |= FX_FROZEN;
    if (f->IsShrunk()) m |= FX_SHRUNK;
    if (f->IsStarActive()) m |= FX_STAR;
    if (f->IsMushroomActive()) m |= FX_MUSHROOM;
    if (f->IsSuperGrowActive()) m |= FX_GIANT;
    if (f->IsSuperPowerActive()) m |= FX_SUPER;
    if (f->m_eActionState == ACTION_ELECTROCUTION) m |= FX_ELECTRO;
    if (f->IsFallenDown()) m |= FX_FALLEN;
    if (IsFielderDazed(f)) m |= FX_DAZED;
    if (f->m_DetPlayer.m_tFireTimer.m_uPackedTime != 0) m |= FX_FIRE;
    if (f->IsConfused()) m |= FX_CONFUSED;
    if (f->IsSlippery()) m |= FX_SLIPPERY;
    if (f->IsStuck()) m |= FX_STUCK;
    if (f->m_pAIContext != 0 && IsConcurrentStateActive(f->m_pAIContext->mScriptMachine, FIELDER_DESIRE_GOOEY)) m |= FX_GOOEY;
    if (f->IsShattered()) m |= FX_SHATTERED;
    return m;
}

bool IsLocalPad(cPlayer* p)
{
    DetInput* in = p->GetGlobalPad();
    if (in == 0)
        return false;
    NetworkPeerChannel* ch = (NetworkPeerChannel*)in->m_pMyUser;
    return ch == 0 || ch->GetLocalChannelPad() != 0;
}

// The fielder this console's pad controls on `side` (-1: the first one on either side; MSC_ITEM_SIDE fixes the side).
cFielder* Controlled(int side)
{
    if (!InMatch())
        return 0;
    if (side < 0)
    {
        if (sAimSide == -2)
        {
            const char* e = getenv("MSC_ITEM_SIDE");
            sAimSide = (e && (*e == '0' || *e == '1')) ? *e - '0' : -1;
        }
        side = sAimSide;
    }
    for (int t = 0; t < 2; ++t)
    {
        if (side >= 0 && t != side)
            continue;
        for (int i = 0; i < 4; ++i)
        {
            cFielder* f = g_pTeams[t]->GetFielder(i);
            if (f != 0 && IsLocalPad(f))
                return f;
        }
    }
    return 0;
}

int LocalSide()
{
    cFielder* f = Controlled(-1);
    if (f)
        return f->m_pTeam->m_nSide;
    if (InMatch())   // the pad may be on the keeper (he holds the ball): the side of any local controlled player
        for (int t = 0; t < 2; ++t)
            for (int i = 0; i < 5; ++i)
            {
                cPlayer* p = g_pTeams[t]->GetPlayer(i);
                if (p != 0 && IsLocalPad(p) && (sAimSide < 0 || sAimSide == t))
                    return t;
            }
    return -1;
}

float Dist2D(const nlVector3& a, const nlVector3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

cFielder* NearestOpponent(cFielder* me, float* d)
{
    cTeam* other = me->m_pTeam->GetOtherTeam();
    cFielder* best = 0;
    float bd = 1e9f;
    for (int i = 0; i < 4; ++i)
    {
        cFielder* f = other->GetFielder(i);
        if (f == 0)
            continue;
        const float x = Dist2D(me->m_DetChar.m_v3Position, f->m_DetChar.m_v3Position);
        if (x < bd)
        {
            bd = x;
            best = f;
        }
    }
    if (d)
        *d = bd;
    return best;
}

// (side, fielder index 0..3, 4 = keeper) of a player, for the log
void Who(const void* p, int* side, int* idx)
{
    *side = -1;
    *idx = -1;
    if (!InMatch() || p == 0)
        return;
    for (int t = 0; t < 2; ++t)
    {
        for (int i = 0; i < 4; ++i)
            if ((const void*)g_pTeams[t]->GetFielder(i) == p)
            {
                *side = t;
                *idx = i;
                return;
            }
        if ((const void*)g_pTeams[t]->GetGoalie() == p)
        {
            *side = t;
            *idx = 4;
            return;
        }
    }
}

void Summary()
{
    if (sEnabled != 1)
        return;
    fprintf(stderr, "[itemsum] hits on fielders %u, on keepers %u\n", sHits, sHitsKeeper);
    for (int t = 0; t < 21; ++t)
    {
        const Stats& s = sStats;
        if (s.given[t] | s.spawned[t] | s.used[t] | s.hitsField[t] | s.hitsKeeper[t])
            fprintf(stderr, "[itemsum] %-8s given %u used %u objects %u hits fielder %u keeper %u\n", TypeName(t), s.given[t], s.used[t],
                s.spawned[t], s.hitsField[t], s.hitsKeeper[t]);
    }
    for (int side = 0; side < 2; ++side)
    {
        char buf[512];
        int n = 0;
        buf[0] = 0;
        for (int k = 0; k < FX_COUNT; ++k)
            if (sStats.fxStarts[side][k] && n < (int)sizeof buf - 40)
                n += snprintf(buf + n, sizeof buf - n, " %s=%u", kFxNames[k], sStats.fxStarts[side][k]);
        fprintf(stderr, "[itemsum] effects started on side %d:%s\n", side, n ? buf : " none");
    }
    fflush(stderr);
}

void FormatSlots(char* out, size_t size, int side)
{
    cTeam* t = g_pTeams[side];
    PowerUpTeamType a = t->GetPowerUpByIndex(0), b = t->GetPowerUpByIndex(1);
    snprintf(out, size, "%s x%d | %s x%d", TypeName(a.eType), a.nnumOfPowerups, TypeName(b.eType), b.nnumOfPowerups);
}
} // namespace

extern "C" int NpItemTestEnabled(void)
{
    if (sEnabled < 0)
    {
        const char* e = getenv("MSC_ITEM_TEST");
        sEnabled = (e && *e && *e != '0') ? 1 : 0;
        if (sEnabled)
        {
            fprintf(stderr, "[item] MSC_ITEM_TEST=1: item test hook on (give requests ride the deterministic input stream, determ type %d)\n",
                NP_ITEMTEST_DETERM_TYPE);
            memset(&sStats, 0, sizeof sStats);
            if (NpHarnessAtExit)
                NpHarnessAtExit(Summary);
        }
    }
    return sEnabled;
}

extern "C" int NpItemTestQueue(int side, int type, int count, int mode)
{
    if (!NpItemTestEnabled() || !InMatch())
        return 0;
    if (side < 0)
        side = LocalSide();
    if (side < 0 || side > 1 || type < -2 || type > POWER_UP_PETEY || type == -1 || count < 1 || count > 9)
        return 0;
    unsigned char msg[8] = { NP_ITEMTEST_DETERM_TYPE, 'I', 'T', (unsigned char)side, (unsigned char)(signed char)type, (unsigned char)count,
        (unsigned char)mode, 0 };
    GetInputRouter()->QueueDetermData(msg, sizeof msg);
    fprintf(stderr, "[item] sim=%u queue give side=%d %s x%d %s\n", SimStep(), side, type == -2 ? "captain" : TypeName(type), count,
        mode ? "set" : "add");
    return 1;
}

extern "C" void NpItemTestReceive(const unsigned char* data, int size)
{
    if (size < 7 || data[1] != 'I' || data[2] != 'T' || !InMatch())
        return;
    const int side = data[3] & 1;
    int type = (signed char)data[4];
    const int count = data[5];
    const int mode = data[6];
    cTeam* team = g_pTeams[side];
    if (type == -2)
    {
        cFielder* cap = team->GetCaptain();
        type = cap ? (int)cap->m_pCharacterInfo->mCaptainPowerup : -1;
    }
    if (type < 0 || type > POWER_UP_PETEY)
        return;
    if (mode)
        team->ClearAllPowerUps();
    const int ok = team->SetCurrentPowerUp((ePowerUpType)type, count);
    if (sEnabled < 0)
        NpItemTestEnabled();
    if (sEnabled == 1)
        ++sStats.given[type];
    char slots[96];
    FormatSlots(slots, sizeof slots, side);
    fprintf(stderr, "[item] sim=%u give side=%d %s x%d %s: %s, slots %s\n", SimStep(), side, TypeName(type), count, mode ? "set" : "add",
        ok ? "given" : "NOT given (both slots full)", slots);
}

extern "C" void NpItemTestNoteHit(int type, int size, const void* thrower, const void* target, int keeper)
{
    if (sEnabled != 1)
        return;
    const unsigned step = SimStep();
    for (int i = 0; i < 8; ++i)
        if (sLastHitTarget[i] == target && sLastHitType[i] == type && step - sLastHitStep[i] < 25u)
        {
            sLastHitStep[i] = step;
            return;
        }
    sLastHitTarget[sHitRing & 7] = target;
    sLastHitType[sHitRing & 7] = type;
    sLastHitStep[sHitRing & 7] = step;
    ++sHitRing;
    int ts, ti, hs, hi;
    Who(thrower, &ts, &ti);
    Who(target, &hs, &hi);
    if (type >= 0 && type < 21)
    {
        if (keeper)
            ++sStats.hitsKeeper[type];
        else
            ++sStats.hitsField[type];
    }
    if (keeper)
        ++sHitsKeeper;
    else
        ++sHits;
    fprintf(stderr, "[item] sim=%u hit %s (%s) thrown by side%d#%d -> %s side%d#%d\n", SimStep(), TypeName(type), SizeName(size), ts, ti,
        keeper ? "KEEPER" : "fielder", hs, hi);
}

extern "C" void NpItemTestFrame(unsigned long long frame)
{
    if (!NpItemTestEnabled())
        return;
    if (!InMatch())
    {
        sScanInit = false;
        return;
    }
    const unsigned step = SimStep();
    if (!sScanInit)
    {
        memset(sLastObj, 0, sizeof sLastObj);
        for (int i = 0; i < 25; ++i)
            sLastObjType[i] = -1;
        memset(sLastFx, 0, sizeof sLastFx);
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 2; ++k)
            {
                sLastSlots[s][k] = -1;
                sLastCounts[s][k] = 0;
            }
        sLastChomp = -1;
        sScanInit = true;
    }
    // the teams' slots
    for (int s = 0; s < 2; ++s)
    {
        PowerUpTeamType a = g_pTeams[s]->GetPowerUpByIndex(0), b = g_pTeams[s]->GetPowerUpByIndex(1);
        if ((int)a.eType < -1 || (int)a.eType > POWER_UP_PETEY || (int)b.eType < -1 || (int)b.eType > POWER_UP_PETEY || (unsigned)a.nnumOfPowerups > 9u
            || (unsigned)b.nnumOfPowerups > 9u)
            continue;   // the team is not set up yet (pre-game)
        if (a.eType != sLastSlots[s][0] || b.eType != sLastSlots[s][1] || a.nnumOfPowerups != sLastCounts[s][0]
            || b.nnumOfPowerups != sLastCounts[s][1])
        {
            // an item left the front slot (thrown / activated) when the front type changes from X to something not equal to the old
            // second slot, or its count fell
            const int oldA = sLastSlots[s][0];
            if (oldA >= 0 && oldA < 21 && ((a.eType != oldA && a.eType != sLastSlots[s][1]) || (a.eType == oldA && a.nnumOfPowerups < sLastCounts[s][0])
                    || (a.eType == sLastSlots[s][1] && b.eType != oldA)))
                ++sStats.used[oldA];
            char slots[96];
            FormatSlots(slots, sizeof slots, s);
            fprintf(stderr, "[item] f=%llu sim=%u slots side=%d: %s\n", frame, step, s, slots);
            sLastSlots[s][0] = a.eType;
            sLastSlots[s][1] = b.eType;
            sLastCounts[s][0] = a.nnumOfPowerups;
            sLastCounts[s][1] = b.nnumOfPowerups;
        }
    }
    // the live item objects
    for (int i = 0; i < 25; ++i)
    {
        PowerupBase* p = g_pPowerups[i];
        const int type = p ? (int)p->m_eType : -1;
        if (p != sLastObj[i] || type != sLastObjType[i])
        {
            if (sLastObj[i] != 0)
                fprintf(stderr, "[item] f=%llu sim=%u object %d gone: %s\n", frame, step, i, TypeName(sLastObjType[i]));
            if (p != 0)
            {
                int ts, ti;
                Who(p->m_pThrower, &ts, &ti);
                if (type >= 0 && type < 21)
                    ++sStats.spawned[type];
                fprintf(stderr, "[item] f=%llu sim=%u object %d new: %s %s%s thrower side%d#%d at %.1f,%.1f\n", frame, step, i, TypeName(type),
                    SizeName(p->meSize), p->mbExploder ? " exploder" : "", ts, ti, p->m_v3Position.x, p->m_v3Position.y);
            }
            sLastObj[i] = p;
            sLastObjType[i] = type;
        }
    }
    // the Chain Chomp
    const int chomp = (gNPCManager != 0 && gNPCManager->mpChainChomp != 0) ? (gNPCManager->mpChainChomp->IsHidden() ? 0 : 1) : -1;
    if (chomp != sLastChomp)
    {
        if (sLastChomp >= 0 || chomp == 1)
            fprintf(stderr, "[item] f=%llu sim=%u chain chomp %s\n", frame, step, chomp == 1 ? "OUT" : chomp == 0 ? "hidden" : "absent");
        sLastChomp = chomp;
    }
    // effects per fielder
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < 4; ++i)
        {
            cFielder* f = g_pTeams[s]->GetFielder(i);
            const unsigned fx = f ? FielderFx(f) : 0;
            const unsigned on = fx & ~sLastFx[s][i], off = sLastFx[s][i] & ~fx;
            if (on | off)
            {
                char buf[256];
                int n = 0;
                buf[0] = 0;
                for (int k = 0; k < FX_COUNT; ++k)
                {
                    if (on & (1u << k))
                    {
                        n += snprintf(buf + n, sizeof buf - n, " +%s", kFxNames[k]);
                        ++sStats.fxStarts[s][k];
                    }
                    if (off & (1u << k))
                        n += snprintf(buf + n, sizeof buf - n, " -%s", kFxNames[k]);
                }
                fprintf(stderr, "[item] f=%llu sim=%u fx side%d#%d:%s\n", frame, step, s, i, buf);
                sLastFx[s][i] = fx;
            }
        }
}

extern "C" int NpItemTestAim(int target, int* sx, int* sy)
{
    *sx = *sy = 0;
    cFielder* me = Controlled(-1);
    if (me == 0)
        return 0;
    const nlVector3& p = me->m_DetChar.m_v3Position;
    nlVector3 q = p;
    cTeam* other = me->m_pTeam->GetOtherTeam();
    switch (target)
    {
    case 1:
    {
        cPlayer* owner = g_pBall->m_pOwner;
        if (owner != 0 && owner->m_pTeam == other)
        {
            q = owner->m_DetChar.m_v3Position;
            break;
        }
    }
    // fall through: no opponent has the ball
    case 0:
    {
        cFielder* f = NearestOpponent(me, 0);
        if (f)
            q = f->m_DetChar.m_v3Position;
        break;
    }
    case 2:
        q = me->m_pTeam->GetAIOffNetLocation(&p);
        break;
    case 3:
        if (other->GetGoalie())
            q = other->GetGoalie()->m_DetChar.m_v3Position;
        break;
    default:
        q = g_pBall->m_v3Position;
        break;
    }
    const float dx = q.x - p.x, dy = q.y - p.y;
    if (dx * dx + dy * dy < 1e-4f)
        return 1;
    DetInput* in = me->GetGlobalPad();
    const unsigned short remap = in ? in->m_aRemapAngle : gInputManager->mFrameProvider->GetInputRemapAngle();
    const unsigned short world = (unsigned short)(int)(atan2f(dy, dx) * 10430.378f);   // the game's angle units (65536 per turn)
    float x, y;
    nlPolarToCartesian(x, y, (unsigned short)(world - remap), 72.0f);
    static int flipY = -1;
    if (flipY < 0)
    {
        const char* e = getenv("MSC_ITEM_AIM_FLIPY");
        flipY = (e && *e == '1') ? 1 : 0;
    }
    *sx = (int)lrintf(x);
    *sy = (int)lrintf(flipY ? -y : y);
    return 1;
}

extern "C" int NpItemTestVar(const char* name, double* out)
{
    static const char* const kVars[] = { "item0", "item0b", "itemn0", "itemn0b", "item1", "item1b", "itemn1", "itemn1b", "pups", "chomp", "fx0",
        "fx1", "hits", "hitk", "own", "side", "ctl", "dopp", "downer", "dgoal", "dkeep", "dball", "itemme", "itemmeb", "itemnme" };
    bool known = false;
    for (unsigned i = 0; i < sizeof kVars / sizeof kVars[0] && !known; ++i)
        known = !strcmp(name, kVars[i]);
    if (!known)
        return 0;
    *out = -1;
    if (!InMatch())
        return 1;
    if (!strcmp(name, "itemme") || !strcmp(name, "itemmeb") || !strcmp(name, "itemnme"))
    {
        // this console's own team (the side of its controlled player): LAN scripts run the same on host and joiner
        const int side = LocalSide();
        if (side < 0)
            return 1;
        PowerUpTeamType t = g_pTeams[side]->GetPowerUpByIndex(name[6] == 'b' ? 1 : 0);
        *out = name[4] == 'n' ? t.nnumOfPowerups : (double)t.eType;
        return 1;
    }
    if (!strncmp(name, "item", 4))
    {
        // item0 item0b itemn0 itemn0b item1 item1b itemn1 itemn1b
        const char* p = name + 4;
        const bool count = *p == 'n';
        if (count)
            ++p;
        const int side = *p == '1' ? 1 : 0;
        const int slot = p[1] == 'b' ? 1 : 0;
        PowerUpTeamType t = g_pTeams[side]->GetPowerUpByIndex(slot);
        *out = count ? t.nnumOfPowerups : (double)t.eType;
        return 1;
    }
    if (!strcmp(name, "pups"))
    {
        int n = 0;
        for (int i = 0; i < 25; ++i)
            n += g_pPowerups[i] != 0;
        *out = n;
        return 1;
    }
    if (!strcmp(name, "chomp"))
    {
        *out = (gNPCManager && gNPCManager->mpChainChomp) ? (gNPCManager->mpChainChomp->IsHidden() ? 0 : 1) : -1;
        return 1;
    }
    if (!strcmp(name, "fx0") || !strcmp(name, "fx1"))
    {
        unsigned m = 0;
        for (int i = 0; i < 4; ++i)
            if (cFielder* f = g_pTeams[name[2] - '0']->GetFielder(i))
                m |= FielderFx(f);
        *out = m;
        return 1;
    }
    if (!strcmp(name, "hits"))
    {
        *out = sHits;
        return 1;
    }
    if (!strcmp(name, "hitk"))
    {
        *out = sHitsKeeper;
        return 1;
    }
    if (!strcmp(name, "own"))
    {
        cPlayer* o = g_pBall->m_pOwner;
        *out = o ? o->m_pTeam->m_nSide : -1;
        return 1;
    }
    cFielder* me = Controlled(-1);
    if (!strcmp(name, "side"))
    {
        *out = me ? me->m_pTeam->m_nSide : -1;
        return 1;
    }
    if (!strcmp(name, "ctl"))
    {
        *out = -1;
        if (me)
            for (int i = 0; i < 4; ++i)
                if (me->m_pTeam->GetFielder(i) == me)
                    *out = i;
        return 1;
    }
    if (me == 0)
        return 1;
    const nlVector3& p = me->m_DetChar.m_v3Position;
    cTeam* other = me->m_pTeam->GetOtherTeam();
    if (!strcmp(name, "dopp"))
    {
        float d;
        NearestOpponent(me, &d);
        *out = d;
    }
    else if (!strcmp(name, "downer"))
    {
        cPlayer* o = g_pBall->m_pOwner;
        *out = (o && o->m_pTeam == other) ? Dist2D(p, o->m_DetChar.m_v3Position) : 999.0;
    }
    else if (!strcmp(name, "dgoal"))
    {
        nlVector3 g = me->m_pTeam->GetAIOffNetLocation(&p);
        *out = Dist2D(p, g);
    }
    else if (!strcmp(name, "dkeep"))
        *out = other->GetGoalie() ? Dist2D(p, other->GetGoalie()->m_DetChar.m_v3Position) : 999.0;
    else if (!strcmp(name, "dball"))
        *out = Dist2D(p, g_pBall->m_v3Position);
    else
        return 0;
    return 1;
}
#endif
