// The few compiler-runtime helpers the SDK sources (compiled unchanged) call. Weak, so that the
// game build's own MWCC runtime replacement (NPB: platform/runtime) takes precedence if it has them.
#include <stddef.h>
#include <string.h>

__attribute__((weak)) void __memclr(void* p, size_t n) { memset(p, 0, n); }

// MWCC intrinsic, declared in revolution/types.h (mem_expHeap.c, mem_frameHeap.c, wenc.c use it).
__attribute__((weak)) int __abs(int v) { return v < 0 ? -v : v; }

// The network configuration (NCD, owned by the multiplayer workstream) is not part of this library:
// "no usable configuration" is what neterrorcode.c's NETGetStartupErrorCode needs to give a generic error.
__attribute__((weak)) int NCDiGetEnabledConfigList(unsigned long* list0, unsigned long* list1, unsigned long* list2) {
    (void)list0; (void)list1; (void)list2;
    return -1;
}
