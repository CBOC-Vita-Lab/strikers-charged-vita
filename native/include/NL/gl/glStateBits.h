#ifndef NL_GL_GL_STATE_BITS_H
#define NL_GL_GL_STATE_BITS_H

// 1.3: the fields of a packed raster state read and written in one step. The results are those of the original bit-by-bit loops for every field of packed_raster
// (widths 0 to 8, start + width <= 32): tests in renderer/tests/gl_state_bits.cpp. glGetRasterState is on the hot list of the profile: it runs for every state change of a send.
static inline unsigned long glStateBitsGet(unsigned long state, int startBit, int numBits)
{
    return (state >> startBit) & ((1u << numBits) - 1u);
}

static inline unsigned long glStateBitsPut(unsigned long state, int startBit, int numBits, unsigned long value)
{
    const unsigned long mask = ((1u << numBits) - 1u) << startBit;
    return (state & ~mask) | ((value << startBit) & mask);
}

#endif
