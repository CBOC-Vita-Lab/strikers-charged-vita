/* np_framesend.h: the frame-send worker of the native port (platform/vita/src/np_framesend.cpp).
 *
 * The game's End Frame task sends the finished frame to the renderer (glx_SendViews walks the views' packet trees and calls the
 * material programs, which call GX, i.e. the vgx recorder; then GXCopyDisp hands the frame to the render thread). That is 35 % of
 * the game thread in open play (np/docs/wave2-NPP.md). This module lets the game hand that job to a second core and go on with the
 * next frame's simulation: the model is the recompiled build's two-core split (split/NOTES.md): fork after the swap wait, join at
 * the first use of what the send reads or writes.
 *
 *   NpFsFork(fn, arg)   game thread. Thread mode: wakes the worker and returns; the worker runs fn(arg). Serial mode: remembers the
 *                       job and runs it on the game thread at the next join (the SAME reordered program on one thread: the A/B that
 *                       separates "reordering" from "two threads"). Off: runs fn(arg) now.
 *   NpFsJoin()          game thread. Returns when the job has finished (nothing pending: returns at once); then the after-join
 *                       hook runs (on the game thread).
 *   NpFsGateSlow(why)   the gates (include/np/framesend.h NpFsGate): a game-thread entry into something the job reads or writes
 *                       joins first; on the worker it does nothing. `why` names the gate in the statistics.
 *
 * Environment (env.txt on the Vita):
 *   MSC_FRAMESEND=off|thread|serial|inline   (inline: the snapshot path, the job runs at the fork: no reordering) default off until the proof runs of docs/wave3-NPP2.md are in (thread on the Vita and armhf
 *                                     once the default is switched: see NpFsDefaultMode)
 *   MSC_FS_STATES=<mask>              task states in which a frame is forked (default 0x2: open play and the kick-off)
 *   MSC_FS_FREEGATE=0|1|2             a heap free of a block older than the window: 1 joins first, 2 keeps the free until the join (default 2)
 *   MSC_FS_TASKS=<mask of priorities> tasks that may run while a job is pending (see nlTask.cpp; default: 0-8, 17, 24 = all but 9..16)
 *   MSC_FS_STACK_KB=<n>               worker stack (default 256 on the Vita, 1024 elsewhere)
 *   MSC_FS_MARKS=1                    syscall marks for the qemu plugin (also on with MSC_QEMU_MARK): 0x30000001 fork (game thread),
 *                                     0x30000002 join done, 0x30000003 job begin, 0x30000004 job end (worker), 0x30000005 join wait
 *                                     starts. The plugin prints every vCPU's instruction count at each mark.
 */
#ifndef NP_FRAMESEND_H
#define NP_FRAMESEND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*NpFsJobFn)(void* arg);

enum { NP_FS_OFF = 0, NP_FS_THREAD = 1, NP_FS_SERIAL = 2, NP_FS_INLINE = 3 };

/* 1 while a job is outstanding (thread mode: forked and not joined; serial: remembered). Read by the inline gates. */
extern volatile int NpFsBusy;

int NpFsMode(void);                           /* decided on first use from MSC_FRAMESEND */
int NpFsStates(void);                         /* MSC_FS_STATES */
int NpFsFreeGate(void);                       /* MSC_FS_FREEGATE: 0 none (unsafe), 1 join, 2 defer the free to the join */
int NpFsTaskMask(void);                       /* bit p set: a task of priority p may run while a job is pending */
void NpFsFork(NpFsJobFn fn, void* arg);
void NpFsJoin(void);
void NpFsGateSlow(const char* why);
int NpFsOnWorker(void);                       /* 1 on the worker thread (and while a serial-mode job runs) */
/* A game-thread GX call that has no result and whose arguments are plain values: while a job is pending the call is kept (arguments copied, `size` bytes) and made
 * on the game thread right after the join, in order with the other deferred calls; returns 1 when it was kept, 0 when the caller makes it now (nothing pending). */
int NpFsDeferCall(void (*fn)(const void*), const void* data, unsigned size);
void NpFsAfterJoin(void (*fn)(void));         /* run on the game thread after every join that had a job */
void NpFsLogStats(const char* when);          /* one block of [fs] lines */

/* heap events while a window is open (game thread): the block is new in the window / an old block is freed. Called by src/NL/nlMemory.cpp;
 * ptr may be 0. NpFsHeapFree: a block allocated inside the window is freed at once (the job cannot refer to it: returns 0); an older block
 * is, with MSC_FS_FREEGATE=1, preceded by a join (returns 0), with MSC_FS_FREEGATE=2 kept until the join (returns 1: the caller does NOT free
 * it; the module frees it through the function given to NpFsSetFreeFn, on the game thread, right after the job has finished). */
void NpFsHeapAlloc(void* ptr);
int NpFsHeapFree(void* ptr);
void NpFsSetFreeFn(void (*fn)(void*));
/* the worker made a heap call (a violation of the job contract: counted and logged once) */
void NpFsWorkerHeapCall(void);

/* A helper lane for data-parallel work of the game thread (NPP2: the software skinning of cut-scenes and 3D menus): a second thread that runs the
 * submitted jobs in order, one at a time, beside the game thread. MSC_SKIN_LANE=1 enables it (default 0); NpLaneEnabled() says whether it is on.
 *   NpLaneSubmit(fn, arg)  game thread: queue fn(arg); it runs on the lane thread (when the queue is full the lane is drained first and fn runs here)
 *   NpLaneWait()           game thread: returns when every job submitted so far has finished (nothing submitted: returns at once)
 * The caller keeps arg alive until NpLaneWait returned; jobs of one submitter run in the order submitted. */
int NpLaneEnabled(void);
void NpLaneSubmit(NpFsJobFn fn, void* arg);
void NpLaneWait(void);

#ifdef __cplusplus
}
#endif
#endif
