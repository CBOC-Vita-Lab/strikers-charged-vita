# THPSimple.cpp is the game's own movie player (decomp: ref/RVL_SDK/thp). A THP file is big-endian and the
# player reads headers and per-frame size tables as structs: swap the words right after each read
# (platform/sdk/src/thp_native.cpp). The library compiles the patched file (REF_CXX_SRC in sources.mk).
/^#include <string.h>$/a\
extern "C" {\
void NpThpFixHeader(void*);\
void NpThpFixCompInfo(void*);\
void NpThpFixInfo(void*, int);\
void NpThpFixFrame(void*, unsigned int);\
int NpThpVideoDecode(void*, void*, void*, void*, void*, int, unsigned, unsigned, unsigned);\
void NpThpPrefetch(const void*, unsigned, int, unsigned, unsigned);\
void NpThpDiscard(const void*, int);\
void NpThpReset(void);\
void NpThpNoteName(const char*);\
void NpThpLogBadFrameSize(unsigned long, unsigned long, long);\
void NpThpLogPastEnd(unsigned long, unsigned long, unsigned long, long);\
}\
static unsigned long NpThpFrameBytes[10];\
static inline int NpThpPastEnd(nlFile* file, unsigned long offset, unsigned long size, long frame)\
{\
    unsigned int rounded = 0;\
    file->FileSize(&rounded);\
    if (offset + size <= (unsigned long)rounded)\
        return 0;\
    NpThpLogPastEnd(offset, size, rounded, frame);\
    return 1;\
}
s/^\( *\)memcpy(&SimpleControl\.compInfo, WorkBuffer, sizeof(THPFrameCompInfo));/\1NpThpFixCompInfo(WorkBuffer);\n&/
s/^\( *\)memcpy(&SimpleControl\.videoInfo, WorkBuffer, sizeof(THPVideoInfo));/\1NpThpFixInfo(WorkBuffer, 3);\n&/
s/^\( *\)memcpy(&SimpleControl\.audioInfo, WorkBuffer, sizeof(THPAudioInfo));/\1NpThpFixInfo(WorkBuffer, 4);\n&/
s/^\( *\)memcpy(SimpleControl\.magic, WorkBuffer, sizeof(THPHeader));/\1NpThpFixHeader(WorkBuffer);\n&/
s/^\( *\)SimpleControl\.readSize = \*(u32\*)SimpleControl\.readBuffer\[SimpleControl\.readIndex\]\.ptr;/\1NpThpFixFrame(SimpleControl.readBuffer[SimpleControl.readIndex].ptr, SimpleControl.compInfo.numComponents);\n&\n\1if (SimpleControl.readSize < 16 || (unsigned long)SimpleControl.readSize > (unsigned long)SimpleControl.bufSize)\n\1{\n\1    NpThpLogBadFrameSize(SimpleControl.readSize, SimpleControl.bufSize, SimpleControl.totalReadFrame);\n\1    SimpleControl.numFrames = SimpleControl.totalReadFrame;\n\1}\n\1else if ((unsigned long)SimpleControl.totalReadFrame < SimpleControl.numFrames \&\& NpThpPastEnd(SimpleControl.fileInfo, SimpleControl.curOffset, SimpleControl.readSize, SimpleControl.totalReadFrame))\n\1{\n\1    SimpleControl.numFrames = SimpleControl.totalReadFrame;\n\1}/
s/^\( *\)SimpleControl\.readSize = \*(long\*)SimpleControl\.readBuffer\[idx\]\.ptr;/\1NpThpFixFrame(SimpleControl.readBuffer[idx].ptr, SimpleControl.compInfo.numComponents);\n&/
# Decode-ahead lane (platform/sdk/src/thp_native.cpp): the player decodes through NpThpVideoDecode (with the frame number and the size) and,
# when a frame is done, tells the lane about the next one; opening, closing, preloading or stopping a movie drains the lane first.
s/^\( *\)long ret = THPVideoDecode(videoFrame, SimpleControl\.textureSet\.ytexture, SimpleControl\.textureSet\.utexture, SimpleControl\.textureSet\.vtexture, SimpleControl\.thpWork);/\1long ret = NpThpVideoDecode(videoFrame, SimpleControl.textureSet.ytexture, SimpleControl.textureSet.utexture, SimpleControl.textureSet.vtexture, SimpleControl.thpWork, SimpleControl.readBuffer[SimpleControl.nextDecodeIndex].frameNumber, SimpleControl.videoInfo.xSize, SimpleControl.videoInfo.ySize, (unsigned)NpThpClampComp(videoFrame, videoSize));/
/^extern "C" int THPSimplePreLoad(long loop)$/i\
static inline unsigned long NpThpFrameRoom(long index)\
{\
    unsigned long total = NpThpFrameBytes[index];\
    return total ? total : (unsigned long)SimpleControl.bufSize;\
}\
static inline unsigned long NpThpClampComp(const unsigned char* p, unsigned long size)\
{\
    unsigned long total = NpThpFrameRoom(SimpleControl.nextDecodeIndex);\
    unsigned long off = (unsigned long)(p - SimpleControl.readBuffer[SimpleControl.nextDecodeIndex].ptr);\
    unsigned long room = off <= total ? total - off : 0;\
    return size <= room ? size : room;\
}\
static inline void NpThpPrefetchNextFrames()\
{\
    for (long ahead = 0; ahead < 4; ahead++)\
    {\
        long idx = (SimpleControl.nextDecodeIndex + ahead) % 10;\
        THPReadBuffer* rb = &SimpleControl.readBuffer[idx];\
        if (rb->isValid == 0)\
            return;\
        unsigned long total = NpThpFrameRoom(idx);\
        unsigned long off = SimpleControl.compInfo.numComponents * 4 + 8;\
        if (off > total)\
            continue;\
        unsigned long* cs = (unsigned long*)(rb->ptr + 8);\
        for (unsigned long i = 0; i < SimpleControl.compInfo.numComponents; i++)\
        {\
            if (*cs > total || off + *cs > total)\
                break;\
            if (SimpleControl.compInfo.frameComp[i] == 0)\
            {\
                NpThpPrefetch(rb->ptr + off, (unsigned)*cs, rb->frameNumber, SimpleControl.videoInfo.xSize, SimpleControl.videoInfo.ySize);\
                break;\
            }\
            off += *cs;\
            cs++;\
        }\
    }\
}
s/^\( *\)SimpleControl\.curOffset += SimpleControl\.readSize;$/\1NpThpFrameBytes[SimpleControl.readIndex] = (unsigned long)SimpleControl.readSize;\n&/
s/^\( *\)checkPrefetch();/\1if (decodeVideo) NpThpPrefetchNextFrames();\n&/
/^extern "C" int THPSimpleOpen(const char\* fileName)$/{n;s/{/{\n    NpThpReset();/}
/^extern "C" int THPSimpleClose()$/{n;s/{/{\n    NpThpReset();/}
/^extern "C" int THPSimplePreLoad(long loop)$/{n;s/{/{\n    NpThpReset();/}
/^extern "C" int THPSimpleLoadStop()$/{n;s/{/{\n    NpThpReset();/}
# The player hands the size of the video component to the decoder (the end of a damaged frame) and names the movie in the log.
s/^static inline int VideoDecode(unsigned char\* videoFrame)$/static inline int VideoDecode(unsigned char* videoFrame, unsigned long videoSize)/
s/VideoDecode(ptr)/VideoDecode(ptr, *compSizePtr)/
s/^\( *\)SimpleControl\.fileInfo = nlOpen(fileName);$/&\n\1if (SimpleControl.fileInfo)\n\1    NpThpNoteName(fileName);/
# The first frame of the pre-load is decoded while the movie starts.
s/^\( *\)SimpleControl\.preFetchState = 1;$/&\n\1NpThpPrefetchNextFrames();/
# A truncated or damaged file must end the movie, not overrun the read buffers: the size of the next frame is checked where it is read
# (open, pre-load, read callback). An implausible size ends the movie after the frames that are there.
/^    SimpleControl\.readSize = SimpleControl\.firstFrameSize;$/a\
    if (SimpleControl.numFrames == 0 || SimpleControl.bufSize < 16 || SimpleControl.bufSize > 0x400000 || SimpleControl.readSize < 16 || (unsigned long)SimpleControl.readSize > (unsigned long)SimpleControl.bufSize)\
    {\
        NpThpLogBadFrameSize(SimpleControl.readSize, SimpleControl.bufSize, 0);\
        nlClose(SimpleControl.fileInfo);\
        SimpleControl.fileInfo = NULL;\
        return 0;\
    }\
    if (NpThpPastEnd(SimpleControl.fileInfo, SimpleControl.movieDataOffsets, SimpleControl.readSize, 0))\
    {\
        nlClose(SimpleControl.fileInfo);\
        SimpleControl.fileInfo = NULL;\
        return 0;\
    }
/^            if ((unsigned long)SimpleControl\.totalReadFrame > SimpleControl\.numFrames - 1)$/i\
            if (SimpleControl.readSize < 16 || (unsigned long)SimpleControl.readSize > (unsigned long)SimpleControl.bufSize)\
            {\
                NpThpLogBadFrameSize(SimpleControl.readSize, SimpleControl.bufSize, SimpleControl.totalReadFrame);\
                SimpleControl.numFrames = SimpleControl.totalReadFrame;\
                break;\
            }\
            if ((unsigned long)SimpleControl.totalReadFrame < SimpleControl.numFrames && NpThpPastEnd(SimpleControl.fileInfo, SimpleControl.curOffset, SimpleControl.readSize, SimpleControl.totalReadFrame))\
            {\
                SimpleControl.numFrames = SimpleControl.totalReadFrame;\
                break;\
            }
