#include "np/touch_diag.h"
#if defined(__vita__) || defined(NP_DIAG_GPU_TEST)
#include <psp2/gxm.h>
#include <atomic>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif
namespace {
std::atomic<uint32_t> renderTid{0},renderTag{0};
uint32_t Tid(){
#if defined(__vita__)
    return (uint32_t)sceKernelGetThreadId();
#else
    return 1;
#endif
}
void Record(unsigned kind,uint64_t begin,int rc,uintptr_t caller){
    if(begin){const uint64_t end=NpDiagNow();const uint32_t tid=Tid();
        const uint64_t tag=renderTid.load(std::memory_order_acquire)==tid?renderTag.load(std::memory_order_acquire):0;
        NpDiagEvent(NPD_GPU,kind,tag,begin,end,(uint32_t)rc,caller,tid);}
}
}
extern "C" {
void NpDiagGpuFrame(uint64_t frame){renderTag.store((uint32_t)frame,std::memory_order_release);renderTid.store(Tid(),std::memory_order_release);}
// The sceGxm wrappers link only with the matching --wrap options: NP_DIAG_GPU=1 on the make line adds both (native/Makefile,
// platform/vita/vita.mk). Ordinary builds have neither, so no __real_* reference is left to dead-code removal.
#if !defined(__vita__) || defined(NP_DIAG_GPU_WRAPS)
void __real_sceGxmFinish(SceGxmContext*);
int __real_sceGxmNotificationWait(const SceGxmNotification*);
int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject*,SceGxmSyncObject*,const void*);
int __real_sceGxmBeginScene(SceGxmContext*,unsigned,const SceGxmRenderTarget*,const SceGxmValidRegion*,SceGxmSyncObject*,SceGxmSyncObject*,const SceGxmColorSurface*,const SceGxmDepthStencilSurface*);
int __real_sceGxmEndScene(SceGxmContext*,const SceGxmNotification*,const SceGxmNotification*);
void __wrap_sceGxmFinish(SceGxmContext* c){
    const uint64_t t=NpDiagActive()?NpDiagNow():0;
    __real_sceGxmFinish(c);Record(1,t,0,(uintptr_t)__builtin_return_address(0));
}
int __wrap_sceGxmNotificationWait(const SceGxmNotification* n){
    const uint64_t t=NpDiagActive()?NpDiagNow():0;
    const int r=__real_sceGxmNotificationWait(n);Record(2,t,r,(uintptr_t)__builtin_return_address(0));return r;
}
int __wrap_sceGxmDisplayQueueAddEntry(SceGxmSyncObject* a,SceGxmSyncObject* b,const void* d){
    const uint64_t t=NpDiagActive()?NpDiagNow():0;
    const int r=__real_sceGxmDisplayQueueAddEntry(a,b,d);Record(3,t,r,(uintptr_t)__builtin_return_address(0));return r;
}
int __wrap_sceGxmBeginScene(SceGxmContext* c,unsigned f,const SceGxmRenderTarget* t,const SceGxmValidRegion* v,SceGxmSyncObject* a,SceGxmSyncObject* b,const SceGxmColorSurface* s,const SceGxmDepthStencilSurface* z){
    const uint64_t begin=NpDiagActive()?NpDiagNow():0;
    const int r=__real_sceGxmBeginScene(c,f,t,v,a,b,s,z);Record(4,begin,r,(uintptr_t)__builtin_return_address(0));return r;
}
int __wrap_sceGxmEndScene(SceGxmContext* c,const SceGxmNotification* v,const SceGxmNotification* f){
    const uint64_t t=NpDiagActive()?NpDiagNow():0;
    const int r=__real_sceGxmEndScene(c,v,f);Record(5,t,r,(uintptr_t)__builtin_return_address(0));return r;
}
#endif
}
#endif
