/* np/framesend.h: the game's side of the frame-send worker (platform/vita/include/np_framesend.h has the contract).
 *
 * The game links with or without the platform library that holds the worker, so every symbol here is weak: without it the gates
 * are one never-taken branch on a null address and glplatSendFrame sends inline, as it always did.
 *
 *   NpFsGate("why")   a game-thread entry into something the frame send reads or writes (the frame allocator, the views' packet
 *                     trees, the movie decoder ...): if a send is pending, wait for it. One load and a branch otherwise.
 */
#ifndef NP_FRAMESEND_GAME_H
#define NP_FRAMESEND_GAME_H

#ifdef NP_NATIVE
#ifdef __cplusplus
extern "C" {
#endif
extern volatile int NpFsBusy __attribute__((weak));
extern volatile int NpFsPipeOn __attribute__((weak));   /* W5 PIPE: the two-stage pipeline is on (MSC_FS_PIPE=1): the job lives until the next fork */
void NpFsGateSlow(const char* why) __attribute__((weak));
typedef void (*NpFsJobFn)(void* arg);
int NpFsMode(void) __attribute__((weak));
int NpFsStates(void) __attribute__((weak));
int NpFsTaskMask(void) __attribute__((weak));
void NpFsFork(NpFsJobFn fn, void* arg) __attribute__((weak));
void NpFsJoin(void) __attribute__((weak));
void NpFsAfterJoin(void (*fn)(void)) __attribute__((weak));
int NpFsOnWorker(void) __attribute__((weak));
void NpFsHeapAlloc(void* ptr) __attribute__((weak));
int NpFsHeapFree(void* ptr) __attribute__((weak));
void NpFsSetFreeFn(void (*fn)(void*)) __attribute__((weak));
int NpFsDeferWrite(void** slot, void* value) __attribute__((weak));   /* W5 PIPE: 1 = the store is kept until the job is done (pipeline only) */
void NpFsWorkerHeapCall(void) __attribute__((weak));
int NpFsDeferCall(void (*fn)(const void*), const void* data, unsigned size) __attribute__((weak));   /* W5 PIPE: 1 = kept until the job is done */
int NpLaneEnabled(void) __attribute__((weak));
void NpLaneSubmit(NpFsJobFn fn, void* arg) __attribute__((weak));
void NpLaneWait(void) __attribute__((weak));
#ifdef __cplusplus
}
#endif
static inline void NpFsGate(const char* why)
{
    if (&NpFsBusy != 0 && NpFsBusy != 0)
    {
        NpFsGateSlow(why);
    }
}
/* A gate that the two-stage pipeline has made unnecessary (the view trees and the frame arena are double buffered at the fork). */
static inline void NpFsGateNotPipe(const char* why)
{
    if (&NpFsBusy != 0 && NpFsBusy != 0 && !(&NpFsPipeOn != 0 && NpFsPipeOn != 0))
    {
        NpFsGateSlow(why);
    }
}
static inline int NpFsPipe(void)
{
    return &NpFsPipeOn != 0 && NpFsPipeOn != 0;
}
/* Make the GX call fn(data) (no result, plain-value arguments copied from `size` bytes at `data`) now, or, with a job pending, right after the join, in order
 * with the other deferred calls: the recorder has one producer and the stream keeps the order of the serial program. */
static inline void NpFsDefer(void (*fn)(const void*), const void* data, unsigned size)
{
    if (&NpFsBusy != 0 && NpFsBusy != 0 && &NpFsDeferCall != 0 && NpFsDeferCall(fn, data, size))
    {
        return;
    }
    fn(data);
}
/* Store `value` into `*slot`, a pointer the send job reads: with a job in flight in the pipeline the store is made after the join. */
static inline void NpFsStore(void** slot, void* value)
{
    if (&NpFsBusy != 0 && NpFsBusy != 0 && &NpFsDeferWrite != 0 && NpFsDeferWrite(slot, value))
    {
        return;
    }
    *slot = value;
}
#else
static inline void NpFsStore(void** slot, void* value) { *slot = value; }
static inline void NpFsGate(const char*) {}
static inline void NpFsGateNotPipe(const char*) {}
static inline int NpFsPipe(void) { return 0; }
static inline void NpFsDefer(void (*fn)(const void*), const void* data, unsigned) { fn(data); }
#endif

#endif // NP_FRAMESEND_GAME_H
