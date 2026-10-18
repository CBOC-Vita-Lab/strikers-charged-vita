// NpDet.cpp (NP, NPB3): determinism probes for the native port. Not part of the decomp.
//
// The simulation must not depend on the process layout (stack addresses, heap addresses, the garbage in uninitialised memory,
// thread timing). Three env-gated probes around every fixed simulation step (FixedUpdateTask::CallFixedUpdateTasks):
//
//   NP_DET_TRACE=<file>   one line per simulation step, keyed by the step number (FixedUpdateTask::mFrame), so two runs compare step by
//                         step whatever their presented-frame timing was:
//                           <step> <all> <rng> <game> <ball> <team0> <team1> <p0> ... <p9>
//                         all = the game's own FixedUpdateTask::CalculateChecksum() (what the network sync compares), rng = the simulation's
//                         random-number seed (gNetworkRandomSeed), then the pieces it is made of, so the first difference says where to look
//                         (the line is written after the step; `tools/det/steprun.py` prints the first differing step and field).
//   NP_STACK_POISON=<byte> before every step, 60 KB of stack below the caller are overwritten with the byte: an uninitialised local that
//                         is read in the step then sees the pattern, not what the previous step or the renderer left there.
//   NP_DET_NAN=1          print (stderr) the first step at which the ball or a character has a non-finite position or velocity.
//   NP_HEAP_POISON=<byte> (src/NL/nlMemory.cpp) every game-heap allocation starts filled with the byte.
// Without the variables the hooks cost a load and a compare per step.
#include "Game/Ball.h"
#include "Game/Game.h"
#include "Game/AI/Fielder.h"
#include "Game/Player.h"
#include "Game/Task/FixedUpdateTask.h"
#include "Game/Team.h"
#include "NL/nlMain.h"

#ifdef NP_NATIVE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned int gNetworkRandomSeed;

namespace
{
int sPoisonStack = -2;
int sNanWatch = 0;
int sSnapFrom = -1, sSnapTo = -1, sSnapTeam = 0, sSnapPlayer = 4;
FILE* sTrace = 0;
unsigned sSinceFlush = 0;

void NpDetFlushAtExit()
{
    if (sTrace != 0)
    {
        fflush(sTrace);
    }
}

void NpDetInit()
{
    const char* e = getenv("NP_STACK_POISON");
    sPoisonStack = (e != 0 && *e != 0) ? (int)(strtol(e, 0, 0) & 0xFF) : -1;
    const char* nw = getenv("NP_DET_NAN");
    sNanWatch = (nw != 0 && *nw != 0 && *nw != '0') ? 1 : 0;
    if (const char* sn = getenv("NP_DET_SNAP"))   // <from>-<to>[:<team>:<player>]: print that character's state at the end of each of those steps
    {
        int t = 0, pl = 4;
        if (sscanf(sn, "%d-%d:%d:%d", &sSnapFrom, &sSnapTo, &t, &pl) >= 2)
        {
            sSnapTeam = t;
            sSnapPlayer = pl;
        }
    }
    const char* t = getenv("NP_DET_TRACE");
    if (t != 0 && *t != 0)
    {
        sTrace = fopen(t, "w");
        if (sTrace != 0)
        {
            setvbuf(sTrace, 0, _IOFBF, 1 << 16);
            atexit(NpDetFlushAtExit);
        }
    }
}

// Fill the dead stack below this function's caller: from just under this frame down 60000 bytes.
__attribute__((noinline)) void PoisonStack(int value)
{
    char* top = (char*)__builtin_frame_address(0) - 1024;
    memset(top - 60000, value, 60000);
    __asm__ volatile("" : : "r"(top) : "memory");
}

struct CharSnap
{
    int step;
    int moveState;
    int actionState;
    float desiredSpeed, actualSpeed, accel, decel, seekSpeed, moveScale;
    unsigned short desMove, actMove, desFace, actFace;
    float px, py, vx, vy;
};
CharSnap sPrev[10];

void TakeSnap(CharSnap& c, cPlayer* p, int step)
{
    const DetChar& u = p->m_DetChar;
    c.step = step;
    c.moveState = (int)u.m_eMovementState;
    c.actionState = (p->m_eClassType == FIELDER) ? (int)((cFielder*)p)->m_eActionState : -1;
    c.desiredSpeed = u.m_fDesiredSpeed;
    c.actualSpeed = u.m_fActualSpeed;
    c.accel = u.m_fAccel;
    c.decel = u.m_fDecel;
    c.seekSpeed = u.m_fDirectionSeekSpeed;
    c.moveScale = u.m_fMovementScale;
    c.desMove = u.m_aDesiredMovementDirection;
    c.actMove = u.m_aActualMovementDirection;
    c.desFace = u.m_aDesiredFacingDirection;
    c.actFace = u.m_aActualFacingDirection;
    c.px = u.m_v3Position.x;
    c.py = u.m_v3Position.y;
    c.vx = u.m_v3Velocity.x;
    c.vy = u.m_v3Velocity.y;
}

void PrintSnap(const char* tag, const CharSnap& c)
{
    fprintf(stderr, "[det]   %s step %d: move state %d action state %d speed des %g act %g accel %g decel %g seek %g scale %g dir des/act move %u/%u face %u/%u pos %g %g vel %g %g\n", tag,
        c.step, c.moveState, c.actionState, c.desiredSpeed, c.actualSpeed, c.accel, c.decel, c.seekSpeed, c.moveScale, c.desMove, c.actMove, c.desFace, c.actFace, c.px,
        c.py, c.vx, c.vy);
}

bool NotFinite(float f)
{
    return !(f - f == 0.0f);   // NaN and +-inf (no <cmath> needed)
}

// NP_DET_NAN=1: report the first step at which the ball or a character has a non-finite position or velocity (once per object), with the step number
void NanWatch(int phase)
{
    static unsigned char reported[11];
    unsigned step = (unsigned)GetFixedUpdateTask()->GetFrame();
    cBall* b = g_pBall;
    if (!reported[10] && (NotFinite(b->m_v3Position.x) || NotFinite(b->m_v3Position.y) || NotFinite(b->m_v3Position.z)
                          || NotFinite(b->m_v3Velocity.x) || NotFinite(b->m_v3Velocity.y) || NotFinite(b->m_v3Velocity.z)))
    {
        reported[10] = 1;
        fprintf(stderr, "[det] step %u phase %d: BALL non-finite: pos %g %g %g vel %g %g %g state %d (&pos %p)\n", step, phase, b->m_v3Position.x, b->m_v3Position.y,
            b->m_v3Position.z, b->m_v3Velocity.x, b->m_v3Velocity.y, b->m_v3Velocity.z, (int)b->meBallState, (void*)&b->m_v3Position);
    }
    for (int t = 0; t < 2; ++t)
    {
        for (int i = 0; i < 5; ++i)
        {
            cPlayer* p = g_pTeams[t]->m_pPlayers[i];
            const nlVector3& pos = p->m_DetChar.m_v3Position;
            const nlVector3& vel = p->m_DetChar.m_v3Velocity;
            if (!reported[t * 5 + i] && (NotFinite(pos.x) || NotFinite(pos.y) || NotFinite(pos.z) || NotFinite(vel.x) || NotFinite(vel.y) || NotFinite(vel.z)
                                         || NotFinite(p->m_DetChar.m_fActualSpeed) || NotFinite(p->m_DetChar.m_fDesiredSpeed)))
            {
                reported[t * 5 + i] = 1;
                {
                    CharSnap now;
                    TakeSnap(now, p, (int)step);
                    PrintSnap("previous", sPrev[t * 5 + i]);
                    PrintSnap("now     ", now);
                }
                fprintf(stderr, "[det] step %u phase %d: team %d player %d non-finite: pos %g %g %g vel %g %g %g (&pos %p &vel %p player %p)\n", step, phase, t, i, pos.x, pos.y,
                    pos.z, vel.x, vel.y, vel.z, (void*)&pos, (void*)&vel, (void*)p);
            }
        }
    }
}

u32 PlayerChecksum(cPlayer* p)
{
    RunningChecksum c;
    p->ChecksumState(&c);
    return ~c.m_nChecksum;
}
} // namespace

