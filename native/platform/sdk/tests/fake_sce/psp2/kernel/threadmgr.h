#ifndef FAKE_PSP2_THREADMGR_H
#define FAKE_PSP2_THREADMGR_H
#include <stdint.h>
#include <psp2/kernel/sysmem.h>
typedef unsigned int SceUInt;
typedef int64_t SceInt64;
typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);
typedef struct SceKernelLwMutexWork { int64_t data[4]; } SceKernelLwMutexWork;   // 32 bytes, 8-aligned
typedef struct SceKernelLwMutexOptParam SceKernelLwMutexOptParam;
typedef struct SceKernelSemaOptParam SceKernelSemaOptParam;
typedef struct SceKernelThreadOptParam SceKernelThreadOptParam;
typedef struct SceKernelThreadInfo {
    SceSize size;
    SceUID processId;
    SceUID threadId;
    char name[32];
    SceUInt attr;
    SceUInt status;
    SceKernelThreadEntry entry;
    void* stack;
    int stackSize;
    int initPriority;
    int currentPriority;
    int initCpuAffinityMask;
    int currentCpuAffinityMask;
} SceKernelThreadInfo;
#ifdef __cplusplus
extern "C" {
#endif
SceUID sceKernelCreateSema(const char* name, SceUInt attr, int initVal, int maxVal, SceKernelSemaOptParam* option);
int sceKernelDeleteSema(SceUID semaid);
int sceKernelSignalSema(SceUID semaid, int signal);
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt* timeout);
int sceKernelCreateLwMutex(SceKernelLwMutexWork* pWork, const char* pName, unsigned int attr, int initCount, const SceKernelLwMutexOptParam* pOptParam);
int sceKernelDeleteLwMutex(SceKernelLwMutexWork* pWork);
int sceKernelLockLwMutex(SceKernelLwMutexWork* pWork, int lockCount, unsigned int* pTimeout);
int sceKernelTryLockLwMutex(SceKernelLwMutexWork* pWork, int lockCount);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork* pWork, int unlockCount);
SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr, int cpuAffinityMask, const SceKernelThreadOptParam* option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void* argp);
int sceKernelWaitThreadEnd(SceUID thid, int* stat, SceUInt* timeout);
int sceKernelDeleteThread(SceUID thid);
int sceKernelExitThread(int status);
int sceKernelExitDeleteThread(int status);
SceUID sceKernelGetThreadId(void);
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo* info);
int sceKernelChangeThreadPriority(SceUID thid, int priority);
int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int mask);
int sceKernelDelayThread(SceUInt delay);
SceInt64 sceKernelGetSystemTimeWide(void);
#ifdef __cplusplus
}
#endif
#endif
