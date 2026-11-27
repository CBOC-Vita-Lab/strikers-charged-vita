#pragma once
#include <stdint.h>
typedef int32_t SceUID;
typedef unsigned int SceSize;   // gt27 MSC_FPS_PROBE_ASYNC: the probe thread and its semaphore (the unit never turns the async read on)
typedef struct SceKernelThreadInfo {
    unsigned size;
    uint64_t runClocks;
    int lastExecutedCpuId;
} SceKernelThreadInfo;
#ifdef __cplusplus
extern "C" {
#endif
int sceKernelGetThreadInfo(SceUID, SceKernelThreadInfo*);
SceUID sceKernelGetThreadId(void);
int sceKernelDelayThread(unsigned);
SceUID sceKernelCreateSema(const char*, unsigned, int, int, void*);
int sceKernelWaitSema(SceUID, int, unsigned*);
int sceKernelSignalSema(SceUID, int);
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, unsigned, unsigned, int, void*);
int sceKernelStartThread(SceUID, SceSize, void*);
int sceKernelChangeThreadCpuAffinityMask(SceUID, int);
#ifdef __cplusplus
}
#endif
