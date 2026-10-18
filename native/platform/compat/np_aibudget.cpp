// The single-player AI script time budget on a CPU that is not the Wii's: see include/np/aibudget.h.
#include "np/aibudget.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__arm__)
#include <unistd.h>
#include <sys/syscall.h>
#define NP_AI_HAVE_INSN 1
#endif

extern "C" {
unsigned NpInterpOps;
unsigned NpInterpCalls;
}

extern "C" {
int NpAiMarksOn;
}

namespace {
enum Mode { kRetail, kOff, kSlow, kOps, kInsn };
Mode sMode = kRetail;
bool sStats = false;
bool sInit = false;
double sParam = 1.0;           // slow: K; ops/insn: R (thousand per ms)
NpAiClockFn sReal = 0;

// Clocks that count work. Base moved at every update end keeps the float small (ms within one update).
double sBaseWork = 0.0;
uint64_t sInsnTotal = 0;
uint32_t sInsnLast = 0;
bool sInsnBad = false;

#ifdef NP_AI_HAVE_INSN
inline void Mark(long op) { syscall(0x7f01, op); }
#else
inline void Mark(long) {}
#endif

double InterpWork() { return (double)NpInterpOps + 8.0 * (double)NpInterpCalls; }

#ifdef NP_AI_HAVE_INSN
double InsnWork()
{
    // syscall 0x7f01: the tools/pipe qemu plugin answers with the low 32 bits of the calling vCPU's instruction count (units of one instruction)
    long r = syscall(0x7f01);
    if (r == -1 && errno == ENOSYS)
    {
        sInsnBad = true;
        return sInsnTotal;
    }
    uint32_t now = (uint32_t)r;
    sInsnTotal += (uint32_t)(now - sInsnLast);   // wrap-safe
    sInsnLast = now;
    return (double)sInsnTotal;
}
#else
double InsnWork() { sInsnBad = true; return 0.0; }
#endif

float SlowClock() { return sReal() / (float)sParam; }
float OpsClock() { return (float)((InterpWork() - sBaseWork) / (sParam * 1000.0)); }
float InsnClock() { return (float)((InsnWork() - sBaseWork) / (sParam * 1000.0)); }

// ---- statistics --------------------------------------------------------------------------------------------------------------
const int kScales = 7;
const double kScale[kScales] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0 };
const int kBuckets = 200;      // 0.025 ms each, up to 5 ms of script time per update
struct Stats
{
    uint64_t updates, checks, rejects;
    double sumP[kScales];             // expected skips if the clock ran kScale[i] times as fast (retail rule), summed over the checks
    uint64_t satUpdates[kScales];     // updates whose script time * s reached the budget (chance floor 0.2)
    uint64_t hist[kBuckets + 1];
    double sumAcc, maxAcc;
    uint64_t updChecks;               // checks in the update in progress
    double updChecksMax;
    uint64_t checksTotalInUpdates;
} S;

void Print(const char* why)
{
    if (!S.updates)
        return;
    fprintf(stderr, "[aibudget] %s: mode=%s param=%g updates=%llu checks=%llu (%.1f per update) skipped=%llu (%.2f %%) script time per update: mean %.3f max %.3f ms\n",
        why, sMode == kRetail ? "retail" : sMode == kOff ? "off" : sMode == kSlow ? "slow" : sMode == kOps ? "ops" : "insn", sParam,
        (unsigned long long)S.updates, (unsigned long long)S.checks, (double)S.checks / (double)S.updates, (unsigned long long)S.rejects,
        S.checks ? 100.0 * (double)S.rejects / (double)S.checks : 0.0, S.sumAcc / (double)S.updates, S.maxAcc);
    uint64_t acc = 0;
    double q[3] = { 0.5, 0.9, 0.99 }, qv[3] = { 0, 0, 0 };
    int qi = 0;
    for (int b = 0; b <= kBuckets && qi < 3; b++)
    {
        acc += S.hist[b];
        while (qi < 3 && (double)acc >= q[qi] * (double)S.updates)
            qv[qi++] = (b + 1) * 0.025;
    }
    fprintf(stderr, "[aibudget]   script time per update (upper bound of its 0.025 ms bucket): p50 %.3f p90 %.3f p99 %.3f ms\n", qv[0], qv[1], qv[2]);
    for (int i = 0; i < kScales; i++)
        fprintf(stderr, "[aibudget]   clock x%.2f: retail rule would skip %.2f %% of the scripts, %.1f %% of the updates reach the budget\n", kScale[i],
            S.checks ? 100.0 * S.sumP[i] / (double)S.checks : 0.0, 100.0 * (double)S.satUpdates[i] / (double)S.updates);
    fflush(stderr);
}

