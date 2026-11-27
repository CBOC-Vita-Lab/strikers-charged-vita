#ifndef _WRCOPY_H_
#define _WRCOPY_H_
#include <stdint.h>
// 0045 (MSC_VITA_WRCOPY): copies into the uncached pools, see wrcopy.c
void vgl_wr_copy(void *dst, const void *src, uint32_t n);
void vglSetWriteCopy(int mode);
int vglGetWriteCopy(void);
void vglWriteCopyVariant(int mode, void *dst, const void *src, uint32_t n);
void vglWriteCopyStats(uint32_t *out4);
void vglWriteCopyStats8(uint32_t *out8);       // 0061
void vglSetWriteCopyFallback(int on);          // 0061
int vglGetWriteCopyFallback(void);
// 0051 (MSC_VITA_TEXCOPY): the level-0 copy of a texture upload, see wrcopy.c
void vgl_tex_copy(void *dst, const void *src, uint32_t n);
void vglSetTextureCopyDma(int mode);
void vglTextureCopyStats(uint32_t *out6);
#endif
