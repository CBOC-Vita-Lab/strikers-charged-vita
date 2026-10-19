# THPSimple.cpp is the game's own movie player (decomp: ref/RVL_SDK/thp). A THP file is big-endian and the
# player reads headers and per-frame size tables as structs: swap the words right after each read
# (platform/sdk/src/thp_native.cpp). The library compiles the patched file (REF_CXX_SRC in sources.mk).
/^#include <string.h>$/a\
extern "C" {\
void NpThpFixHeader(void*);\
void NpThpFixCompInfo(void*);\
void NpThpFixInfo(void*, int);\
void NpThpFixFrame(void*, unsigned int);\
int NpThpVideoDecode(void*, void*, void*, void*, void*, int, unsigned, unsigned);\
void NpThpPrefetch(const void*, int, unsigned, unsigned);\
void NpThpDiscard(const void*, int);\
void NpThpReset(void);\
}
s/^\( *\)memcpy(&SimpleControl\.compInfo, WorkBuffer, sizeof(THPFrameCompInfo));/\1NpThpFixCompInfo(WorkBuffer);\n&/
s/^\( *\)memcpy(&SimpleControl\.videoInfo, WorkBuffer, sizeof(THPVideoInfo));/\1NpThpFixInfo(WorkBuffer, 3);\n&/
s/^\( *\)memcpy(&SimpleControl\.audioInfo, WorkBuffer, sizeof(THPAudioInfo));/\1NpThpFixInfo(WorkBuffer, 4);\n&/
s/^\( *\)memcpy(SimpleControl\.magic, WorkBuffer, sizeof(THPHeader));/\1NpThpFixHeader(WorkBuffer);\n&/
s/^\( *\)SimpleControl\.readSize = \*(u32\*)SimpleControl\.readBuffer\[SimpleControl\.readIndex\]\.ptr;/\1NpThpFixFrame(SimpleControl.readBuffer[SimpleControl.readIndex].ptr, SimpleControl.compInfo.numComponents);\n&/
s/^\( *\)SimpleControl\.readSize = \*(long\*)SimpleControl\.readBuffer\[idx\]\.ptr;/\1NpThpFixFrame(SimpleControl.readBuffer[idx].ptr, SimpleControl.compInfo.numComponents);\n&/
# Decode-ahead lane (platform/sdk/src/thp_native.cpp): the player decodes through NpThpVideoDecode (with the frame number and the size) and,
# when a frame is done, tells the lane about the next one; opening, closing, preloading or stopping a movie drains the lane first.
s/^\( *\)long ret = THPVideoDecode(videoFrame, SimpleControl\.textureSet\.ytexture, SimpleControl\.textureSet\.utexture, SimpleControl\.textureSet\.vtexture, SimpleControl\.thpWork);/\1long ret = NpThpVideoDecode(videoFrame, SimpleControl.textureSet.ytexture, SimpleControl.textureSet.utexture, SimpleControl.textureSet.vtexture, SimpleControl.thpWork, SimpleControl.readBuffer[SimpleControl.nextDecodeIndex].frameNumber, SimpleControl.videoInfo.xSize, SimpleControl.videoInfo.ySize);/
/^extern "C" long THPSimpleDecode(long audioTrack)$/i\
static inline void NpThpPrefetchNextFrames()\
{\
    for (long ahead = 0; ahead < 4; ahead++)\
    {\
        THPReadBuffer* rb = &SimpleControl.readBuffer[(SimpleControl.nextDecodeIndex + ahead) % 10];\
        if (rb->isValid == 0)\
            return;\
        unsigned long* cs = (unsigned long*)(rb->ptr + 8);\
        unsigned char* p = rb->ptr + SimpleControl.compInfo.numComponents * 4 + 8;\
        for (unsigned long i = 0; i < SimpleControl.compInfo.numComponents; i++)\
        {\
            if (SimpleControl.compInfo.frameComp[i] == 0)\
            {\
                NpThpPrefetch(p, rb->frameNumber, SimpleControl.videoInfo.xSize, SimpleControl.videoInfo.ySize);\
                break;\
            }\
            p += *cs;\
            cs++;\
        }\
    }\
}
s/^\( *\)checkPrefetch();/\1if (decodeVideo) NpThpPrefetchNextFrames();\n&/
/^extern "C" int THPSimpleOpen(const char\* fileName)$/{n;s/{/{\n    NpThpReset();/}
/^extern "C" int THPSimpleClose()$/{n;s/{/{\n    NpThpReset();/}
/^extern "C" int THPSimplePreLoad(long loop)$/{n;s/{/{\n    NpThpReset();/}
/^extern "C" int THPSimpleLoadStop()$/{n;s/{/{\n    NpThpReset();/}
