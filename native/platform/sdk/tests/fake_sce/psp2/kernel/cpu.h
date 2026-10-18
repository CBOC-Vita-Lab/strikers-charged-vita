// Fake of the VitaSDK headers, just what vita/np_plat_vita.cpp uses, backed by pthreads (fake_sce.cpp):
// lets the Vita platform file run in the host unit tests.
#ifndef FAKE_PSP2_CPU_H
#define FAKE_PSP2_CPU_H
#define SCE_KERNEL_CPU_MASK_USER_0 0x00010000
#define SCE_KERNEL_CPU_MASK_USER_1 0x00020000
#define SCE_KERNEL_CPU_MASK_USER_2 0x00040000
#define SCE_KERNEL_CPU_MASK_USER_ALL (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2)
#endif
