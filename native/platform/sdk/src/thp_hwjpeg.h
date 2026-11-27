#ifndef NP_THP_HWJPEG_H
#define NP_THP_HWJPEG_H
// The JPEG codec behind the THP hardware path (MSC_THP_HWJPEG): decodes a standard baseline 4:2:0 JPEG
// into planar YCbCr. The console's implementation is thp_jpeg_vita.cpp (sceJpegDecodeMJpegYCbCr); hosts have
// none, apart from the test codec in thp_native.cpp that proves the plumbing around it.
#include <stdint.h>

struct NpJpegCodec {
    const char* name;
    // Prepares the codec for streams of at most `maxStream` bytes. `probe` is a first valid stream (the codec
    // reads the geometry from it). 0 on success: *ctx is the codec's state, *width/*height the picture size.
    int (*open)(void** ctx, const uint8_t* probe, uint32_t probeSize, uint32_t maxStream, uint32_t* width, uint32_t* height);
    // Decodes one stream. On success returns 0 with *planes pointing at the luma plane (Cb and Cr follow it,
    // each a quarter of its size) and the pitch (bytes per luma row) and the number of luma rows decoded. The
    // memory belongs to the codec and stays valid until the next decode or close; it may be uncached.
    int (*decode)(void* ctx, const uint8_t* jpeg, uint32_t size, const uint8_t** planes, uint32_t* pitchW, uint32_t* pitchH);
    void (*close)(void* ctx);
};

// The console's codec, or null where there is none (hosts).
const NpJpegCodec* NpJpegCodecVita(void);

#endif
