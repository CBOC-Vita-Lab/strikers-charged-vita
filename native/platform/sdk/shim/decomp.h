// Overlay for the decomp's include/decomp.h (found first on the include path of platform/sdk):
// the real header, then the definitions a GNU compiler needs (NPB's include/np/compat.h will
// supersede this for the game build; the two must agree).
#ifndef NP_SDK_SHIM_DECOMP_H
#define NP_SDK_SHIM_DECOMP_H
#undef __CONCAT
#include_next <decomp.h>

// the same text as glibc's <sys/cdefs.h>, so that either include order is quiet
#undef __CONCAT
#define __CONCAT(x,y) x ## y

#undef DECL_SECTION
#define DECL_SECTION(x)
#undef DECL_WEAK
#define DECL_WEAK __attribute__((weak))
#undef DECOMP_FORCEACTIVE
#define DECOMP_FORCEACTIVE(module, ...)
#undef AT_ADDRESS
#define AT_ADDRESS(address)
// MWCC writes this after the declarator; GNU compilers do not accept that position.
#undef DECOMP_DONT_INLINE
#define DECOMP_DONT_INLINE

// A halfword that the console's big-endian layout puts first in a 32-bit word, as a 32-bit constant
// whose memory image has the halfword first (used by patches/*.sed of ref sources that pack two
// halfwords into one store).
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define NP_BE_HI16(v) ((unsigned int)(v))
#else
#define NP_BE_HI16(v) ((unsigned int)(v) << 16)
#endif

// MWCC / PowerPC intrinsics used by SDK sources.
static inline int __cntlzw(unsigned int x) { return x ? __builtin_clz(x) : 32; }

#endif