void PrintAtExit() { Print("exit"); }

void Init()
{
    if (sInit)
        return;
    sInit = true;
    const char* e = getenv("MSC_AI_BUDGET");
    if (e && *e)
    {
        if (!strcmp(e, "off") || !strcmp(e, "-1"))
            sMode = kOff;
        else if (!strncmp(e, "slow:", 5))
        {
            sMode = kSlow;
            sParam = atof(e + 5);
            if (!(sParam > 0.0))
                sParam = 1.0;
        }
        else if (!strncmp(e, "ops:", 4))
        {
            sMode = kOps;
            sParam = atof(e + 4);
            if (!(sParam > 0.0))
                sParam = 1.0;
        }
        else if (!strncmp(e, "insn:", 5))
        {
            sMode = kInsn;
            sParam = atof(e + 5);
            if (!(sParam > 0.0))
                sParam = 230.0;
        }
        else if (strcmp(e, "retail") != 0)
            fprintf(stderr, "[aibudget] unknown MSC_AI_BUDGET=%s (retail, off, slow:K, ops:R, insn:R): retail\n", e);
        sStats = true;
    }
    const char* m = getenv("MSC_AI_MARKS");      // armhf under tools/pipe/libframecount2.so: the plugin accounts the instructions of the script brackets
    if (m && *m && *m != '0')
        NpAiMarksOn = 1;
    const char* s = getenv("MSC_AI_STATS");
    if (s && *s && *s != '0')
        sStats = true;
    if (sStats)
        atexit(PrintAtExit);
}
} // namespace

extern "C" float NpAiBudgetSetup(NpAiClockFn* profilingClock, NpAiClockFn realClock)
{
    Init();
    sReal = realClock;
    sBaseWork = 0.0;
    switch (sMode)
    {
    case kOff:
        fprintf(stderr, "[aibudget] single player: budget -1 (no script is skipped)\n");
        return -1.0f;
    case kSlow:
        *profilingClock = SlowClock;
        fprintf(stderr, "[aibudget] single player: budget 1.0, real clock / %g (CPU %g times slower than the Wii)\n", sParam, sParam);
        break;
    case kOps:
        *profilingClock = OpsClock;
        fprintf(stderr, "[aibudget] single player: budget 1.0, interpreter-work clock (%g thousand weighted ops = 1 ms)\n", sParam);
        break;
    case kInsn:
        InsnWork();
        if (sInsnBad)
        {
            fprintf(stderr, "[aibudget] insn clock needs the qemu plugin (syscall 0x7f01): retail clock\n");
            sMode = kRetail;
            break;
        }
        sBaseWork = InsnWork();
        *profilingClock = InsnClock;
        fprintf(stderr, "[aibudget] single player: budget 1.0, instruction clock (%g thousand instructions = 1 ms)\n", sParam);
        break;
    default:
        if (sStats)
            fprintf(stderr, "[aibudget] single player: retail budget 1.0, real clock (statistics on)\n");
        break;
    }
    return 1.0f;
}

extern "C" void NpAiMark(int op)
{
    Mark(op);
}

extern "C" void NpAiBudgetObserve(float acc, int rejected)
{
    if (NpAiMarksOn)
        Mark(3);
    if (!sStats)
        return;
    S.checks++;
    S.updChecks++;
    if (rejected)
        S.rejects++;
    if (acc > 0.0f)
        for (int i = 0; i < kScales; i++)
        {
            double t = (double)acc * kScale[i];
            S.sumP[i] += 0.8 * (t < 1.0 ? t : 1.0);
        }
}

extern "C" void NpAiBudgetEndUpdate(float acc)
{
    if (NpAiMarksOn)
        Mark(4);
    if (sMode == kOps)
        sBaseWork = InterpWork();
    else if (sMode == kInsn)
        sBaseWork = InsnWork();
    if (!sStats)
        return;
    S.updates++;
    S.sumAcc += acc;
    if (acc > S.maxAcc)
        S.maxAcc = acc;
    int b = (int)(acc / 0.025f);
    S.hist[b < 0 ? 0 : b > kBuckets ? kBuckets : b]++;
    for (int i = 0; i < kScales; i++)
        if ((double)acc * kScale[i] >= 1.0)
            S.satUpdates[i]++;
    S.updChecks = 0;
    if (S.updates % 3600 == 0)
        Print("running");
}
