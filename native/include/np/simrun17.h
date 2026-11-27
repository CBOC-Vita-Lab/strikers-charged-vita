// np/simrun17.h (block17, plan-simthread17 stage 2a): the simulation step loop on its own thread, the game thread blocking (MSC_SIMT=1, default OFF).
// No gain by design: it proves what the step needs of its thread (stack, game-heap identity, floating-point mode, frame-send gates) before any overlap is built.
#ifndef NP_SIMRUN17_H
#define NP_SIMRUN17_H
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*NpSimtFn)(void*);
// Runs fn(ctx): on the np_sim thread while the caller waits (MSC_SIMT=1, allowed in this game, thread made), else inline on the caller. Same effect either way.
void NpSimtRun(NpSimtFn fn, void* ctx);
// overlap18 (MSC_SIMT=2): 1 when asked for and not refused at start-up / will this picture's step run on np_sim beside the window (game thread, before the kick; makes the thread).
int NpSimtOverlapMode(void);
int NpSimtOverlapNow(void);
// 1 on the np_sim thread (stack-address range test, no TLS).
int NpSimtOnSimThread(void);
// The caller says how many steps the kicked loop ran (game thread, after the join): the periodic [simt] line is printed from here.
void NpSimtAddSteps(int steps);
// Phase marker for the watchdog line (0 idle, 1 pad, 2 capture, 3 step); the sim thread's own.
void NpSimtPhase(int phase);
int NpSimtLevel(void);          /* pace18: MSC_SIMT in force: 0, 1 blocking, 2 overlap */
unsigned NpSimtCoreMask(void);  /* pace18: cores np_sim is pinned to, one bit a core (0 unknown) */
// Probe counters read by the hooks in nlMemory.cpp and glRenderList.cpp (weak there): nonzero while the thread exists.
extern volatile int gNpSimtCensus;
void NpSimtNoteHostAlloc(void);   // a game-heap request that went to the host heap (nlMemory.cpp): counted when it comes from the sim thread
void NpSimtNoteGameAlloc(void);   // a game-heap request (operator new) made on the sim thread
void NpSimtNoteGxRecord(void);    // a packet attached to a render list (glRenderList.cpp): counted when it comes from the sim thread
#ifdef __cplusplus
}
#endif
#endif
