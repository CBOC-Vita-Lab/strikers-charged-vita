/* npa_verify.h - independent structural checks of CONVERTED (host-order) assets. Test tooling only. */
#ifndef NPA_VERIFY_H
#define NPA_VERIFY_H
#include <stddef.h>
#include "np_assets.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 0 = ok, nonzero = problem described in msg */
int npav_verify(NpaKind kind, const void* data, size_t size, char* msg, size_t msgcap);
#ifdef __cplusplus
}
#endif
#endif
