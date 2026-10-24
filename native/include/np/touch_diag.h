#pragma once
#include <stdint.h>
#include <string.h>
enum NpDiagKind {
    NPD_START=1, NPD_POLL, NPD_CONTACT, NPD_PANEL, NPD_SAMPLE, NPD_GESTURE,
    NPD_HANDOVER, NPD_DECODE, NPD_BACKEND, NPD_BACKEND_END, NPD_SWAP,
    NPD_CALLBACK, NPD_GATE, NPD_FIFO_ERROR, NPD_TASK_DT, NPD_SIM_STEP,
    NPD_ATTEMPT, NPD_POINTER_BEFORE, NPD_SAVE, NPD_HANDOFF, NPD_SEND,
    NPD_KEEPER, NPD_SHADER, NPD_CTRL, NPD_MEMORY, NPD_QUIET,
    NPD_CONTEXT, NPD_SIM_END, NPD_SWAP_END, NPD_TASK, NPD_COMMAND,
    NPD_CACHE, NPD_GEOMETRY, NPD_THREAD, NPD_THREAD_SAMPLE_COST,
    NPD_FRAME_SEND, NPD_FRAME_JOIN, NPD_POINTER_AFTER, NPD_CLOCK_COST, NPD_GPU,
    NPD_MEGA_PHASE, NPD_LIBRARY_DISPLAY
};
#ifdef __cplusplus
extern "C" {
#endif
int NpDiagEnabled(void);
int NpDiagActive(void);
uint64_t NpDiagNow(void);
void NpDiagContext(uint32_t mode,uint32_t scene,uint32_t task,uint32_t paused,uint32_t gameState);
void NpDiagIdentity(uint32_t stadium,uint32_t simFrame,float simTime,float timeScale,uint32_t game);
void NpDiagMega(void);
void NpDiagMegaPhase(uint32_t actorSide,uint32_t locallyControlled,uint32_t substate);
int NpDiagInputMode(void);
void NpDiagSnapshot(void);
void NpDiagRegisterThread(uint32_t role,uint32_t tid);
void NpDiagTaskBegin(uint32_t priority);
void NpDiagTaskEnd(uint32_t priority);
void NpDiagGpuFrame(uint64_t tag);
void NpDiagEvent(uint32_t kind,uint32_t flags,uint64_t id,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e);
uint64_t NpDiagFrame(void);
uint64_t NpDiagPollNext(void);
uint64_t NpDiagPollId(void);
void NpDiagUsePoll(uint64_t id);
uint64_t NpDiagUsedPoll(void);
uint64_t NpDiagStep(void);
uint64_t NpDiagStepId(void);
uint64_t NpDiagLatch(void);
uint64_t NpDiagTake(void);
#ifdef __cplusplus
}
static inline uint64_t NpDiagFloats(float a,float b){uint32_t x,y;memcpy(&x,&a,4);memcpy(&y,&b,4);return (uint64_t)x|((uint64_t)y<<32);}
static inline uint64_t NpDiagInts(uint32_t a,uint32_t b){return (uint64_t)a|((uint64_t)b<<32);}
#endif
