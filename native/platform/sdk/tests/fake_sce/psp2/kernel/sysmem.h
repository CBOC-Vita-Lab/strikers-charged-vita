#ifndef FAKE_PSP2_SYSMEM_H
#define FAKE_PSP2_SYSMEM_H
#include <stdint.h>
typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceKernelMemBlockType;
typedef struct SceKernelAllocMemBlockOpt SceKernelAllocMemBlockOpt;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0C20D060
#ifdef __cplusplus
extern "C" {
#endif
SceUID sceKernelAllocMemBlock(const char* name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt* opt);
int sceKernelGetMemBlockBase(SceUID uid, void** base);
int sceKernelFreeMemBlock(SceUID uid);
#ifdef __cplusplus
}
#endif
#endif
