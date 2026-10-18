/* np/aibudget.h: the single-player AI script time budget on a CPU that is not the Wii's (PIPE, wave 5).
 *
 * Retail (Game.cpp, cGame start): a match that is not a network or recorded game sets the script time budget to 1.0 and the clock to the
 * real ticker in milliseconds. Every script that is about to run draws nlRandomf(1.0, &nlDefaultSeed) and is skipped when the draw is
 * above chance = 1.0 - 0.8 * min(t / 1.0, 1), t = the ms of script time already used in this update (ScriptMachine.cpp CheckScriptTimeBudget,
 * ResetScriptFrameTime once per UpdateCachedGameData). On a CPU slower than the Wii's t grows faster and more scripts are skipped.
 *
 * Switch MSC_AI_BUDGET (environment, env.txt on the Vita), default retail:
 *   retail          unchanged (budget 1.0, real ticker clock)
 *   off             budget -1 in single player too, like network and recorded matches: no script is ever skipped, no draw from nlDefaultSeed,
 *                   the AI does not depend on the clock any more
 *   slow:K          K = how many times slower than the Wii this CPU runs the scripts (> 1): the real clock reading is divided by K, so the
 *                   budget is used up at the pace of the Wii. Needs the real K from the console (MSC_AI_STATS=1 prints a table)
 *   ops:R           deterministic clock, independent of the host: interpreter work counted, R thousand weighted interpreter ops = 1 ms
 *                   (op = 1, builtin call = 8). For host runs that emulate a slow CPU (tools/pipe/aistats.sh)
 *   insn:R          armhf under the qemu plugin only (tools/pipe/libframecount2.so, syscall 0x7f01 = instruction count of the calling vCPU):
 *                   R thousand instructions = 1 ms (230 = the Vita at 444 MHz [E])
 * MSC_AI_MARKS=1 (armhf under tools/pipe/libframecount2.so only): every script bracket, budget check and update end is reported to the qemu
 * plugin by syscall 0x7f01, which accounts the INSTRUCTIONS spent in scripts per update and prints a table "[aiscript]" at exit with the skip
 * rate the retail rule would give if one millisecond were K thousand instructions (the Vita's console time per instruction is what turns that
 * into a prediction, see docs/wave5-PIPE.md); with MSC_AI_BUDGET=off nothing is skipped and nothing is perturbed.
 * MSC_AI_STATS=1 (or any mode above): count what the budget does and print a table every 3600 updates and at exit ("[aibudget]" lines):
 * per update the script time t, how often the budget check ran, how many scripts it skipped, and what the retail rule WOULD skip if the clock
 * ran s times faster or slower (s = 0.25 .. 3), computed from the accumulated time at each check, so it can be taken in the `off` mode
 * (no script skipped, nothing perturbed) on the console.
 */
#ifndef NP_AIBUDGET_H
#define NP_AIBUDGET_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
typedef float (*NpAiClockFn)(void);
/* called after the retail single-player setup: returns the budget to set and may replace *profilingClock */
float NpAiBudgetSetup(NpAiClockFn* profilingClock, NpAiClockFn realClock);
/* ScriptMachine.cpp: a script is about to run with `acc` ms of script time used in this update; `rejected` is the budget's answer */
void NpAiBudgetObserve(float acc, int rejected);
/* ResetScriptFrameTime: the update is over, `acc` is its script time */
void NpAiBudgetEndUpdate(float acc);
extern int NpAiMarksOn;          /* MSC_AI_MARKS=1: bracket marks for the qemu plugin (syscall 0x7f01) */
void NpAiMark(int op);           /* 1 = bracket begin, 2 = bracket end (3 check, 4 update end are issued by the module) */
extern unsigned NpInterpOps;     /* interpreter steps (InterpreterCore::Step) */
extern unsigned NpInterpCalls;   /* builtin calls (DoFunctionCall) */
#ifdef __cplusplus
}
#endif
#define NP_AI_BRACKET_BEGIN() do { if (NpAiMarksOn) NpAiMark(1); } while (0)
#define NP_AI_BRACKET_END() do { if (NpAiMarksOn) NpAiMark(2); } while (0)
#define NP_AI_COUNT_OP() (++NpInterpOps)
#define NP_AI_COUNT_CALL() (++NpInterpCalls)
#else
#define NP_AI_BRACKET_BEGIN() ((void)0)
#define NP_AI_BRACKET_END() ((void)0)
#define NP_AI_COUNT_OP() ((void)0)
#define NP_AI_COUNT_CALL() ((void)0)
#endif

#endif // NP_AIBUDGET_H