extern "C" void NpDetStepBegin()
{
    if (sPoisonStack == -2)
    {
        NpDetInit();
    }
    if (sPoisonStack >= 0)
    {
        PoisonStack(sPoisonStack);
    }
}

// phase marks inside the step (NP_DET_NAN): 1 AI update, 2 NPC update, 3 pre-physics AI, 4 physics, 5 post-physics AI
extern "C" void NpDetPhase(int phase)
{
    if (sNanWatch)
    {
        NanWatch(phase);
    }
}

extern "C" void NpDetStepEnd()
{
    if (sSnapFrom >= 0)
    {
        int step = (int)GetFixedUpdateTask()->GetFrame();
        if (step >= sSnapFrom && step <= sSnapTo)
        {
            CharSnap c;
            TakeSnap(c, g_pTeams[sSnapTeam]->m_pPlayers[sSnapPlayer], step);
            fprintf(stderr, "[snap] step %d: move %d action %d speed des %.9g act %.9g accel %.9g decel %.9g dirs %u/%u %u/%u pos %.9g %.9g vel %.9g %.9g\n", step, c.moveState, c.actionState,
                c.desiredSpeed, c.actualSpeed, c.accel, c.decel, c.desMove, c.actMove, c.desFace, c.actFace, c.px, c.py, c.vx, c.vy);
        }
    }
    if (sNanWatch)
    {
        NanWatch(99);
        for (int t = 0; t < 2; ++t)
        {
            for (int i = 0; i < 5; ++i)
            {
                TakeSnap(sPrev[t * 5 + i], g_pTeams[t]->m_pPlayers[i], (int)GetFixedUpdateTask()->GetFrame());
            }
        }
    }
    if (sTrace == 0)
    {
        return;
    }
    RunningChecksum game, ball, team0, team1;
    g_pGame->ChecksumState(&game);
    g_pBall->ChecksumState(&ball);
    g_pTeams[0]->ChecksumState(&team0);
    g_pTeams[1]->ChecksumState(&team1);
    fprintf(sTrace, "%u %08x %08x %08x %08x %08x %08x", (unsigned)GetFixedUpdateTask()->GetFrame(), (unsigned)GetFixedUpdateTask()->CalculateChecksum(),
        gNetworkRandomSeed, ~game.m_nChecksum, ~ball.m_nChecksum, ~team0.m_nChecksum, ~team1.m_nChecksum);
    for (int t = 0; t < 2; ++t)
    {
        for (int i = 0; i < 5; ++i)
        {
            fprintf(sTrace, " %08x", (unsigned)PlayerChecksum(g_pTeams[t]->m_pPlayers[i]));
        }
    }
    fputc('\n', sTrace);
    if (++sSinceFlush >= 64)
    {
        fflush(sTrace);
        sSinceFlush = 0;
    }
}
#endif
