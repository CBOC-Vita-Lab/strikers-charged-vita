/* Compatibility prelude so clang -m32 can read the decomp's MWCC-flavoured headers for layout dumps. */
#include <stddef.h>
#define __fabs(x) __builtin_fabs(x)
#define __declspec(x)
