// With no platform input source installed (a plain host run) player 1 has an untouched controller, the others are
// not connected; NP_PAD_PRESENT=0 (set before the first read) removes it.
#include <revolution/kpad.h>
#include <revolution/os.h>
#include <revolution/wpad.h>

#include <stdio.h>
#include <stdlib.h>

#include "np/np_pad.h"
#include "np/np_sdk.h"

static int g_fail = 0;
#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static void* alloc(u32) { return 0; }
static BOOL freefn(void*) { return TRUE; }

int main(int argc, char**) {
    NpSdkConfig cfg;
    NpSdk_ConfigDefaults(&cfg);
    NpSdk_Init(&cfg);
    WPADRegisterAllocator(alloc, freefn);
    KPADInit();
    WPADDeviceType dev;
    if (argc > 1) {   // run with the variable set by the test driver
        CHECK(WPADProbe(0, &dev) == WPAD_ERR_NO_CONTROLLER);
    } else {
        CHECK(WPADProbe(0, &dev) == WPAD_ERR_OK && dev == WPAD_DEV_FREESTYLE);
        CHECK(WPADProbe(1, &dev) == WPAD_ERR_NO_CONTROLLER);
        KPADStatus k[1];
        OSSleepTicks(OSMillisecondsToTicks(5));
        CHECK(KPADRead(0, k, 1) == 1);
        printf("  hold 0x%08x\n", (unsigned)k[0].hold);
        CHECK((k[0].hold & ~(unsigned)(NP_GC_TAG | (3u << NP_GC_CHAN_SHIFT))) == 0);   // untouched: only the player tag
    }
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_input_default: ok\n");
    return 0;
}
