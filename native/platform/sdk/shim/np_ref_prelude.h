// Force-included in the SDK sources taken over from the decomp (REFFLAGS): declarations MWCC let them skip.
// GCC 14 and later make an implicit function declaration an error.
#ifndef NP_REF_PRELUDE_H
#define NP_REF_PRELUDE_H
#ifdef __cplusplus
extern "C" {
#endif
int OSDisableInterrupts(void);
int OSRestoreInterrupts(int enabled);
void __memclr(void* dst, unsigned long n);
#ifdef __cplusplus
}
#endif
#endif
