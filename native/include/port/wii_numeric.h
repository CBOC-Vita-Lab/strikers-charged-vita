// MWCC / PowerPC numeric conversions that the host compilers leave undefined.
// Adapted from yannicksuter/mscharged-port (CC0) src/platform/variant_numeric_abi.h, patches 0700/0702.
#ifndef PORT_WII_NUMERIC_H
#define PORT_WII_NUMERIC_H

#include <stdint.h>

// MWCC's __cvt_fp2unsigned: below zero gives 0; >= 2^32 and NaN give UINT32_MAX; the rest truncates.
// A plain (unsigned)float cast of a negative value is undefined: ARM VFP (Vita, armhf) and PowerPC
// saturate to 0, but x86 (host32, x86-64) wraps through int64 (-3.0f -> 4294967293), which made the
// game treat "10 * (duration - time) == 0" as false once the clock runs past the whistle
// (sudden death, a shot still live at the whistle) and the HUD clock show a negative tenths field.
static inline uint32_t port_f2u32(float value)
{
    if (value < 0.0f)
        return 0;
    if (!(value < 4294967296.0f))
        return 0xFFFFFFFFu;
    return (uint32_t)value;
}

#endif
