#ifndef NP_DISC_ERROR_H
#define NP_DISC_ERROR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copies the first failed extracted-disc read, or an empty string before a failure.
int NpDiscGetReadError(char* details, size_t capacity);
// Game-thread only; skips interrupt callbacks and releases any inherited
// interrupt lock around terminal presentation.
// SDK-only builds without platform presentation hooks are left untouched.
int NpDiscReportReadError(void);

#ifdef __cplusplus
}
#endif
#endif
