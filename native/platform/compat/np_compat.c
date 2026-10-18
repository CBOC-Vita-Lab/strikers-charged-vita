/* Data and out-of-line functions behind include/np/compat.h (MSL / Metrowerks runtime names). */
const unsigned int __float_nan[1]  = { 0x7FFFFFFFu };  /* MSL: quiet NaN */
const unsigned int __float_huge[1] = { 0x7F800000u };  /* +infinity */
const float __float_max = 3.40282347e+38f;             /* declared by ode/collision_space.cpp */

/* MSL / RVL SDK `int __abs(int)` (revolution/types.h) */
int __abs(int value) { return value < 0 ? -value : value; }

/* MSL <extras.h> names */
#include <strings.h>
int strcmpi(const char *a, const char *b) { return strcasecmp(a, b); }

/* revolution/base/PPCArch.h */
void PPCSync(void) { __sync_synchronize(); }
