// The Vita's JPEG engine behind the THP hardware path (see thp_hwjpeg.h, thp_native.cpp).
//
// sceJpegDecodeMJpegYCbCr takes a standard baseline JPEG and writes planar YCbCr (4:2:0: a luma plane, then Cb,
// then Cr) without colour conversion. The codec reads and writes physically contiguous memory, so the stream, the
// output and the coefficient buffer live in one memory block of CDRAM (what the console's own samples use; write
// combined, so nothing needs flushing, but reading it from the CPU is slow: the caller reads in whole rows) with
// physically contiguous main memory as the second choice. MSC_THP_HWJPEG_MEM=cdram|phycont|auto picks.
//
// The prototypes are declared here and psp2/jpeg.h is NOT included: the VitaSDK header lists sceJpegDecodeMJpegYCbCr
// with the mode before the output buffer, the order of the Sony library is (jpeg, size, output, outputSize, mode,
// coefficients, coefficientSize). The stubs are linked weak (-lSceJpeg_stub_weak): without the library every call
// reports "unavailable" and the player keeps decoding in software.
#include "thp_hwjpeg.h"

#if defined(NP_VITA) && !defined(NP_FAKE_SCE)

#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
struct NpSceJpegInitParam {
    uint32_t size;
    int32_t decoderCount;
    int32_t options;
};
struct NpSceJpegPitch {
    uint32_t x, y;
};
struct NpSceJpegOutputInfo {
    int32_t colorSpace;
    uint16_t width, height;
    uint32_t outputSize;
    uint32_t tempSize;
    uint32_t coefSize;
    NpSceJpegPitch pitch[4];
};
static_assert(sizeof(NpSceJpegInitParam) == 0xC, "SceJpegMJpegInitParam");
static_assert(sizeof(NpSceJpegOutputInfo) == 0x34, "SceJpegOutputInfo");
int sceJpegInitMJpegWithParam(const NpSceJpegInitParam* param) __attribute__((weak));
int sceJpegFinishMJpeg(void) __attribute__((weak));
int sceJpegGetOutputInfo(const uint8_t* jpeg, uint32_t size, int32_t format, int32_t mode, NpSceJpegOutputInfo* out) __attribute__((weak));
int sceJpegDecodeMJpegYCbCr(const uint8_t* jpeg, uint32_t size, uint8_t* yuv, uint32_t yuvSize, int32_t mode, void* coef,
                            uint32_t coefSize) __attribute__((weak));
}

namespace {
const int32_t kModeWithDht = 0;             // SCE_JPEG_MJPEG_WITH_DHT: the stream carries its tables
const int32_t kNoCsc = -1;                  // SCE_JPEG_NO_CSC_OUTPUT: YCbCr planes
const uint32_t kSpaceMask = 0xFFFF0000u, kSpaceYuv = 0x00020000u, kSamplingMask = 0xFFFFu, kSampling420 = 0x0202u;
const uint32_t kAlign = 256;

struct Ctx {
    SceUID block;
    uint8_t* stream;
    uint8_t* yuv;
    void* coef;
    uint32_t streamCap, yuvCap, coefCap;
    bool jpegInit;
};

uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// One block of `bytes` (rounded to what the memory type allows) of CDRAM or physically contiguous main memory.
SceUID AllocBlock(const char* name, uint32_t bytes, uint8_t** base, const char** kind)
{
    const char* e = getenv("MSC_THP_HWJPEG_MEM");
    const bool tryCdram = !(e && !strcmp(e, "phycont")), tryPhy = !(e && !strcmp(e, "cdram"));
    if (tryCdram) {
        const SceUID id = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, RoundUp(bytes, 256 * 1024), nullptr);
        void* p = nullptr;
        if (id >= 0 && sceKernelGetMemBlockBase(id, &p) >= 0 && p) {
            *base = static_cast<uint8_t*>(p);
            *kind = "cdram";
            return id;
        }
        if (id >= 0) sceKernelFreeMemBlock(id);
        fprintf(stderr, "[thp-hw] no CDRAM block of %u KiB (code 0x%08x)\n", (unsigned)(RoundUp(bytes, 256 * 1024) >> 10), (unsigned)id);
    }
    if (tryPhy) {
        const SceUID id = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, RoundUp(bytes, 4096), nullptr);
        void* p = nullptr;
        if (id >= 0 && sceKernelGetMemBlockBase(id, &p) >= 0 && p) {
            *base = static_cast<uint8_t*>(p);
            *kind = "phycont";
            return id;
        }
        if (id >= 0) sceKernelFreeMemBlock(id);
        fprintf(stderr, "[thp-hw] no physically contiguous block of %u KiB (code 0x%08x)\n", (unsigned)(RoundUp(bytes, 4096) >> 10), (unsigned)id);
    }
    return -1;
}

