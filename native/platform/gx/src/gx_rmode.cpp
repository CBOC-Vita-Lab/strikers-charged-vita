// GX glue, part 8: the SDK's render mode tables (data, GXFrameBuf.c of the decompilation). The game picks one by
// the TV format and hands it to GXAdjustForOverscan / VIConfigure; only the efb / xfb sizes matter natively.
#include <revolution/gx.h>

extern "C" {

GXRenderModeObj GXNtsc480IntDf = {
    0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 8, 8, 10, 12, 10, 8, 8 }
};

GXRenderModeObj GXNtsc480Prog = {
    2, 640, 480, 480, 40, 0, 640, 480, 0, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 0, 0, 21, 22, 21, 0, 0 }
};

GXRenderModeObj GXNtsc480ProgSoft = {
    2, 640, 480, 480, 40, 0, 640, 480, 0, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 8, 8, 10, 12, 10, 8, 8 }
};

GXRenderModeObj GXMpal480IntDf = {
    8, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 8, 8, 10, 12, 10, 8, 8 }
};

GXRenderModeObj GXPal528IntDf = {
    4, 640, 528, 528, 40, 23, 640, 528, 1, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 8, 8, 10, 12, 10, 8, 8 }
};

GXRenderModeObj GXEurgb60Hz480IntDf = {
    20, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 8, 8, 10, 12, 10, 8, 8 }
};

GXRenderModeObj GXEurgb60Hz480Prog = {
    22, 640, 480, 480, 40, 0, 640, 480, 0, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 0, 0, 21, 22, 21, 0, 0 }
};

GXRenderModeObj GXEurgb60Hz480ProgSoft = {
    22, 640, 480, 480, 40, 0, 640, 480, 0, 0, 0, { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, { 8, 8, 10, 12, 10, 8, 8 }
};

} // extern "C"