void Close(void* c)
{
    Ctx* x = static_cast<Ctx*>(c);
    if (!x) return;
    if (x->block >= 0) sceKernelFreeMemBlock(x->block);
    if (x->jpegInit && sceJpegFinishMJpeg) sceJpegFinishMJpeg();
    free(x);
}

int Open(void** ctx, const uint8_t* probe, uint32_t probeSize, uint32_t maxStream, uint32_t* width, uint32_t* height)
{
    if (!sceJpegInitMJpegWithParam || !sceJpegGetOutputInfo || !sceJpegDecodeMJpegYCbCr || !sceJpegFinishMJpeg) return -1;
    if (!probe || probeSize == 0 || maxStream < probeSize || maxStream > (4u << 20)) return -2;
    Ctx* x = static_cast<Ctx*>(calloc(1, sizeof(Ctx)));
    if (!x) return -2;
    x->block = -1;
    const NpSceJpegInitParam init = {sizeof(init), 0, 0};
    int r = sceJpegInitMJpegWithParam(&init);
    if (r < 0) {
        fprintf(stderr, "[thp-hw] sceJpegInitMJpegWithParam failed 0x%08x\n", (unsigned)r);
        free(x);
        return -3;
    }
    x->jpegInit = true;
    x->streamCap = RoundUp(maxStream, kAlign);

    // the geometry: the codec reads it (and the sizes of the buffers it wants) from a stream in contiguous memory
    uint8_t* pbase = nullptr;
    const char* pkind = "";
    const SceUID probeBlock = AllocBlock("thp_jpeg_probe", x->streamCap, &pbase, &pkind);
    if (probeBlock < 0) {
        Close(x);
        return -4;
    }
    memcpy(pbase, probe, probeSize);
    NpSceJpegOutputInfo info;
    memset(&info, 0, sizeof(info));
    r = sceJpegGetOutputInfo(pbase, probeSize, kNoCsc, kModeWithDht, &info);
    sceKernelFreeMemBlock(probeBlock);
    if (r < 0) {
        fprintf(stderr, "[thp-hw] sceJpegGetOutputInfo failed 0x%08x\n", (unsigned)r);
        Close(x);
        return -5;
    }
    const uint32_t space = static_cast<uint32_t>(info.colorSpace);
    if ((space & kSpaceMask) != kSpaceYuv || (space & kSamplingMask) != kSampling420 || info.width == 0 || info.height == 0 ||
        info.outputSize == 0) {
        fprintf(stderr, "[thp-hw] unexpected stream shape: colour space 0x%x, %ux%u, output %u\n", (unsigned)space, info.width, info.height,
                info.outputSize);
        Close(x);
        return -6;
    }
    x->yuvCap = RoundUp(info.outputSize, kAlign);
    x->coefCap = info.coefSize ? RoundUp(info.coefSize, kAlign) : 0;
    const uint64_t total = (uint64_t)x->streamCap + x->yuvCap + x->coefCap;
    if (total > (8u << 20)) {
        Close(x);
        return -7;
    }
    const char* kind = "";
    uint8_t* base = nullptr;
    x->block = AllocBlock("thp_jpeg", (uint32_t)total, &base, &kind);
    if (x->block < 0) {
        Close(x);
        return -8;
    }
    x->stream = base;
    x->yuv = base + x->streamCap;
    x->coef = x->coefCap ? static_cast<void*>(x->yuv + x->yuvCap) : nullptr;
    fprintf(stderr, "[thp-hw] codec ready: %ux%u colour space 0x%x, %s block %u KiB (stream %u, planes %u, coefficients %u)\n", info.width,
            info.height, (unsigned)space, kind, (unsigned)(total >> 10), x->streamCap, x->yuvCap, x->coefCap);
    *width = info.width;
    *height = info.height;
    *ctx = x;
    return 0;
}

int Decode(void* c, const uint8_t* jpeg, uint32_t size, const uint8_t** planes, uint32_t* pitchW, uint32_t* pitchH)
{
    Ctx* x = static_cast<Ctx*>(c);
    if (!x || !jpeg || size == 0 || size > x->streamCap) return -1;
    memcpy(x->stream, jpeg, size);
    const int r = sceJpegDecodeMJpegYCbCr(x->stream, size, x->yuv, x->yuvCap, kModeWithDht, x->coef, x->coefCap);
    if (r < 0) return r;
    *planes = x->yuv;
    *pitchW = static_cast<uint32_t>(r) >> 16;
    *pitchH = static_cast<uint32_t>(r) & 0xFFFFu;
    return (*pitchW == 0 || *pitchH == 0) ? -2 : 0;
}

const NpJpegCodec kVita = {"sceJpeg", Open, Decode, Close};
} // namespace

const NpJpegCodec* NpJpegCodecVita(void) { return &kVita; }

#else

const NpJpegCodec* NpJpegCodecVita(void) { return nullptr; }

#endif
